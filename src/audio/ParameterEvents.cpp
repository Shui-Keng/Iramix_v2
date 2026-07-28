#include "iramix/audio/ParameterEvents.hpp"

#include <algorithm>

namespace iramix::audio {

ParameterValueState::ParameterValueState(
    const float initialValue
) noexcept
    : currentValue_ {initialValue},
      targetValue_ {initialValue} {}

void ParameterValueState::beginBlock(
    const float fallbackValue
) noexcept {
    if (!ramping()) {
        currentValue_ = fallbackValue;
        targetValue_ = fallbackValue;
    }
}

void ParameterValueState::apply(
    const NodeParameterEvent& event
) noexcept {
    switch (event.type) {
    case ParameterEventType::value:
        currentValue_ = event.value;
        targetValue_ = event.value;
        rampIncrement_ = 0.0F;
        remainingRampSamples_ = 0;
        break;
    case ParameterEventType::linearRamp:
        targetValue_ = event.value;
        remainingRampSamples_ = std::max(1, event.durationSamples);
        rampIncrement_ =
            (targetValue_ - currentValue_)
            / static_cast<float>(remainingRampSamples_);
        break;
    case ParameterEventType::modulation:
        modulation_ = event.value;
        break;
    }
}

float ParameterValueState::nextValue() noexcept {
    if (remainingRampSamples_ > 0) {
        currentValue_ += rampIncrement_;
        --remainingRampSamples_;
        if (remainingRampSamples_ == 0) {
            currentValue_ = targetValue_;
            rampIncrement_ = 0.0F;
        }
    }
    return currentValue_ + modulation_;
}

void ParameterEventBuffer::reserve(const int maximumEvents) {
    events_.assign(
        static_cast<std::size_t>(std::max(0, maximumEvents)),
        NodeParameterEvent {}
    );
    clear();
    resetDroppedEventCount();
}

void ParameterEventBuffer::clear() noexcept {
    eventCount_ = 0;
}

bool ParameterEventBuffer::add(
    const NodeParameterEvent& event
) noexcept {
    const bool validDuration =
        (event.type == ParameterEventType::linearRamp
            && event.durationSamples > 0)
        || (event.type != ParameterEventType::linearRamp
            && event.durationSamples == 0);
    if (event.parameter == 0U
        || event.sampleOffset < 0
        || !validDuration
        || eventCount_ >= static_cast<int>(events_.size())) {
        ++droppedEvents_;
        return false;
    }

    int insertionIndex = eventCount_;
    while (insertionIndex > 0) {
        const auto& previous =
            events_[static_cast<std::size_t>(insertionIndex - 1)];
        if (previous.sampleOffset < event.sampleOffset
            || (previous.sampleOffset == event.sampleOffset
                && previous.sequence < event.sequence)) {
            break;
        }
        events_[static_cast<std::size_t>(insertionIndex)] = previous;
        --insertionIndex;
    }
    events_[static_cast<std::size_t>(insertionIndex)] = event;
    ++eventCount_;
    return true;
}

NodeParameterEvent ParameterEventBuffer::event(
    const int index
) const noexcept {
    if (index < 0 || index >= eventCount_) {
        return {};
    }
    return events_[static_cast<std::size_t>(index)];
}

} // namespace iramix::audio
