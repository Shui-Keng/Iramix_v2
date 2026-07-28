package com.iramix.ui.ipc;

import java.util.Objects;

public record IpcMessage(
    int version,
    Type type,
    long sequence,
    String payload
) {
    public IpcMessage {
        if (version < 1 || version > 0xFFFF) {
            throw new IllegalArgumentException("Invalid protocol version.");
        }
        Objects.requireNonNull(type, "type");
        Objects.requireNonNull(payload, "payload");
    }

    public enum Type {
        HELLO(1),
        WELCOME(2),
        PING(3),
        ACKNOWLEDGEMENT(4),
        SHUTDOWN(5),
        REJECT(6);

        private final int wireValue;

        Type(int wireValue) {
            this.wireValue = wireValue;
        }

        public int wireValue() {
            return wireValue;
        }

        public static Type fromWireValue(int value) {
            for (var type : values()) {
                if (type.wireValue == value) {
                    return type;
                }
            }
            throw new IllegalArgumentException(
                "Unknown IPC message type: " + value
            );
        }
    }
}
