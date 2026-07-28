package com.iramix.ui.tools;

import com.iramix.ui.ipc.EngineSession;
import com.iramix.ui.ipc.IpcMessage;
import com.iramix.ui.ipc.IpcProtocol;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.nio.file.Files;
import java.nio.file.Path;

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
            project
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
            if (!welcome.capabilities().contains("save_session")) {
                throw new AssertionError(
                    "Engine did not advertise session save."
                );
            }
            var saved = session.saveSessionAsync(1).get();
            if (
                saved.revision() != 1
                || saved.serializedBytes() == 0
                || saved.serializationNanoseconds() == 0
                || saved.durableSaveNanoseconds() == 0
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
                    + saved.durableSaveNanoseconds() + ")."
            );
        } finally {
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
