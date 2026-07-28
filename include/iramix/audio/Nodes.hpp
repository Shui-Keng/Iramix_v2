#pragma once

#include "iramix/audio/Node.hpp"

#include <atomic>

namespace iramix::audio {

// Audio-thread bridge for an external planar input block. bindInput() and
// process() must run on the same audio thread in the same callback. The
// borrowed view is never owned and is cleared by unbindInput().
class DeviceInputNode final : public IAudioNode {
public:
    void prepare(const NodePrepareInfo& info) override;
    void process(const NodeProcessContext& context) noexcept override;

    void bindInput(ConstAudioBufferView input) noexcept;
    void unbindInput() noexcept;

private:
    ConstAudioBufferView input_;
    int outputChannelCount_ {0};
};

// Per-track trim, linear stereo balance, and mute. Parameter changes use
// always-lock-free atomics and become visible at block boundaries.
class TrackNode final : public IAudioNode {
public:
    static constexpr ParameterId kGainParameter = 1U;
    static constexpr ParameterId kPanParameter = 2U;
    static constexpr ParameterId kMuteParameter = 3U;

    void prepare(const NodePrepareInfo& info) override;
    void process(const NodeProcessContext& context) noexcept override;

    void setGain(float linearGain) noexcept;
    [[nodiscard]] float gain() const noexcept;

    void setPan(float pan) noexcept;
    [[nodiscard]] float pan() const noexcept;

    void setMuted(bool muted) noexcept;
    [[nodiscard]] bool muted() const noexcept;

private:
    std::atomic<float> gain_ {1.0F};
    std::atomic<float> pan_ {0.0F};
    std::atomic<std::uint32_t> muted_ {0U};
    ParameterValueState gainState_ {1.0F};
    ParameterValueState panState_ {0.0F};
    int channelCount_ {0};
};

class GainNode final : public IAudioNode {
public:
    static constexpr ParameterId kGainParameter = 1U;

    explicit GainNode(float linearGain = 1.0F) noexcept;

    void prepare(const NodePrepareInfo& info) override;
    void process(const NodeProcessContext& context) noexcept override;

    void setGain(float linearGain) noexcept;
    [[nodiscard]] float gain() const noexcept;

private:
    std::atomic<float> gain_ {1.0F};
    ParameterValueState gainState_ {1.0F};
    int channelCount_ {0};
};

// Audio fan-in is performed by the render plan before this node runs. This
// node owns the mix-bus output trim and is the future insertion point for
// mixer-bus metering.
class MixerNode final : public IAudioNode {
public:
    static constexpr ParameterId kOutputGainParameter = 1U;

    void prepare(const NodePrepareInfo& info) override;
    void process(const NodeProcessContext& context) noexcept override;

    void setOutputGain(float linearGain) noexcept;
    [[nodiscard]] float outputGain() const noexcept;

private:
    std::atomic<float> outputGain_ {1.0F};
    ParameterValueState outputGainState_ {1.0F};
    int channelCount_ {0};
};

// Explicit graph sink. The executor snapshots this node's input channels
// immediately after process() returns.
class OutputNode final : public IAudioNode {
public:
    void prepare(const NodePrepareInfo& info) override;
    void process(const NodeProcessContext& context) noexcept override;

private:
    int inputChannelCount_ {0};
};

static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

} // namespace iramix::audio
