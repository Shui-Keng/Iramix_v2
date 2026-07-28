#include "iramix/audio/RenderPlan.hpp"

#include <algorithm>
#include <deque>
#include <set>
#include <utility>

namespace iramix::audio {
namespace {

struct SignalKey final {
    NodeId node {0U};
    int channel {0};

    [[nodiscard]] bool operator<(const SignalKey& other) const noexcept {
        return node != other.node
            ? node < other.node
            : channel < other.channel;
    }
};

class BufferPool final {
public:
    [[nodiscard]] int acquire() {
        if (!free_.empty()) {
            const int result = *free_.begin();
            free_.erase(free_.begin());
            return result;
        }
        return highWater_++;
    }

    void release(const int index) {
        if (index >= 0) {
            free_.insert(index);
        }
    }

    [[nodiscard]] int highWater() const noexcept {
        return highWater_;
    }

private:
    std::set<int> free_;
    int highWater_ {0};
};

void emitAudioOp(
    RenderPlan& plan,
    const RenderOpType type,
    const int source,
    const int destination
) {
    plan.operations.push_back({
        .type = type,
        .sourceBuffer = source,
        .destinationBuffer = destination,
    });
}

void emitDelayOp(
    RenderPlan& plan,
    const int buffer,
    const int samples
) {
    plan.operations.push_back({
        .type = RenderOpType::delay,
        .destinationBuffer = buffer,
        .delayLine = plan.delayLineCount++,
        .delaySamples = samples,
    });
    plan.maximumDelaySamples =
        std::max(plan.maximumDelaySamples, samples);
}

} // namespace

RenderPlan compileRenderPlan(
    const GraphDescription& graph,
    const NodeInfoMap& nodeInfo
) {
    RenderPlan plan;
    const auto nodes = graph.nodes();
    const auto connections = graph.connections();

    for (const NodeId id : nodes) {
        const auto iterator = nodeInfo.find(id);
        if (iterator == nodeInfo.end()) {
            plan.error =
                "node " + std::to_string(id) + " has no port information";
            return plan;
        }

        const auto& info = iterator->second;
        if (info.audioInputCount < 0
            || info.audioOutputCount < 0
            || info.latencySamples < 0) {
            plan.error =
                "node " + std::to_string(id) + " has invalid metadata";
            return plan;
        }
    }

    for (const auto& connection : connections) {
        const auto source = nodeInfo.find(connection.sourceNode);
        const auto destination =
            nodeInfo.find(connection.destinationNode);
        if (source == nodeInfo.end() || destination == nodeInfo.end()) {
            plan.error = "a connection references an unknown node";
            return plan;
        }

        if (!connection.isMidi()
            && (connection.sourceChannel
                    >= source->second.audioOutputCount
                || connection.destinationChannel
                    >= destination->second.audioInputCount)) {
            plan.error = "a connection references an audio port outside "
                         "the node layout";
            return plan;
        }
    }

    std::map<NodeId, std::vector<NodeId>> successors;
    std::map<NodeId, int> inDegree;
    for (const NodeId id : nodes) {
        inDegree[id] = 0;
    }

    std::set<std::pair<NodeId, NodeId>> orderingEdges;
    for (const auto& connection : connections) {
        orderingEdges.insert({
            connection.sourceNode,
            connection.destinationNode,
        });
    }
    for (const auto& [source, destination] : orderingEdges) {
        successors[source].push_back(destination);
        ++inDegree[destination];
    }

    std::deque<NodeId> ready;
    for (const auto& [id, degree] : inDegree) {
        if (degree == 0) {
            ready.push_back(id);
        }
    }

    std::vector<NodeId> order;
    order.reserve(nodes.size());
    while (!ready.empty()) {
        const NodeId id = ready.front();
        ready.pop_front();
        order.push_back(id);
        for (const NodeId successor : successors[id]) {
            if (--inDegree[successor] == 0) {
                ready.push_back(successor);
            }
        }
    }

    if (order.size() != nodes.size()) {
        plan.error = "the graph contains a cycle";
        return plan;
    }

    std::map<NodeId, int> position;
    for (std::size_t index = 0U; index < order.size(); ++index) {
        position[order[index]] = static_cast<int>(index);
    }

    std::map<SignalKey, int> lastConsumer;
    std::map<SignalKey, std::vector<SignalKey>> audioInputs;
    std::map<NodeId, std::vector<NodeId>> midiInputs;

    for (const auto& connection : connections) {
        if (connection.isMidi()) {
            midiInputs[connection.destinationNode].push_back(
                connection.sourceNode
            );
            continue;
        }

        const SignalKey source {
            connection.sourceNode,
            connection.sourceChannel,
        };
        const SignalKey destination {
            connection.destinationNode,
            connection.destinationChannel,
        };
        audioInputs[destination].push_back(source);
        lastConsumer[source] = std::max(
            lastConsumer.contains(source) ? lastConsumer[source] : -1,
            position[connection.destinationNode]
        );
    }

    for (auto& [destination, sources] : audioInputs) {
        static_cast<void>(destination);
        std::sort(sources.begin(), sources.end());
    }
    for (auto& [destination, sources] : midiInputs) {
        static_cast<void>(destination);
        std::sort(sources.begin(), sources.end());
        sources.erase(
            std::unique(sources.begin(), sources.end()),
            sources.end()
        );
    }

    std::map<NodeId, int> arrivalAtNode;
    for (const NodeId id : order) {
        int arrival = 0;
        for (const auto& connection : connections) {
            if (connection.isMidi()
                || connection.destinationNode != id) {
                continue;
            }
            const auto& producer = nodeInfo.at(connection.sourceNode);
            arrival = std::max(
                arrival,
                arrivalAtNode[connection.sourceNode]
                    + producer.latencySamples
            );
        }
        arrivalAtNode[id] = arrival;
        plan.latencyAtNode[id] =
            arrival + nodeInfo.at(id).latencySamples;
    }

    std::map<NodeId, int> midiBufferForNode;
    for (const NodeId id : order) {
        midiBufferForNode[id] = plan.midiBufferCount++;
    }

    BufferPool pool;
    std::map<SignalKey, int> bufferForSignal;

    for (int step = 0; step < static_cast<int>(order.size()); ++step) {
        const NodeId id = order[static_cast<std::size_t>(step)];
        const auto& info = nodeInfo.at(id);
        const int channelCount =
            std::max(info.audioInputCount, info.audioOutputCount);
        const int firstChannel =
            static_cast<int>(plan.channelBuffers.size());

        for (int channel = 0; channel < channelCount; ++channel) {
            const int destinationBuffer = pool.acquire();
            plan.channelBuffers.push_back(destinationBuffer);
            const auto sources =
                audioInputs.find({id, channel});

            if (sources == audioInputs.end() || sources->second.empty()) {
                emitAudioOp(
                    plan,
                    RenderOpType::clear,
                    -1,
                    destinationBuffer
                );
                continue;
            }

            bool firstSource = true;
            for (const auto& sourceSignal : sources->second) {
                const auto sourceBuffer =
                    bufferForSignal.find(sourceSignal);
                if (sourceBuffer == bufferForSignal.end()) {
                    continue;
                }

                const int sourceArrival =
                    arrivalAtNode[sourceSignal.node]
                    + nodeInfo.at(sourceSignal.node).latencySamples;
                const int compensation =
                    arrivalAtNode[id] - sourceArrival;

                if (firstSource) {
                    emitAudioOp(
                        plan,
                        RenderOpType::copy,
                        sourceBuffer->second,
                        destinationBuffer
                    );
                    if (compensation > 0) {
                        emitDelayOp(
                            plan,
                            destinationBuffer,
                            compensation
                        );
                    }
                    firstSource = false;
                } else if (compensation == 0) {
                    emitAudioOp(
                        plan,
                        RenderOpType::add,
                        sourceBuffer->second,
                        destinationBuffer
                    );
                } else {
                    const int scratch = pool.acquire();
                    emitAudioOp(
                        plan,
                        RenderOpType::copy,
                        sourceBuffer->second,
                        scratch
                    );
                    emitDelayOp(plan, scratch, compensation);
                    emitAudioOp(
                        plan,
                        RenderOpType::add,
                        scratch,
                        destinationBuffer
                    );
                    pool.release(scratch);
                }
            }

            if (firstSource) {
                emitAudioOp(
                    plan,
                    RenderOpType::clear,
                    -1,
                    destinationBuffer
                );
            }
        }

        const int midiBuffer = midiBufferForNode[id];
        plan.operations.push_back({
            .type = RenderOpType::midiClear,
            .destinationMidi = midiBuffer,
        });

        if (const auto sources = midiInputs.find(id);
            sources != midiInputs.end()) {
            for (const NodeId source : sources->second) {
                plan.operations.push_back({
                    .type = RenderOpType::midiMerge,
                    .sourceMidi = midiBufferForNode[source],
                    .destinationMidi = midiBuffer,
                });
            }
        }

        plan.operations.push_back({
            .type = RenderOpType::process,
            .node = id,
            .firstChannel = firstChannel,
            .channelCount = channelCount,
            .destinationMidi = midiBuffer,
        });

        for (int channel = 0;
             channel < info.audioOutputCount && channel < channelCount;
             ++channel) {
            bufferForSignal[{id, channel}] =
                plan.channelBuffers[
                    static_cast<std::size_t>(firstChannel + channel)
                ];
        }

        for (int channel = 0; channel < channelCount; ++channel) {
            const SignalKey signal {id, channel};
            const bool isOutput = channel < info.audioOutputCount;
            if (!isOutput || !lastConsumer.contains(signal)) {
                pool.release(
                    plan.channelBuffers[
                        static_cast<std::size_t>(firstChannel + channel)
                    ]
                );
                bufferForSignal.erase(signal);
            }
        }

        for (auto iterator = bufferForSignal.begin();
             iterator != bufferForSignal.end();) {
            const auto consumer = lastConsumer.find(iterator->first);
            if (consumer != lastConsumer.end()
                && consumer->second == step) {
                pool.release(iterator->second);
                iterator = bufferForSignal.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    plan.bufferCount = pool.highWater();
    plan.valid = true;
    return plan;
}

} // namespace iramix::audio
