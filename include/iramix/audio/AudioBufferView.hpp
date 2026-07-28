#pragma once

#include <cassert>
#include <cstddef>
#include <span>
#include <type_traits>

namespace iramix::audio {

// Non-owning planar floating-point buffer. Views are valid only for the current
// processing call and must never be retained by a node.
class ConstAudioBufferView;

class AudioBufferView final {
public:
    constexpr AudioBufferView() noexcept = default;

    constexpr AudioBufferView(
        float* const* channels,
        const int channelCount,
        const int frameCount
    ) noexcept
        : channels_ {channels},
          channelCount_ {channelCount},
          frameCount_ {frameCount} {}

    [[nodiscard]] constexpr int channelCount() const noexcept {
        return channelCount_;
    }

    [[nodiscard]] constexpr int frameCount() const noexcept {
        return frameCount_;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return channels_ == nullptr
            || channelCount_ <= 0
            || frameCount_ <= 0;
    }

    [[nodiscard]] float* channel(const int index) const noexcept {
        assert(index >= 0 && index < channelCount_);
        return channels_[index];
    }

    [[nodiscard]] std::span<float> channelSpan(
        const int index
    ) const noexcept {
        return {
            channel(index),
            static_cast<std::size_t>(frameCount_),
        };
    }

    [[nodiscard]] constexpr float* const* data() const noexcept {
        return channels_;
    }

    [[nodiscard]] constexpr AudioBufferView firstFrames(
        const int count
    ) const noexcept {
        return {
            channels_,
            channelCount_,
            count < frameCount_ ? count : frameCount_,
        };
    }

    [[nodiscard]] constexpr AudioBufferView firstChannels(
        const int count
    ) const noexcept {
        return {
            channels_,
            count < channelCount_ ? count : channelCount_,
            frameCount_,
        };
    }

    [[nodiscard]] constexpr ConstAudioBufferView asConst() const noexcept;

private:
    float* const* channels_ {nullptr};
    int channelCount_ {0};
    int frameCount_ {0};
};

class ConstAudioBufferView final {
public:
    constexpr ConstAudioBufferView() noexcept = default;

    constexpr ConstAudioBufferView(
        const float* const* channels,
        const int channelCount,
        const int frameCount
    ) noexcept
        : channels_ {channels},
          channelCount_ {channelCount},
          frameCount_ {frameCount} {}

    [[nodiscard]] constexpr int channelCount() const noexcept {
        return channelCount_;
    }

    [[nodiscard]] constexpr int frameCount() const noexcept {
        return frameCount_;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return channels_ == nullptr
            || channelCount_ <= 0
            || frameCount_ <= 0;
    }

    [[nodiscard]] const float* channel(const int index) const noexcept {
        assert(index >= 0 && index < channelCount_);
        return channels_[index];
    }

    [[nodiscard]] std::span<const float> channelSpan(
        const int index
    ) const noexcept {
        return {
            channel(index),
            static_cast<std::size_t>(frameCount_),
        };
    }

    [[nodiscard]] constexpr const float* const* data() const noexcept {
        return channels_;
    }

private:
    const float* const* channels_ {nullptr};
    int channelCount_ {0};
    int frameCount_ {0};
};

constexpr ConstAudioBufferView AudioBufferView::asConst() const noexcept {
    return {
        const_cast<const float* const*>(channels_),
        channelCount_,
        frameCount_,
    };
}

static_assert(std::is_trivially_copyable_v<AudioBufferView>);
static_assert(std::is_trivially_copyable_v<ConstAudioBufferView>);

} // namespace iramix::audio
