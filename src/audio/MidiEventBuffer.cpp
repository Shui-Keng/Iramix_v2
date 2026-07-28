#include "iramix/audio/MidiEventBuffer.hpp"

#include <algorithm>
#include <cstring>

namespace iramix::audio {

void MidiEventBuffer::reserve(const int maxEvents, const int maxBytes) {
    entries_.assign(
        static_cast<std::size_t>(std::max(0, maxEvents)),
        Entry {}
    );
    bytes_.assign(
        static_cast<std::size_t>(std::max(0, maxBytes)),
        std::uint8_t {}
    );
    clear();
    resetDroppedEventCount();
}

void MidiEventBuffer::clear() noexcept {
    eventCount_ = 0;
    usedBytes_ = 0;
    nextInsertionOrder_ = 0U;
}

bool MidiEventBuffer::addEvent(
    const int sampleOffset,
    const std::uint8_t* const data,
    const int byteCount
) noexcept {
    if (data == nullptr || byteCount <= 0 || sampleOffset < 0) {
        return false;
    }

    if (eventCount_ >= static_cast<int>(entries_.size())
        || usedBytes_ + byteCount > static_cast<int>(bytes_.size())) {
        ++droppedEvents_;
        return false;
    }

    std::memcpy(
        bytes_.data() + usedBytes_,
        data,
        static_cast<std::size_t>(byteCount)
    );

    const auto order = nextInsertionOrder_++;
    int insertionIndex = eventCount_;
    while (insertionIndex > 0) {
        const auto& previous =
            entries_[static_cast<std::size_t>(insertionIndex - 1)];
        if (previous.sampleOffset < sampleOffset
            || (previous.sampleOffset == sampleOffset
                && previous.insertionOrder < order)) {
            break;
        }
        entries_[static_cast<std::size_t>(insertionIndex)] = previous;
        --insertionIndex;
    }

    entries_[static_cast<std::size_t>(insertionIndex)] = {
        .sampleOffset = sampleOffset,
        .byteOffset = usedBytes_,
        .byteCount = byteCount,
        .insertionOrder = order,
    };
    usedBytes_ += byteCount;
    ++eventCount_;
    return true;
}

bool MidiEventBuffer::mergeFrom(
    const MidiEventBuffer& source
) noexcept {
    if (&source == this) {
        return true;
    }

    bool complete = true;
    const int count = source.eventCount();
    for (int index = 0; index < count; ++index) {
        const auto sourceEvent = source.event(index);
        complete = addEvent(
            sourceEvent.sampleOffset,
            sourceEvent.data,
            sourceEvent.byteCount
        ) && complete;
    }
    return complete;
}

MidiEventView MidiEventBuffer::event(const int index) const noexcept {
    if (index < 0 || index >= eventCount_) {
        return {};
    }

    const auto& entry = entries_[static_cast<std::size_t>(index)];
    return {
        .sampleOffset = entry.sampleOffset,
        .data = bytes_.data() + entry.byteOffset,
        .byteCount = entry.byteCount,
    };
}

} // namespace iramix::audio
