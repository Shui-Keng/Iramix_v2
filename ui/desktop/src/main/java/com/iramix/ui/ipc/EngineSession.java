package com.iramix.ui.ipc;

import java.io.IOException;
import java.nio.file.Path;
import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.locks.ReentrantLock;

public final class EngineSession implements AutoCloseable {
    private static final Duration TIMEOUT = Duration.ofSeconds(5);

    private final Process process;
    private final ExecutorService readerExecutor;
    private final EngineWelcome welcome;
    private final ReentrantLock exchangeLock = new ReentrantLock();
    private long nextSequence = 2;
    private volatile boolean closed;

    private EngineSession(
        Process process,
        ExecutorService readerExecutor,
        EngineWelcome welcome
    ) {
        this.process = process;
        this.readerExecutor = readerExecutor;
        this.welcome = welcome;
    }

    public static EngineSession launch(Path executable) throws IOException {
        return launch(executable, null);
    }

    public static EngineSession launch(
        Path executable,
        Path projectTarget
    ) throws IOException {
        Objects.requireNonNull(executable, "executable");
        var command = new java.util.ArrayList<String>();
        command.add(executable.toAbsolutePath().toString());
        command.add("--ipc-stdio");
        if (projectTarget != null) {
            command.add("--project");
            command.add(projectTarget.toAbsolutePath().toString());
        }
        var process = new ProcessBuilder(command)
            .redirectError(ProcessBuilder.Redirect.INHERIT)
            .start();
        var executor = Executors.newThreadPerTaskExecutor(
            Thread.ofVirtual().name("iramix-engine-ipc-", 0).factory()
        );

        try {
            var hello = new IpcMessage(
                IpcProtocol.VERSION,
                IpcMessage.Type.HELLO,
                1,
                "client=java-ui"
            );
            IpcProtocol.write(process.getOutputStream(), hello);
            var response = readWithTimeout(process, executor);
            if (response.sequence() != hello.sequence()) {
                throw new IOException("Engine WELCOME sequence mismatch.");
            }
            if (response.type() == IpcMessage.Type.REJECT) {
                throw new IOException(
                    "Engine rejected HELLO: " + response.payload()
                );
            }
            return new EngineSession(
                process,
                executor,
                EngineWelcome.fromMessage(response)
            );
        } catch (IOException exception) {
            executor.shutdownNow();
            process.destroyForcibly();
            throw exception;
        }
    }

    public EngineWelcome welcome() {
        return welcome;
    }

    public Duration ping() throws IOException {
        var started = System.nanoTime();
        var response = exchangeNew(
            IpcMessage.Type.PING,
            ""
        );
        if (
            response.type() != IpcMessage.Type.ACKNOWLEDGEMENT
            || !"pong".equals(response.payload())
        ) {
            throw new IOException("Engine returned an invalid PING response.");
        }
        return Duration.ofNanos(System.nanoTime() - started);
    }

    public long currentSessionRevision() throws IOException {
        var response = exchangeNew(
            IpcMessage.Type.SESSION_STATE,
            ""
        );
        return parseRequiredLong(
            response.payload(),
            "revision",
            "session state"
        );
    }

    public long setTempo(
        long expectedRevision,
        double tempo
    ) throws IOException {
        if (
            expectedRevision <= 0
            || !Double.isFinite(tempo)
            || tempo <= 0.0
            || tempo > 1_000.0
        ) {
            throw new IllegalArgumentException("Invalid tempo edit.");
        }
        var response = exchangeNew(
            IpcMessage.Type.SET_TEMPO,
            "expected_revision=" + expectedRevision
                + ";tempo=" + tempo
        );
        return parseRequiredLong(
            response.payload(),
            "revision",
            "tempo edit"
        );
    }

    public SessionSaveResult saveSession(long revision)
        throws IOException {
        requestSessionSave(revision);
        return awaitSessionSave(revision);
    }

    public CompletableFuture<SessionSaveResult> saveSessionAsync(
        long revision
    ) {
        try {
            requestSessionSave(revision);
        } catch (IOException exception) {
            return CompletableFuture.failedFuture(exception);
        }
        return CompletableFuture.supplyAsync(
            () -> {
                try {
                    return awaitSessionSave(revision);
                } catch (IOException exception) {
                    throw new CompletionException(exception);
                }
            },
            Thread::startVirtualThread
        );
    }

    private void requestSessionSave(long revision)
        throws IOException {
        if (revision <= 0) {
            throw new IllegalArgumentException(
                "Session revision must be positive."
            );
        }
        var response = exchangeNew(
            IpcMessage.Type.SAVE_SESSION,
            "revision=" + revision
        );
        if (
            response.type() != IpcMessage.Type.ACKNOWLEDGEMENT
            || !(response.payload().equals(
                    "accepted;revision=" + revision
                )
                || response.payload().equals(
                    "coalesced;revision=" + revision
                ))
        ) {
            throw new IOException(
                "Engine returned invalid SAVE_SESSION ACK."
            );
        }
    }

    private SessionSaveResult awaitSessionSave(long revision)
        throws IOException {
        var deadline = System.nanoTime() + TIMEOUT.toNanos();
        while (System.nanoTime() < deadline) {
            var completion = exchangeNew(
                IpcMessage.Type.POLL_SAVE_COMPLETION,
                "revision=" + revision
            );
            if (!"none".equals(completion.payload())) {
                return parseSaveCompletion(
                    completion.payload(),
                    revision
                );
            }
            try {
                Thread.sleep(Duration.ofMillis(1));
            } catch (InterruptedException exception) {
                Thread.currentThread().interrupt();
                throw new IOException(
                    "Interrupted while waiting for session save.",
                    exception
                );
            }
        }
        throw new IOException("Session save completion timed out.");
    }

