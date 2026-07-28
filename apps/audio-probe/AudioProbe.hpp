#pragma once

#include "iramix/persistence/DeviceResolver.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace iramix::audio_probe {

int run(std::uint32_t secondsPerBuffer);

struct DeviceInventory final {
    // False when this platform's backend has no enumeration implementation
    // yet. An empty inventory with supported=true means the backend ran and
    // genuinely found no active endpoint.
    bool supported {false};
    std::vector<persistence::AvailableAudioDevice> devices;
    std::string error;
};

// Enumerates the backend's active endpoints into the same records the
// session device resolver consumes.
[[nodiscard]] DeviceInventory enumerateDevices();

// Loads a session project, resolves its stored device configuration against
// the enumerated hardware, and runs the probe on whatever the resolver
// selected. Returns non-zero when the session cannot be restored.
[[nodiscard]] int runRestoredDevice(
    const std::filesystem::path& project,
    std::uint32_t secondsPerBuffer
);

} // namespace iramix::audio_probe
