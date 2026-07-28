#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

namespace iramix::ipc {

inline constexpr std::uint32_t kMagic = 0x4952414DU;
inline constexpr std::uint16_t kProtocolVersion = 1U;
inline constexpr std::uint32_t kMaximumPayloadBytes = 64U * 1024U;

enum class MessageType : std::uint16_t {
    hello = 1U,
    welcome = 2U,
    ping = 3U,
    acknowledgement = 4U,
    shutdown = 5U,
    reject = 6U,
    saveSession = 7U,
    pollSaveCompletion = 8U,
};

struct Message {
    std::uint16_t version {kProtocolVersion};
    MessageType type {MessageType::reject};
    std::uint64_t sequence {0U};
    std::string payload;
};

[[nodiscard]] bool readMessage(
    std::istream& input,
    Message& message,
    std::string& error
);

[[nodiscard]] bool writeMessage(
    std::ostream& output,
    const Message& message,
    std::string& error
);

} // namespace iramix::ipc
