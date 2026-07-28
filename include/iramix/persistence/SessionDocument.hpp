#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace iramix::persistence {

inline constexpr std::uint32_t currentSessionSchemaVersion = 3U;

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

struct SessionClip final {
    std::uint64_t stableId {0U};
    std::uint64_t trackId {0U};
    std::uint64_t sourceId {0U};
    std::uint64_t startFrame {0U};
    std::uint64_t lengthFrames {0U};
    std::uint64_t sourceOffsetFrames {0U};
    float gain {1.0F};
    bool muted {false};
    std::string name;
};

struct SessionRoute final {
    std::uint64_t stableId {0U};
    std::uint64_t sourceTrackId {0U};
    std::uint64_t destinationTrackId {0U};
    float gain {1.0F};
    bool enabled {true};
};

enum class SessionParameterId : std::uint32_t {
    gain = 1U,
    pan = 2U,
    mute = 3U,
};

struct SessionAutomationPoint final {
    std::uint64_t samplePosition {0U};
    float value {0.0F};
};

struct SessionAutomationLane final {
    std::uint64_t stableId {0U};
    std::uint64_t targetTrackId {0U};
    SessionParameterId parameter {SessionParameterId::gain};
    std::vector<SessionAutomationPoint> points;
};

struct SessionDocument final {
    std::uint64_t revision {0U};
    std::uint32_t sampleRate {48'000U};
    double tempo {120.0};
    std::vector<SessionTrack> tracks;
    std::vector<SessionClip> clips;
    std::vector<SessionRoute> routes;
    std::vector<SessionAutomationLane> automationLanes;
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
