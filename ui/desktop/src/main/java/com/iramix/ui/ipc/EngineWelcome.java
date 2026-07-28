package com.iramix.ui.ipc;

import java.util.Arrays;
import java.util.List;
import java.util.Objects;

public record EngineWelcome(
    int protocolVersion,
    String operatingSystem,
    List<String> capabilities
) {
    public EngineWelcome {
        Objects.requireNonNull(operatingSystem, "operatingSystem");
        capabilities = List.copyOf(capabilities);
        if (operatingSystem.isBlank()) {
            throw new IllegalArgumentException("Operating system is blank.");
        }
    }

    public static EngineWelcome fromMessage(IpcMessage message) {
        Objects.requireNonNull(message, "message");
        if (message.type() != IpcMessage.Type.WELCOME) {
            throw new IllegalArgumentException(
                "Expected WELCOME, received " + message.type() + "."
            );
        }

        String operatingSystem = "";
        List<String> capabilities = List.of();
        for (var field : message.payload().split(";")) {
            var pair = field.split("=", 2);
            if (pair.length != 2) {
                continue;
            }
            if ("os".equals(pair[0])) {
                operatingSystem = pair[1];
            } else if ("capabilities".equals(pair[0])) {
                capabilities = pair[1].isBlank()
                    ? List.of()
                    : Arrays.asList(pair[1].split(","));
            }
        }
        return new EngineWelcome(
            message.version(),
            operatingSystem,
            capabilities
        );
    }
}
