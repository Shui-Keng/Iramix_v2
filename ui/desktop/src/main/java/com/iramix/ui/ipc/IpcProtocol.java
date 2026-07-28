package com.iramix.ui.ipc;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Objects;

public final class IpcProtocol {
    public static final int VERSION = 1;
    public static final int MAXIMUM_PAYLOAD_BYTES = 64 * 1024;

    private static final int MAGIC = 0x4952414D;

    private IpcProtocol() {}

    public static IpcMessage read(InputStream input) throws IOException {
        Objects.requireNonNull(input, "input");
        var wire = new DataInputStream(input);
        if (wire.readInt() != MAGIC) {
            throw new IOException("Invalid IPC message magic.");
        }

        var version = wire.readUnsignedShort();
        final IpcMessage.Type type;
        try {
            type = IpcMessage.Type.fromWireValue(wire.readUnsignedShort());
        } catch (IllegalArgumentException exception) {
            throw new IOException("Unknown IPC message type.", exception);
        }

        var payloadLength = wire.readInt();
        if (
            payloadLength < 0
            || payloadLength > MAXIMUM_PAYLOAD_BYTES
        ) {
            throw new IOException("IPC payload exceeds the size limit.");
        }
        var sequence = wire.readLong();
        var payloadBytes = wire.readNBytes(payloadLength);
        if (payloadBytes.length != payloadLength) {
            throw new IOException("Incomplete IPC message payload.");
        }
        var payload = new String(payloadBytes, StandardCharsets.UTF_8);

        return new IpcMessage(version, type, sequence, payload);
    }

    public static void write(OutputStream output, IpcMessage message)
        throws IOException {
        Objects.requireNonNull(output, "output");
        Objects.requireNonNull(message, "message");

        var payload = message.payload().getBytes(StandardCharsets.UTF_8);
        if (payload.length > MAXIMUM_PAYLOAD_BYTES) {
            throw new IOException("IPC payload exceeds the size limit.");
        }

        var wire = new DataOutputStream(output);
        wire.writeInt(MAGIC);
        wire.writeShort(message.version());
        wire.writeShort(message.type().wireValue());
        wire.writeInt(payload.length);
        wire.writeLong(message.sequence());
        wire.write(payload);
        wire.flush();
    }
}
