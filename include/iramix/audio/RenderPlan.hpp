#pragma once

#include "iramix/audio/Graph.hpp"

#include <map>
#include <string>
#include <vector>

namespace iramix::audio {

enum class RenderOpType {
    clear,
    copy,
    add,
    delay,
    midiClear,
    midiMerge,
    process,
};

struct RenderOp final {
    RenderOpType type {RenderOpType::clear};
    int sourceBuffer {-1};
    int destinationBuffer {-1};
    NodeId node {0U};
    int firstChannel {0};
    int channelCount {0};
    int sourceMidi {-1};
    int destinationMidi {-1};
    int delayLine {-1};
    int delaySamples {0};
};

struct NodeInfo final {
    int audioInputCount {0};
    int audioOutputCount {0};
    int latencySamples {0};
};

using NodeInfoMap = std::map<NodeId, NodeInfo>;

// Pure, immutable data after compilation. Runtime storage and node ownership
// live in PreparedRenderPlan, not here.
struct RenderPlan final {
    std::vector<RenderOp> operations;
    std::vector<int> channelBuffers;
    int bufferCount {0};
    int delayLineCount {0};
    int maximumDelaySamples {0};
    int midiBufferCount {0};
    std::map<NodeId, int> latencyAtNode;
    bool valid {false};
    std::string error;
};

// Control-thread only. Rejects cycles, invalid ports, and missing node
// metadata. Fan-in ordering and buffer reuse are deterministic.
[[nodiscard]] RenderPlan compileRenderPlan(
    const GraphDescription& graph,
    const NodeInfoMap& nodeInfo
);

} // namespace iramix::audio
