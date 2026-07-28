package com.iramix.ui;

import java.util.Objects;

public record EngineHandshake(int protocolVersion, String operatingSystem) {
    private static final String PREFIX = "IRAMIX_ENGINE";

    public EngineHandshake {
        if (protocolVersion < 1) {
            throw new IllegalArgumentException("Invalid protocol version.");
        }
        Objects.requireNonNull(operatingSystem, "operatingSystem");
        if (operatingSystem.isBlank()) {
            throw new IllegalArgumentException("Operating system is blank.");
        }
    }

    public static EngineHandshake parse(String line) {
        Objects.requireNonNull(line, "line");
        var fields = line.strip().split(" ", 3);
        if (fields.length != 3 || !PREFIX.equals(fields[0])) {
            throw new IllegalArgumentException("Invalid engine handshake.");
        }
        return new EngineHandshake(Integer.parseInt(fields[1]), fields[2]);
    }
}

