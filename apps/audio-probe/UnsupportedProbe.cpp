#include "AudioProbe.hpp"

#include <cstdint>
#include <iostream>

namespace iramix::audio_probe {

int run(const std::uint32_t) {
    std::cerr
        << "Audio callback probe is pending on this operating system.\n";
    return 3;
}

DeviceInventory enumerateDevices() {
    return {
        .supported = false,
        .devices = {},
        .error = "device enumeration is pending on this operating "
                 "system",
    };
}

int runRestoredDevice(
    const std::filesystem::path&,
    const std::uint32_t
) {
    std::cerr
        << "Device restoration is pending on this operating system.\n";
    return 3;
}

} // namespace iramix::audio_probe
