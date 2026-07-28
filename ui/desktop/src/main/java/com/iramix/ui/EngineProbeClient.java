package com.iramix.ui;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.time.Duration;
import java.util.concurrent.TimeUnit;

public final class EngineProbeClient {
    private static final Duration TIMEOUT = Duration.ofSeconds(5);

    private EngineProbeClient() {}

    public static EngineHandshake launch(Path executable)
        throws IOException, InterruptedException {
        var process = new ProcessBuilder(
            executable.toAbsolutePath().toString(),
            "--handshake"
        ).redirectErrorStream(true).start();

        var exited = process.waitFor(TIMEOUT.toMillis(), TimeUnit.MILLISECONDS);
        if (!exited) {
            process.destroyForcibly();
            throw new IOException("Engine handshake timed out.");
        }

        try (
            var reader = new BufferedReader(
                new InputStreamReader(
                    process.getInputStream(),
                    StandardCharsets.UTF_8
                )
            )
        ) {
            var line = reader.readLine();
            if (process.exitValue() != 0 || line == null) {
                throw new IOException("Engine handshake failed.");
            }
            return EngineHandshake.parse(line);
        }
    }
}

