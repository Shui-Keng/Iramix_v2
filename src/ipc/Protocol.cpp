#include "iramix/ipc/Protocol.hpp"

#include <array>
#include <cstddef>
#include <istream>
#include <limits>
#include <ostream>

namespace iramix::ipc {
namespace {

constexpr std::size_t kHeaderBytes = 20U;

void store16(
    std::array<std::uint8_t, kHeaderBytes>& buffer,
    const std::size_t offset,
    const std::uint16_t value
) {
    buffer[offset] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    buffer[offset + 1U] = static_cast<std::uint8_t>(value & 0xFFU);
}

void store32(
    std::array<std::uint8_t, kHeaderBytes>& buffer,
    const std::size_t offset,
    const std::uint32_t value
) {
    buffer[offset] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
    buffer[offset + 1U] =
        static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    buffer[offset + 2U] =
        static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    buffer[offset + 3U] = static_cast<std::uint8_t>(value & 0xFFU);
}

void store64(
    std::array<std::uint8_t, kHeaderBytes>& buffer,
    const std::size_t offset,
    const std::uint64_t value
) {
    for (std::size_t index = 0U; index < 8U; ++index) {
        const auto shift = static_cast<unsigned int>((7U - index) * 8U);
        buffer[offset + index] =
            static_cast<std::uint8_t>((value >> shift) & 0xFFU);
    }
}

[[nodiscard]] std::uint16_t load16(
    const std::array<std::uint8_t, kHeaderBytes>& buffer,
    const std::size_t offset
) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(buffer[offset]) << 8U)
        | static_cast<std::uint16_t>(buffer[offset + 1U])
    );
}

[[nodiscard]] std::uint32_t load32(
    const std::array<std::uint8_t, kHeaderBytes>& buffer,
    const std::size_t offset
) {
    return (static_cast<std::uint32_t>(buffer[offset]) << 24U)
        | (static_cast<std::uint32_t>(buffer[offset + 1U]) << 16U)
        | (static_cast<std::uint32_t>(buffer[offset + 2U]) << 8U)
        | static_cast<std::uint32_t>(buffer[offset + 3U]);
}

[[nodiscard]] std::uint64_t load64(
    const std::array<std::uint8_t, kHeaderBytes>& buffer,
    const std::size_t offset
) {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
        value = (value << 8U)
            | static_cast<std::uint64_t>(buffer[offset + index]);
    }
    return value;
}

} // namespace

bool readMessage(
    std::istream& input,
    Message& message,
    std::string& error
) {
    std::array<std::uint8_t, kHeaderBytes> header {};
    input.read(
        reinterpret_cast<char*>(header.data()),
        static_cast<std::streamsize>(header.size())
    );
    if (input.gcount() != static_cast<std::streamsize>(header.size())) {
        error = input.eof() ? "end of stream" : "incomplete message header";
        return false;
    }

    if (load32(header, 0U) != kMagic) {
        error = "invalid message magic";
        return false;
    }

    const auto payloadBytes = load32(header, 8U);
    if (payloadBytes > kMaximumPayloadBytes) {
        error = "message payload exceeds limit";
        return false;
    }

    message.version = load16(header, 4U);
    message.type = static_cast<MessageType>(load16(header, 6U));
    message.sequence = load64(header, 12U);
    message.payload.resize(payloadBytes);

    if (payloadBytes != 0U) {
        input.read(
            message.payload.data(),
            static_cast<std::streamsize>(payloadBytes)
        );
        if (input.gcount() != static_cast<std::streamsize>(payloadBytes)) {
            error = "incomplete message payload";
            return false;
        }
    }

    error.clear();
    return true;
}

bool writeMessage(
    std::ostream& output,
    const Message& message,
    std::string& error
) {
    if (message.payload.size() > kMaximumPayloadBytes
        || message.payload.size()
            > static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max()
            )) {
        error = "message payload exceeds limit";
        return false;
    }

    std::array<std::uint8_t, kHeaderBytes> header {};
    store32(header, 0U, kMagic);
    store16(header, 4U, message.version);
    store16(header, 6U, static_cast<std::uint16_t>(message.type));
    store32(
        header,
        8U,
        static_cast<std::uint32_t>(message.payload.size())
    );
    store64(header, 12U, message.sequence);

    output.write(
        reinterpret_cast<const char*>(header.data()),
        static_cast<std::streamsize>(header.size())
    );
    output.write(
        message.payload.data(),
        static_cast<std::streamsize>(message.payload.size())
    );
    output.flush();

    if (!output.good()) {
        error = "failed to write message";
        return false;
    }

    error.clear();
    return true;
}

} // namespace iramix::ipc
