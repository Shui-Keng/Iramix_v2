#include "iramix/audio/DeviceBufferConversion.hpp"

#include <algorithm>
#include <cstddef>

namespace iramix::audio {

void interleaveFloat32(
    const ConstAudioBufferView source,
    float* const destination
) noexcept {
    if (destination == nullptr) {
        return;
    }
    for (int frame = 0; frame < source.frameCount(); ++frame) {
        for (int channel = 0;
             channel < source.channelCount();
             ++channel) {
            destination[
                static_cast<std::size_t>(frame)
                    * static_cast<std::size_t>(source.channelCount())
                    + static_cast<std::size_t>(channel)
            ] = source.channel(channel)[frame];
        }
    }
}

void interleavePcm16(
    const ConstAudioBufferView source,
    std::int16_t* const destination
) noexcept {
    if (destination == nullptr) {
        return;
    }
    for (int frame = 0; frame < source.frameCount(); ++frame) {
        for (int channel = 0;
             channel < source.channelCount();
             ++channel) {
            const float sample = std::clamp(
                source.channel(channel)[frame],
                -1.0F,
                1.0F
            );
            destination[
                static_cast<std::size_t>(frame)
                    * static_cast<std::size_t>(source.channelCount())
                    + static_cast<std::size_t>(channel)
            ] = static_cast<std::int16_t>(sample * 32'767.0F);
        }
    }
}

} // namespace iramix::audio
