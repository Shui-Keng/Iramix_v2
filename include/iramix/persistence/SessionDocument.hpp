#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace iramix::persistence {

inline constexpr std::uint32_t currentSessionSchemaVersion = 2U;

enum class SessionTrackType : std::uint32_t {
    audio = 1U,
    instrument = 2U,
    group = 3U,
    effectReturn = 4U,
    master = 5U,
};

struct SessionTrack final {
    std::uint64_t stableId {0U};
    SessionTrackType type {SessionTrackType::audio};
    float gain {1.0F};
    std::uint32_t color {0U};
    std::string name;
};

struct SessionDocument final {
    std::uint64_t revision {0U};
    std::uint32_t sampleRate {48'000U};
    double tempo {120.0};
    std::vector<SessionTrack> tracks;
};

struct SessionDecodeResult final {
    SessionDocument document;
    std::uint32_t sourceSchemaVersion {0U};
    bool migrated {false};
    std::string error;

    [[nodiscard]] bool ok() const noexcept {
        return error.empty();
    }
};

// targetSchemaVersion exists for deterministic migration fixtures and export
// testing. New project saves should use the default current schema.
[[nodiscard]] std::vector<std::byte> serializeSessionDocument(
    const SessionDocument& document,
    std::string& error,
    std::uint32_t targetSchemaVersion =
        currentSessionSchemaVersion
);

[[nodiscard]] SessionDecodeResult deserializeSessionDocument(
    std::span<const std::byte> bytes
);

} // namespace iramix::persistence
