#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace iramix::persistence {

inline constexpr std::uint32_t currentSessionSchemaVersion = 4U;

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

// External media referenced by clips. A source with an empty path and zero
// audio properties is an unresolved placeholder: it names an identity that a
// clip depends on without claiming the bytes were located. Schema v3
// migration produces exactly that for every clip source it cannot describe.
struct SessionMediaSource final {
    std::uint64_t stableId {0U};
    std::uint64_t contentHash {0U};
    std::uint64_t frameCount {0U};
    std::uint32_t sampleRate {0U};
    std::uint32_t channelCount {0U};
    std::string path;
    std::string name;
};

struct SessionMidiNote final {
    std::uint64_t startFrame {0U};
    std::uint64_t lengthFrames {0U};
    std::uint32_t channel {0U};
    std::uint32_t key {0U};
    float velocity {1.0F};
};

// Sample-domain MIDI, addressed by clips through the same source ID space as
// SessionMediaSource so a clip resolves to exactly one of the two.
struct SessionMidiSequence final {
    std::uint64_t stableId {0U};
    std::string name;
    std::vector<SessionMidiNote> notes;
};

enum class SessionAudioBackend : std::uint32_t {
    unspecified = 1U,
    wasapi = 2U,
    asio = 3U,
    coreAudio = 4U,
    alsa = 5U,
    jack = 6U,
};

// Hardware the session was last driven by. Zero means "ask the backend", so a
// default-constructed configuration restores on unfamiliar hardware.
struct SessionDeviceConfiguration final {
    SessionAudioBackend backend {SessionAudioBackend::unspecified};
    std::uint32_t sampleRate {0U};
    std::uint32_t bufferFrames {0U};
    std::uint32_t inputChannelCount {0U};
    std::uint32_t outputChannelCount {0U};
    std::string inputDeviceId;
    std::string outputDeviceId;
};

enum class SessionPluginFormat : std::uint32_t {
    internal = 1U,
    vst3 = 2U,
    clap = 3U,
};

struct SessionPlugin final {
    std::uint64_t stableId {0U};
    std::uint64_t targetTrackId {0U};
    SessionPluginFormat format {SessionPluginFormat::internal};
    std::uint32_t slotIndex {0U};
    bool bypassed {false};
    std::string identifier;
    std::string name;
    // Opaque to the host: bounded, checksum-protected, never interpreted.
    std::vector<std::byte> state;
};

struct SessionDocument final {
    std::uint64_t revision {0U};
    std::uint32_t sampleRate {48'000U};
    double tempo {120.0};
    std::vector<SessionTrack> tracks;
    std::vector<SessionClip> clips;
    std::vector<SessionRoute> routes;
    std::vector<SessionAutomationLane> automationLanes;
    std::vector<SessionMediaSource> mediaSources;
    std::vector<SessionMidiSequence> midiSequences;
    std::vector<SessionPlugin> plugins;
    SessionDeviceConfiguration device;
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
