#pragma once

#include "iramix/audio/Graph.hpp"

#include <cstdint>
#include <type_traits>

namespace iramix::audio {

enum class RealtimeCommandType : std::uint8_t {
    resetAllNodes = 1U,
    resetNode = 2U,
};

struct RealtimeCommand final {
    std::uint64_t sequence {0U};
    RealtimeCommandType type {RealtimeCommandType::resetAllNodes};
    NodeId targetNode {0U};
};

enum class CommandCompletionStatus : std::uint8_t {
    applied = 1U,
    rejectedUnknownTarget = 2U,
    rejectedInvalidCommand = 3U,
};

struct RealtimeCommandCompletion final {
    std::uint64_t sequence {0U};
    CommandCompletionStatus status {
        CommandCompletionStatus::rejectedInvalidCommand
    };
    NodeId targetNode {0U};
    std::uint64_t appliedGeneration {0U};
};

struct RealtimeBlockTelemetry final {
    std::uint64_t generation {0U};
    std::int64_t samplePosition {0};
    std::uint32_t frameCount {0U};
    std::uint32_t commandsApplied {0U};
    std::uint32_t commandsRejected {0U};
    std::uint32_t parameterEventsRouted {0U};
};

static_assert(std::is_trivially_copyable_v<RealtimeCommand>);
static_assert(std::is_trivially_copyable_v<RealtimeCommandCompletion>);
static_assert(std::is_trivially_copyable_v<RealtimeBlockTelemetry>);

} // namespace iramix::audio
