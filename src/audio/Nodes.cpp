#include "iramix/audio/Nodes.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace iramix::audio {
namespace {

void requireValidPrepareInfo(
    const NodePrepareInfo& info,
    const char* const nodeName
) {
    if (info.sampleRate <= 0.0 || info.maxBlockSize <= 0) {
        throw std::invalid_argument {
            std::string {nodeName}
            + " requires a positive sample rate and block size"
        };
    }
}

[[nodiscard]] int requireSymmetricLayout(
    const NodePrepareInfo& info,
    const char* const nodeName
) {
    requireValidPrepareInfo(info, nodeName);
    const int inputs = totalChannels(info.inputBuses);
    const int outputs = totalChannels(info.outputBuses);
    if (inputs <= 0 || inputs != outputs) {
        throw std::invalid_argument {
            std::string {nodeName}
            + " requires equal, non-zero input and output channels"
        };
    }
    return inputs;
}

void clearAudio(const AudioBufferView audio) noexcept {
    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        std::fill_n(
            audio.channel(channel),
            audio.frameCount(),
            0.0F
        );
    }
}

} // namespace

void DeviceInputNode::prepare(const NodePrepareInfo& info) {
    requireValidPrepareInfo(info, "DeviceInputNode");
    if (totalChannels(info.inputBuses) != 0) {
        throw std::invalid_argument {
            "DeviceInputNode cannot have graph input buses"
        };
    }
    outputChannelCount_ = totalChannels(info.outputBuses);
    if (outputChannelCount_ <= 0) {
        throw std::invalid_argument {
            "DeviceInputNode requires at least one output channel"
        };
    }
}

void DeviceInputNode::bindInput(
    const ConstAudioBufferView input
) noexcept {
    input_ = input;
}

void DeviceInputNode::unbindInput() noexcept {
    input_ = {};
}

void DeviceInputNode::process(
    const NodeProcessContext& context
) noexcept {
    clearAudio(context.audio);
    const int outputChannels = std::min(
        context.audio.channelCount(),
        outputChannelCount_
    );
    const int copiedChannels = std::min(
        outputChannels,
        input_.channelCount()
    );
    const int copiedFrames = std::min(
        context.audio.frameCount(),
        input_.frameCount()
    );

    for (int channel = 0; channel < copiedChannels; ++channel) {
        std::memcpy(
            context.audio.channel(channel),
            input_.channel(channel),
            static_cast<std::size_t>(copiedFrames) * sizeof(float)
        );
    }
}

void TrackNode::prepare(const NodePrepareInfo& info) {
    channelCount_ = requireSymmetricLayout(info, "TrackNode");
}

void TrackNode::process(
    const NodeProcessContext& context
) noexcept {
    float currentGain = gain();
    float currentPan = pan();
    bool currentMute = muted();
    int eventIndex = 0;
    const int eventCount = context.parameters != nullptr
        ? context.parameters->eventCount()
        : 0;
    const int channels = std::min(
        context.audio.channelCount(),
        channelCount_
    );

    for (int frame = 0;
         frame < context.audio.frameCount();
         ++frame) {
        while (eventIndex < eventCount) {
            const auto event =
                context.parameters->event(eventIndex);
            if (event.sampleOffset != frame) {
                break;
            }
            if (event.parameter == kGainParameter) {
                currentGain = event.value;
                gain_.store(currentGain, std::memory_order_relaxed);
            } else if (event.parameter == kPanParameter) {
                currentPan = std::clamp(
                    event.value,
                    -1.0F,
                    1.0F
                );
                pan_.store(currentPan, std::memory_order_relaxed);
            } else if (event.parameter == kMuteParameter) {
                currentMute = event.value >= 0.5F;
                muted_.store(
                    currentMute ? 1U : 0U,
                    std::memory_order_relaxed
                );
            }
            ++eventIndex;
        }

        if (currentMute) {
            for (int channel = 0; channel < channels; ++channel) {
                context.audio.channel(channel)[frame] = 0.0F;
            }
            continue;
        }

        const float leftGain = currentGain
            * (currentPan > 0.0F ? 1.0F - currentPan : 1.0F);
        const float rightGain = currentGain
            * (currentPan < 0.0F ? 1.0F + currentPan : 1.0F);
        for (int channel = 0; channel < channels; ++channel) {
            const float channelGain = channel == 0
                ? leftGain
                : (channel == 1 ? rightGain : currentGain);
            context.audio.channel(channel)[frame] *= channelGain;
        }
    }
}

