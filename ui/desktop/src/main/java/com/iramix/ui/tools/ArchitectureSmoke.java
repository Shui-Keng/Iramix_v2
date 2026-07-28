package com.iramix.ui.tools;

import com.iramix.ui.ipc.EngineSession;
import com.iramix.ui.ipc.IpcMessage;
import com.iramix.ui.ipc.IpcProtocol;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;

public final class ArchitectureSmoke {
    private ArchitectureSmoke() {}

    public static void main(String[] args) throws Exception {
        verifyCodec();

        var engineProbe = System.getenv("IRAMIX_ENGINE_PROBE");
        if (engineProbe == null || engineProbe.isBlank()) {
            System.out.println(
                "Java IPC codec smoke test passed; engine probe not supplied."
            );
            return;
        }

        var temporary = Files.createTempDirectory(
            "iramix-ipc-save-smoke-"
        );
        var project = temporary.resolve("session.irpx");
        try (var session = EngineSession.launch(
            Path.of(engineProbe),
            project,
            Duration.ofMillis(50)
        )) {
            var welcome = session.welcome();
            if (welcome.protocolVersion() != IpcProtocol.VERSION) {
                throw new AssertionError(
                    "Protocol version did not round-trip."
                );
            }
            if (!welcome.capabilities().contains("ping")) {
                throw new AssertionError("Engine did not advertise PING.");
            }
            session.ping();
            var revision = session.currentSessionRevision();
            if (revision != 1) {
                throw new AssertionError(
                    "Engine session did not start at revision 1."
                );
            }
            revision = session.setTempo(revision, 132.0);
            if (revision != 2) {
                throw new AssertionError(
                    "Revisioned tempo edit did not apply."
                );
            }
            revision = session.undo(revision);
            if (revision != 3) {
                throw new AssertionError(
                    "Revisioned undo did not apply."
                );
            }
            revision = session.redo(revision);
            if (revision != 4) {
                throw new AssertionError(
                    "Revisioned redo did not apply."
                );
            }
            var autosaved = session.awaitAutosave(revision);
            if (
                autosaved.revision() != revision
                || autosaved.serializedBytes() == 0
            ) {
                throw new AssertionError(
                    "Timed autosave did not commit revision 4."
                );
            }
            if (!welcome.capabilities().contains("save_session")) {
                throw new AssertionError(
                    "Engine did not advertise session save."
                );
            }
            var firstSave = session.saveSessionAsync(revision);
            revision = session.setTempo(revision, 133.0);
            var latestSave = session.saveSessionAsync(revision);
            var saved = latestSave.get();
            var covered = firstSave.get();
            if (
                saved.revision() != revision
                || covered.revision() < 2
                || saved.serializedBytes() == 0
                || saved.serializationNanoseconds() == 0
                || saved.durableSaveNanoseconds() == 0
                || !"committed".equals(saved.backupStatus())
                || saved.backupNanoseconds() == 0
                || saved.backupRetainedCount() == 0
                || !Files.isRegularFile(project)
            ) {
                throw new AssertionError(
                    "Session save did not durably round-trip."
                );
            }
            System.out.println(
                "Persistent Java/C++ IPC and background save passed on "
                    + welcome.operatingSystem() + " (bytes="
                    + saved.serializedBytes() + ", serialize_ns="
                    + saved.serializationNanoseconds() + ", save_ns="
                    + saved.durableSaveNanoseconds()
                    + ", backup_status=" + saved.backupStatus()
                    + ", backup_ns=" + saved.backupNanoseconds()
                    + ", backup_retained="
                    + saved.backupRetainedCount()
                    + ", production_revision=" + revision
                    + ", covered_revision=" + covered.revision()
                    + ")."
            );
        }
        Files.writeString(
            project,
            "corrupt active project fixture",
            java.nio.file.StandardOpenOption.TRUNCATE_EXISTING
        );
        try (var recovered = EngineSession.launch(
            Path.of(engineProbe),
            project,
            Duration.ofMillis(50)
        )) {
            var revision = recovered.currentSessionRevision();
            if (revision != 5) {
                throw new AssertionError(
                    "Recovered session did not reopen at revision 5."
                );
            }
            revision = recovered.undo(revision);
            if (revision != 6) {
                throw new AssertionError(
                    "Recovered undo history did not advance revision."
                );
            }
            var saved = recovered.saveSession(revision);
            if (saved.revision() != 6) {
                throw new AssertionError(
                    "Recovered undo revision was not durable."
                );
            }
            System.out.println(
                "Automatic backup restore passed after active-project "
                    + "corruption (restored_revision=5, "
                    + "post_restore_revision=6)."
            );
        } finally {
            var backupDirectory = Path.of(project + ".backups");
            if (Files.isDirectory(backupDirectory)) {
                try (var paths = Files.walk(backupDirectory)) {
                    for (var path : paths.sorted(
                        java.util.Comparator.reverseOrder()
                    ).toList()) {
                        Files.deleteIfExists(path);
                    }
                }
            }
            Files.deleteIfExists(
                Path.of(project + ".commands.irjc")
            );
            Files.deleteIfExists(project);
            Files.deleteIfExists(temporary);
        }
    }

    private static void verifyCodec() throws Exception {
        var sent = new IpcMessage(
            IpcProtocol.VERSION,
            IpcMessage.Type.PING,
            42,
            "codec-smoke"
        );
        var output = new ByteArrayOutputStream();
        IpcProtocol.write(output, sent);
        var received = IpcProtocol.read(
            new ByteArrayInputStream(output.toByteArray())
        );
        if (!sent.equals(received)) {
            throw new AssertionError("Java IPC codec did not round-trip.");
        }
    }
}
