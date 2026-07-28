#pragma once

#include "iramix/audio/Graph.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace iramix::audio {

using ParameterId = std::uint32_t;

struct ScheduledParameterEvent final {
    NodeId targetNode {0U};
    ParameterId parameter {0U};
    std::int64_t samplePosition {0};
    float value {0.0F};
    std::uint64_t sequence {0U};
};

struct NodeParameterEvent final {
    ParameterId parameter {0U};
    int sampleOffset {0};
    float value {0.0F};
    std::uint64_t sequence {0U};
};

// Fixed-capacity per-node block storage. reserve() is control-thread only.
class ParameterEventBuffer final {
public:
    void reserve(int maximumEvents);
    void clear() noexcept;

    [[nodiscard]] bool add(
        const NodeParameterEvent& event
    ) noexcept;

    [[nodiscard]] int eventCount() const noexcept {
        return eventCount_;
    }

    [[nodiscard]] NodeParameterEvent event(int index) const noexcept;

    [[nodiscard]] std::uint64_t droppedEventCount() const noexcept {
        return droppedEvents_;
    }

    void resetDroppedEventCount() noexcept {
        droppedEvents_ = 0U;
    }

private:
    std::vector<NodeParameterEvent> events_;
    int eventCount_ {0};
    std::uint64_t droppedEvents_ {0U};
};

// Wait-free SPSC ring for one control-thread producer and one audio-thread
// consumer. Capacity is exact; push/pop perform no allocation or destruction.
template<typename Value, std::size_t Capacity>
class SpscQueue final {
    static_assert(Capacity > 0U);
    static_assert(std::is_trivially_copyable_v<Value>);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

public:
    [[nodiscard]] bool tryPush(const Value& value) noexcept {
        const auto write = writePosition_.load(
            std::memory_order_relaxed
        );
        const auto read = readPosition_.load(
            std::memory_order_acquire
        );
        if (write - read >= Capacity) {
            rejectedPushes_.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
        storage_[static_cast<std::size_t>(write % Capacity)] = value;
        writePosition_.store(write + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool tryPeek(Value& value) const noexcept {
        const auto read = readPosition_.load(
            std::memory_order_relaxed
        );
        const auto write = writePosition_.load(
            std::memory_order_acquire
        );
        if (read == write) {
            return false;
        }
        value = storage_[static_cast<std::size_t>(read % Capacity)];
        return true;
    }

    [[nodiscard]] bool tryPop(Value& value) noexcept {
        const auto read = readPosition_.load(
            std::memory_order_relaxed
        );
        const auto write = writePosition_.load(
            std::memory_order_acquire
        );
        if (read == write) {
            return false;
        }
        value = storage_[static_cast<std::size_t>(read % Capacity)];
        readPosition_.store(read + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::uint64_t size() const noexcept {
        const auto write = writePosition_.load(
            std::memory_order_acquire
        );
        const auto read = readPosition_.load(
            std::memory_order_acquire
        );
        return write - read;
    }

    [[nodiscard]] std::uint64_t rejectedPushCount() const noexcept {
        return rejectedPushes_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool full() const noexcept {
        return size() >= Capacity;
    }

    // Both producer and consumer must be stopped.
    void clear() noexcept {
        readPosition_.store(0U, std::memory_order_relaxed);
        writePosition_.store(0U, std::memory_order_relaxed);
        rejectedPushes_.store(0U, std::memory_order_relaxed);
    }

private:
    std::array<Value, Capacity> storage_ {};
    std::atomic<std::uint64_t> readPosition_ {0U};
    std::atomic<std::uint64_t> writePosition_ {0U};
    std::atomic<std::uint64_t> rejectedPushes_ {0U};
};

} // namespace iramix::audio
