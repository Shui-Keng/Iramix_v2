#pragma once

#include "iramix/audio/AudioBufferView.hpp"

#include <cstdint>

namespace iramix::audio {

void interleaveFloat32(
    ConstAudioBufferView source,
    float* destination
) noexcept;

void interleavePcm16(
    ConstAudioBufferView source,
    std::int16_t* destination
) noexcept;

} // namespace iramix::audio