    @Override
    public void close() throws IOException {
        IOException failure = null;
        exchangeLock.lock();
        try {
            if (closed) {
                return;
            }
            closed = true;
            var request = new IpcMessage(
                IpcProtocol.VERSION,
                IpcMessage.Type.SHUTDOWN,
                nextSequence++,
                ""
            );
            var response = exchangeWhileClosing(request);
            if (
                response.type() != IpcMessage.Type.ACKNOWLEDGEMENT
                || !"shutdown".equals(response.payload())
            ) {
                throw new IOException(
                    "Engine returned an invalid SHUTDOWN response."
                );
            }
            if (
                !process.waitFor(
                    TIMEOUT.toMillis(),
                    TimeUnit.MILLISECONDS
                )
            ) {
                throw new IOException("Engine did not shut down in time.");
            }
        } catch (InterruptedException exception) {
            Thread.currentThread().interrupt();
            failure = new IOException(
                "Interrupted while stopping the engine.",
                exception
            );
        } catch (IOException exception) {
            failure = exception;
        } finally {
            exchangeLock.unlock();
            readerExecutor.shutdownNow();
            if (process.isAlive()) {
                process.destroyForcibly();
            }
        }

        if (failure != null) {
            throw failure;
        }
    }

    private IpcMessage exchangeNew(
        IpcMessage.Type type,
        String payload
    ) throws IOException {
        exchangeLock.lock();
        try {
            ensureOpen();
            var request = new IpcMessage(
                IpcProtocol.VERSION,
                type,
                nextSequence++,
                payload
            );
            IpcProtocol.write(process.getOutputStream(), request);
            var response = readWithTimeout(process, readerExecutor);
            if (response.sequence() != request.sequence()) {
                throw sequenceMismatch(request, response);
            }
            if (response.type() == IpcMessage.Type.REJECT) {
                throw new IOException(
                    "Engine rejected message: " + response.payload()
                );
            }
            return response;
        } finally {
            exchangeLock.unlock();
        }
    }

    private IpcMessage exchangeWhileClosing(IpcMessage request)
        throws IOException {
        IpcProtocol.write(process.getOutputStream(), request);
        var response = readWithTimeout(process, readerExecutor);
        if (response.sequence() != request.sequence()) {
            throw sequenceMismatch(request, response);
        }
        return response;
    }

    private static IpcMessage readWithTimeout(
        Process process,
        ExecutorService executor
    ) throws IOException {
        var pending = executor.submit(
            () -> IpcProtocol.read(process.getInputStream())
        );
        try {
            return pending.get(TIMEOUT.toMillis(), TimeUnit.MILLISECONDS);
        } catch (TimeoutException exception) {
            pending.cancel(true);
            process.destroyForcibly();
            throw new IOException("Engine IPC response timed out.", exception);
        } catch (InterruptedException exception) {
            pending.cancel(true);
            Thread.currentThread().interrupt();
            throw new IOException(
                "Interrupted while waiting for the engine.",
                exception
            );
        } catch (ExecutionException exception) {
            var cause = exception.getCause();
            if (cause instanceof IOException ioException) {
                throw ioException;
            }
            throw new IOException("Engine IPC read failed.", cause);
        }
    }

    private void ensureOpen() throws IOException {
        if (closed || !process.isAlive()) {
            throw new IOException("Engine session is closed.");
        }
    }

    private static IOException sequenceMismatch(
        IpcMessage request,
        IpcMessage response
    ) {
        return new IOException(
            "Engine response sequence mismatch: expected "
                + request.sequence()
                + ", received "
                + response.sequence()
                + " (type="
                + response.type()
                + ", payload="
                + response.payload()
                + ")."
        );
    }

    private static SessionSaveResult parseSaveCompletion(
        String payload,
        long expectedRevision
    ) throws IOException {
        var fields = new java.util.HashMap<String, String>();
        var parts = payload.split(";");
        for (int index = 1; index < parts.length; ++index) {
            var separator = parts[index].indexOf('=');
            if (separator > 0) {
                fields.put(
                    parts[index].substring(0, separator),
                    parts[index].substring(separator + 1)
                );
            }
        }
        if ("failed".equals(parts[0])) {
            throw new IOException(
                "Engine session save failed: "
                    + fields.getOrDefault("detail", "unknown failure")
            );
        }
        if (!"committed".equals(parts[0])) {
            throw new IOException(
                "Unknown session save completion: " + payload
            );
        }
        try {
            var revision = Long.parseLong(fields.get("revision"));
            if (revision < expectedRevision) {
                throw new IOException(
                    "Session save revision regressed: expected at least "
                        + expectedRevision + ", received " + revision
                );
            }
            return new SessionSaveResult(
                revision,
                Long.parseLong(fields.get("bytes")),
                Long.parseLong(fields.get("serialize_ns")),
                Long.parseLong(fields.get("save_ns"))
            );
        } catch (NullPointerException | NumberFormatException exception) {
            throw new IOException(
                "Malformed session save completion: " + payload,
                exception
            );
        }
    }

    private static long parseRequiredLong(
        String payload,
        String field,
        String context
    ) throws IOException {
        var fields = new java.util.HashMap<String, String>();
        for (var part : payload.split(";")) {
            var separator = part.indexOf('=');
            if (separator > 0) {
                fields.put(
                    part.substring(0, separator),
                    part.substring(separator + 1)
                );
            }
        }
        try {
            return Long.parseLong(fields.get(field));
        } catch (NullPointerException | NumberFormatException exception) {
            throw new IOException(
                "Malformed " + context + " response: " + payload,
                exception
            );
        }
    }
}
