#pragma once

#include "iramix/persistence/SessionDocument.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace iramix::persistence {

// One entry of a backend's enumerated hardware. Supplied by the caller so
// resolution stays pure logic: it never touches a device itself, which is
// what makes restoration testable without the hardware present.
struct AvailableAudioDevice final {
    SessionAudioBackend backend {SessionAudioBackend::unspecified};
    std::string deviceId;
    std::string name;
    // Ascending, non-empty for a usable device.
    std::vector<std::uint32_t> supportedSampleRates;
    std::uint32_t minimumBufferFrames {0U};
    std::uint32_t maximumBufferFrames {0U};
    std::uint32_t inputChannelCount {0U};
    std::uint32_t outputChannelCount {0U};
};

enum class DeviceResolutionStatus : std::uint32_t {
    // Every stored field was honored exactly.
    restored = 1U,
    // The stored device opened, but a rate, buffer size, or channel count
    // had to be renegotiated against what the hardware actually offers.
    adjusted = 2U,
    // The stored device is absent; another device on the same backend was
    // selected instead.
    substituted = 3U,
    // The stored backend is not present at all. Nothing is selected: a
    // session must not silently move to a different audio subsystem.
    unavailableBackend = 4U,
    // The session carried no device configuration.
    unconfigured = 5U,
};

struct DeviceResolution final {
    DeviceResolutionStatus status {
        DeviceResolutionStatus::unconfigured
    };
    // The configuration to actually open. Empty/default when status is
    // unavailableBackend or unconfigured, so a caller that ignores the
    // status still cannot open the wrong hardware.
    SessionDeviceConfiguration resolved;
    bool outputDeviceSubstituted {false};
    bool inputDeviceSubstituted {false};
    bool sampleRateAdjusted {false};
    bool bufferFramesAdjusted {false};
    bool channelCountAdjusted {false};
    // Human-readable explanation of every difference from the stored
    // configuration. Empty when status is restored.
    std::string reason;

    [[nodiscard]] bool openable() const noexcept {
        return status == DeviceResolutionStatus::restored
            || status == DeviceResolutionStatus::adjusted
            || status == DeviceResolutionStatus::substituted;
    }
};

// Resolves a stored device configuration against enumerated hardware.
// Inventory order is the caller's preference order: the first entry on a
// backend is that backend's default. Sample-rate renegotiation picks the
// nearest supported rate, preferring the higher one on a tie.
[[nodiscard]] DeviceResolution resolveDeviceConfiguration(
    const SessionDeviceConfiguration& stored,
    const std::vector<AvailableAudioDevice>& inventory
);

} // namespace iramix::persistence
