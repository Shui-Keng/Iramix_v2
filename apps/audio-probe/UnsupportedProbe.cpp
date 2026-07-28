#include "AudioProbe.hpp"

#include <cstdint>
#include <iostream>

namespace iramix::audio_probe {

int run(const std::uint32_t) {
    std::cerr
        << "Audio callback probe is pending on this operating system.\n";
    return 3;
}

} // namespace iramix::audio_probe
