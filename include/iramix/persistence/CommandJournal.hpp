#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace iramix::persistence {

struct JournalCommand final {
    std::uint64_t sequence {0U};
    std::vector<std::byte> payload;
};

struct JournalRecoveryResult final {
    std::vector<JournalCommand> commands;
    std::uint64_t validBytes {0U};
    bool discardedInvalidTail {false};
    std::string error;

    [[nodiscard]] bool ok() const noexcept {
        return error.empty();
    }
};

[[nodiscard]] JournalRecoveryResult recoverCommandJournal(
    const std::filesystem::path& path
);

class CommandJournal final {
public:
    explicit CommandJournal(std::filesystem::path path);

    CommandJournal(const CommandJournal&) = delete;
    CommandJournal& operator=(const CommandJournal&) = delete;

    // Durable append is the persistent-command ACK boundary.
    [[nodiscard]] bool append(
        std::uint64_t sequence,
        std::span<const std::byte> payload,
        std::string& error
    );

    [[nodiscard]] std::uint64_t lastSequence() const noexcept {
        return lastSequence_;
    }

private:
    [[nodiscard]] bool initialize(std::string& error);

    std::filesystem::path path_;
    std::uint64_t lastSequence_ {0U};
    bool initialized_ {false};
};

} // namespace iramix::persistence
