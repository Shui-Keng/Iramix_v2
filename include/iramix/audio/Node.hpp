#pragma once

#include "iramix/audio/AudioBufferView.hpp"
#include "iramix/audio/MidiEventBuffer.hpp"
#include "iramix/audio/ParameterEvents.hpp"

#include <cstdint>
#include <span>

namespace iramix::audio {

enum class ProcessingMode : std::uint8_t {
    realtime,
    offline,
};

enum class AudioBusRole : std::uint8_t {
    main,
    sidechain,
    auxiliary,
};

struct AudioBusLayout final {
    int channelCount {0};
    AudioBusRole role {AudioBusRole::main};
    bool enabled {true};
};

struct NodePrepareInfo final {
    double sampleRate {0.0};
    int maxBlockSize {0};
    std::span<const AudioBusLayout> inputBuses;
    std::span<const AudioBusLayout> outputBuses;
    int maxMidiEvents {0};
    int maxMidiBytes {0};
    ProcessingMode mode {ProcessingMode::realtime};
};

struct TransportSnapshot final {
    std::int64_t samplePosition {0};
    double seconds {0.0};
    double quarterNotePosition {0.0};
    double tempo {120.0};
    int timeSignatureNumerator {4};
    int timeSignatureDenominator {4};
    bool playing {false};
    bool recording {false};
    bool looping {false};
    std::int64_t loopStartSample {0};
    std::int64_t loopEndSample {0};
    ProcessingMode mode {ProcessingMode::realtime};
};

struct NodeProcessContext final {
    AudioBufferView audio;
    MidiEventBuffer* midi {nullptr};
    const ParameterEventBuffer* parameters {nullptr};
    const TransportSnapshot* transport {nullptr};
};

class IAudioNode {
public:
    virtual ~IAudioNode() = default;

    virtual void prepare(const NodePrepareInfo& info) = 0;
    virtual void process(const NodeProcessContext& context) noexcept = 0;
    virtual void reset() noexcept {}

    [[nodiscard]] virtual int latencySamples() const noexcept {
        return 0;
    }
};

[[nodiscard]] inline int totalChannels(
    const std::span<const AudioBusLayout> buses
) noexcept {
    int result = 0;
    for (const auto& bus : buses) {
        if (bus.enabled && bus.channelCount > 0) {
            result += bus.channelCount;
        }
    }
    return result;
}

} // namespace iramix::audio