void TrackNode::setGain(const float linearGain) noexcept {
    gain_.store(linearGain, std::memory_order_relaxed);
}

float TrackNode::gain() const noexcept {
    return gain_.load(std::memory_order_relaxed);
}

void TrackNode::setPan(const float pan) noexcept {
    pan_.store(
        std::clamp(pan, -1.0F, 1.0F),
        std::memory_order_relaxed
    );
}

float TrackNode::pan() const noexcept {
    return pan_.load(std::memory_order_relaxed);
}

void TrackNode::setMuted(const bool muted) noexcept {
    muted_.store(muted ? 1U : 0U, std::memory_order_relaxed);
}

bool TrackNode::muted() const noexcept {
    return muted_.load(std::memory_order_relaxed) != 0U;
}

GainNode::GainNode(const float linearGain) noexcept
    : gain_ {linearGain} {}

void GainNode::prepare(const NodePrepareInfo& info) {
    channelCount_ = requireSymmetricLayout(info, "GainNode");
}

void GainNode::process(
    const NodeProcessContext& context
) noexcept {
    float currentGain = gain();
    int eventIndex = 0;
    const int eventCount = context.parameters != nullptr
        ? context.parameters->eventCount()
        : 0;
    const int channels = std::min(
        context.audio.channelCount(),
        channelCount_
    );
    for (int frame = 0;
         frame < context.audio.frameCount();
         ++frame) {
        while (eventIndex < eventCount) {
            const auto event =
                context.parameters->event(eventIndex);
            if (event.sampleOffset != frame) {
                break;
            }
            if (event.parameter == kGainParameter) {
                currentGain = event.value;
                gain_.store(currentGain, std::memory_order_relaxed);
            }
            ++eventIndex;
        }
        for (int channel = 0; channel < channels; ++channel) {
            context.audio.channel(channel)[frame] *= currentGain;
        }
    }
}

void GainNode::setGain(const float linearGain) noexcept {
    gain_.store(linearGain, std::memory_order_relaxed);
}

float GainNode::gain() const noexcept {
    return gain_.load(std::memory_order_relaxed);
}

void MixerNode::prepare(const NodePrepareInfo& info) {
    channelCount_ = requireSymmetricLayout(info, "MixerNode");
}

void MixerNode::process(
    const NodeProcessContext& context
) noexcept {
    float currentGain = outputGain();
    int eventIndex = 0;
    const int eventCount = context.parameters != nullptr
        ? context.parameters->eventCount()
        : 0;
    const int channels = std::min(
        context.audio.channelCount(),
        channelCount_
    );
    for (int frame = 0;
         frame < context.audio.frameCount();
         ++frame) {
        while (eventIndex < eventCount) {
            const auto event =
                context.parameters->event(eventIndex);
            if (event.sampleOffset != frame) {
                break;
            }
            if (event.parameter == kOutputGainParameter) {
                currentGain = event.value;
                outputGain_.store(
                    currentGain,
                    std::memory_order_relaxed
                );
            }
            ++eventIndex;
        }
        for (int channel = 0; channel < channels; ++channel) {
            context.audio.channel(channel)[frame] *= currentGain;
        }
    }
}

void MixerNode::setOutputGain(const float linearGain) noexcept {
    outputGain_.store(linearGain, std::memory_order_relaxed);
}

float MixerNode::outputGain() const noexcept {
    return outputGain_.load(std::memory_order_relaxed);
}

void OutputNode::prepare(const NodePrepareInfo& info) {
    requireValidPrepareInfo(info, "OutputNode");
    inputChannelCount_ = totalChannels(info.inputBuses);
    if (inputChannelCount_ <= 0
        || totalChannels(info.outputBuses) != 0) {
        throw std::invalid_argument {
            "OutputNode requires input channels and no output buses"
        };
    }
}

void OutputNode::process(
    const NodeProcessContext& context
) noexcept {
    static_cast<void>(context);
}

} // namespace iramix::audio
