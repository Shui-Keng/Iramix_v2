#include "iramix/audio/ParameterEvents.hpp"

#include <algorithm>

namespace iramix::audio {

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
    if (event.parameter == 0U
        || event.sampleOffset < 0
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
