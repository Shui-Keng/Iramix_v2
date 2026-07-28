#pragma once

#include <cstdint>
#include <vector>

namespace iramix::audio {

struct MidiEventView final {
    int sampleOffset {0};
    const std::uint8_t* data {nullptr};
    int byteCount {0};
};

// Fixed-capacity MIDI storage. reserve() is control-thread only; all other
// mutation is allocation-free and suitable for the audio callback.
class MidiEventBuffer final {
public:
    void reserve(int maxEvents, int maxBytes);
    void clear() noexcept;

    [[nodiscard]] bool addEvent(
        int sampleOffset,
        const std::uint8_t* data,
        int byteCount
    ) noexcept;

    [[nodiscard]] bool mergeFrom(
        const MidiEventBuffer& source
    ) noexcept;

    [[nodiscard]] int eventCount() const noexcept {
        return eventCount_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return eventCount_ == 0;
    }

    [[nodiscard]] MidiEventView event(int index) const noexcept;

    [[nodiscard]] int eventCapacity() const noexcept {
        return static_cast<int>(entries_.size());
    }

    [[nodiscard]] int byteCapacity() const noexcept {
        return static_cast<int>(bytes_.size());
    }

    [[nodiscard]] std::uint64_t droppedEventCount() const noexcept {
        return droppedEvents_;
    }

    void resetDroppedEventCount() noexcept {
        droppedEvents_ = 0U;
    }

private:
    struct Entry final {
        int sampleOffset {0};
        int byteOffset {0};
        int byteCount {0};
        std::uint64_t insertionOrder {0U};
    };

    std::vector<Entry> entries_;
    std::vector<std::uint8_t> bytes_;
    int eventCount_ {0};
    int usedBytes_ {0};
    std::uint64_t droppedEvents_ {0U};
    std::uint64_t nextInsertionOrder_ {0U};
};

} // namespace iramix::audio
