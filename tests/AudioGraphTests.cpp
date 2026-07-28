#include "iramix/audio/AudioBufferView.hpp"
#include "iramix/audio/DeviceBufferConversion.hpp"
#include "iramix/audio/Graph.hpp"
#include "iramix/audio/MidiEventBuffer.hpp"
#include "iramix/audio/Node.hpp"
#include "iramix/audio/Nodes.hpp"
#include "iramix/audio/RenderPlan.hpp"
#include "iramix/audio/RenderPlanExecutor.hpp"
#include "iramix/realtime/Audit.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class SourceNode final : public iramix::audio::IAudioNode {
public:
    SourceNode(
        const int impulseOffset,
        const int midiOffset,
        const std::uint8_t midiValue
    )
        : impulseOffset_ {impulseOffset},
          midiOffset_ {midiOffset},
          midiValue_ {midiValue} {}

    void prepare(
        const iramix::audio::NodePrepareInfo&
    ) override {}

    void process(
        const iramix::audio::NodeProcessContext& context
    ) noexcept override {
        if (context.audio.channelCount() > 0
            && impulseOffset_ < context.audio.frameCount()) {
            context.audio.channel(0)[impulseOffset_] = 1.0F;
        }
        if (context.midi != nullptr) {
            static_cast<void>(context.midi->addEvent(
                midiOffset_,
                &midiValue_,
                1
            ));
        }
    }

private:
    int impulseOffset_ {0};
    int midiOffset_ {0};
    std::uint8_t midiValue_ {0U};
};

class ObservingPassThroughNode final
    : public iramix::audio::IAudioNode {
public:
    void prepare(
        const iramix::audio::NodePrepareInfo&
    ) override {}

    void process(
        const iramix::audio::NodeProcessContext& context
    ) noexcept override {
        observedMidiCount = context.midi != nullptr
            ? context.midi->eventCount()
            : 0;
        for (int index = 0;
             index < observedMidiCount
                && index < static_cast<int>(observedMidiOffsets.size());
             ++index) {
            observedMidiOffsets[static_cast<std::size_t>(index)] =
                context.midi->event(index).sampleOffset;
        }
        if (context.transport != nullptr) {
            observedSamplePosition =
                context.transport->samplePosition;
            observedTempo = context.transport->tempo;
        }
    }

    void reset() noexcept override {
        ++resetCount;
    }

    std::array<int, 8> observedMidiOffsets {};
    int observedMidiCount {0};
    std::int64_t observedSamplePosition {-1};
    double observedTempo {0.0};
    int resetCount {0};
};

class OutputNode final : public iramix::audio::IAudioNode {
public:
    void prepare(
        const iramix::audio::NodePrepareInfo&
    ) override {}

    void process(
        const iramix::audio::NodeProcessContext&
    ) noexcept override {}
};

void testBufferAndBusAbi() {
    std::array<float, 4> left {1.0F, 2.0F, 3.0F, 4.0F};
    std::array<float, 4> right {5.0F, 6.0F, 7.0F, 8.0F};
    std::array<float*, 2> pointers {left.data(), right.data()};
    const iramix::audio::AudioBufferView view {
        pointers.data(),
        2,
        4,
    };
    require(view.channelCount() == 2, "buffer channel count");
    require(view.frameCount() == 4, "buffer frame count");
    view.channelSpan(1)[2] = 9.0F;
    require(right[2] == 9.0F, "buffer view writes through");
    require(
        view.firstFrames(2).frameCount() == 2,
        "buffer view narrows frames"
    );
    require(
        view.asConst().channel(0)[3] == 4.0F,
        "const buffer view reads samples"
    );

    const std::array<iramix::audio::AudioBusLayout, 5> inputs {{
        {2, iramix::audio::AudioBusRole::main, true},
        {2, iramix::audio::AudioBusRole::sidechain, true},
        {2, iramix::audio::AudioBusRole::sidechain, true},
        {2, iramix::audio::AudioBusRole::sidechain, true},
        {2, iramix::audio::AudioBusRole::sidechain, true},
    }};
    const std::array<iramix::audio::AudioBusLayout, 1> outputs {{
        {2, iramix::audio::AudioBusRole::main, true},
    }};
    const iramix::audio::NodePrepareInfo prepareInfo {
        .sampleRate = 48'000.0,
        .maxBlockSize = 256,
        .inputBuses = inputs,
        .outputBuses = outputs,
        .maxMidiEvents = 256,
        .maxMidiBytes = 1'024,
    };
    require(
        iramix::audio::totalChannels(prepareInfo.inputBuses) == 10,
        "asymmetric sidechain input layout"
    );
    require(
        iramix::audio::totalChannels(prepareInfo.outputBuses) == 2,
        "asymmetric output layout"
    );
}

void testMidiCapacityAndOrdering() {
    iramix::audio::MidiEventBuffer buffer;
    buffer.reserve(3, 3);
    const std::uint8_t first = 1U;
    const std::uint8_t second = 2U;
    const std::uint8_t third = 3U;
    const std::uint8_t overflow = 4U;

    require(buffer.addEvent(12, &first, 1), "first MIDI event");
    require(buffer.addEvent(4, &second, 1), "second MIDI event");
    require(buffer.addEvent(12, &third, 1), "stable MIDI tie");
    require(
        buffer.event(0).sampleOffset == 4,
        "MIDI events sorted by sample offset"
    );
    require(
        buffer.event(1).data[0] == first
            && buffer.event(2).data[0] == third,
        "MIDI events preserve insertion order at equal offsets"
    );
    require(
        !buffer.addEvent(20, &overflow, 1),
        "MIDI overflow is rejected"
    );
    require(
        buffer.droppedEventCount() == 1U,
        "MIDI overflow is counted"
    );
}

void testBoundedParameterQueue() {
    iramix::audio::SpscQueue<
        iramix::audio::ScheduledParameterEvent,
        2
    > queue;
    const iramix::audio::ScheduledParameterEvent first {
        .targetNode = 1U,
        .parameter = 1U,
        .samplePosition = 10,
        .value = 0.5F,
        .sequence = 1U,
    };
    const iramix::audio::ScheduledParameterEvent second {
        .targetNode = 2U,
        .parameter = 1U,
        .samplePosition = 20,
        .value = 0.25F,
        .sequence = 2U,
    };
    require(queue.tryPush(first), "parameter queue first push");
    require(queue.tryPush(second), "parameter queue second push");
    require(!queue.tryPush(first), "parameter queue bounded overflow");
    require(queue.size() == 2U, "parameter queue exact capacity");
    require(
        queue.rejectedPushCount() == 1U,
        "parameter queue overflow diagnostic"
    );

    iramix::audio::ScheduledParameterEvent read;
    require(
        queue.tryPeek(read) && read.sequence == 1U,
        "parameter queue peek"
    );
    require(
        queue.tryPop(read) && read.sequence == 1U,
        "parameter queue FIFO first"
    );
    require(
        queue.tryPop(read) && read.sequence == 2U,
        "parameter queue FIFO second"
    );
    require(!queue.tryPop(read), "parameter queue empty");

    iramix::audio::SpscQueue<
        iramix::audio::RealtimeCommand,
        2
    > commands;
    require(
        commands.tryPush({
            .sequence = 1U,
            .type = iramix::audio::RealtimeCommandType::resetAllNodes,
        }),
        "realtime command first push"
    );
    require(
        commands.tryPush({
            .sequence = 2U,
            .type = iramix::audio::RealtimeCommandType::resetNode,
            .targetNode = 1U,
        }),
        "realtime command second push"
    );
    require(
        !commands.tryPush({
            .sequence = 3U,
            .type = iramix::audio::RealtimeCommandType::resetAllNodes,
        }),
        "realtime command saturation is explicit"
    );
    require(
        commands.rejectedPushCount() == 1U,
        "realtime command saturation counter"
    );
}

void testDeviceBufferConversion() {
    std::array<float, 3> left {-1.5F, 0.25F, 1.0F};
    std::array<float, 3> right {1.5F, -0.5F, 0.0F};
    std::array<float*, 2> pointers {left.data(), right.data()};
    const auto source = iramix::audio::AudioBufferView {
        pointers.data(),
        2,
        3,
    }.asConst();

    std::array<float, 6> floating {};
    iramix::audio::interleaveFloat32(source, floating.data());
    require(
        floating == std::array<float, 6> {
            -1.5F,
            1.5F,
            0.25F,
            -0.5F,
            1.0F,
            0.0F,
        },
        "float device interleave"
    );

    std::array<std::int16_t, 6> pcm {};
    iramix::audio::interleavePcm16(source, pcm.data());
    require(
        pcm == std::array<std::int16_t, 6> {
            -32'767,
            32'767,
            8'191,
            -16'383,
            32'767,
            0,
        },
        "PCM16 device interleave and clamp"
    );
}

void testCompilerValidation() {
    iramix::audio::GraphDescription cycle;
    require(cycle.addNode(1U), "cycle node 1");
    require(cycle.addNode(2U), "cycle node 2");
    require(
        cycle.addConnection({1U, 0, 2U, 0}),
        "cycle forward edge"
    );
    require(
        cycle.addConnection({2U, 0, 1U, 0}),
        "cycle reverse edge"
    );
    const iramix::audio::NodeInfoMap cycleInfo {
        {1U, {1, 1, 0}},
        {2U, {1, 1, 0}},
    };
    const auto cyclePlan =
        iramix::audio::compileRenderPlan(cycle, cycleInfo);
    require(!cyclePlan.valid, "cycle is rejected");

    iramix::audio::GraphDescription invalidPort;
    require(invalidPort.addNode(1U), "invalid-port node 1");
    require(invalidPort.addNode(2U), "invalid-port node 2");
    require(
        invalidPort.addConnection({1U, 1, 2U, 0}),
        "topology stores a flat port pending compilation"
    );
    const iramix::audio::NodeInfoMap invalidPortInfo {
        {1U, {0, 1, 0}},
        {2U, {1, 0, 0}},
    };
    const auto invalidPortPlan =
        iramix::audio::compileRenderPlan(
            invalidPort,
            invalidPortInfo
        );
    require(!invalidPortPlan.valid, "out-of-range port is rejected");
}

void testRenderPlanPdcMidiAndRealtimeAudit() {
    iramix::audio::GraphDescription graph;
    for (const auto id : {1U, 2U, 3U, 4U}) {
        require(graph.addNode(id), "render graph node");
    }
    require(
        graph.addConnection({1U, 0, 3U, 0}),
        "fast audio path"
    );
    require(
        graph.addConnection({2U, 0, 3U, 0}),
        "slow audio path"
    );
    require(
        graph.addConnection({
            1U,
            iramix::audio::kMidiLane,
            3U,
            iramix::audio::kMidiLane,
        }),
        "first MIDI path"
    );
    require(
        graph.addConnection({
            2U,
            iramix::audio::kMidiLane,
            3U,
            iramix::audio::kMidiLane,
        }),
        "second MIDI path"
    );
    require(
        graph.addConnection({3U, 0, 4U, 0}),
        "mix to output"
    );

    const iramix::audio::NodeInfoMap nodeInfo {
        {1U, {0, 1, 0}},
        {2U, {0, 1, 3}},
        {3U, {1, 1, 0}},
        {4U, {1, 0, 0}},
    };
    const auto plan =
        iramix::audio::compileRenderPlan(graph, nodeInfo);
    require(plan.valid, "render plan compiles");
    require(plan.delayLineCount == 1, "PDC emits one delay line");
    require(plan.latencyAtNode.at(4U) == 3, "sink latency is reported");

    auto fast = std::make_shared<SourceNode>(
        0,
        12,
        static_cast<std::uint8_t>(0x11U)
    );
    auto slow = std::make_shared<SourceNode>(
        3,
        4,
        static_cast<std::uint8_t>(0x22U)
    );
    auto mix = std::make_shared<ObservingPassThroughNode>();
    auto output = std::make_shared<OutputNode>();

    iramix::audio::RenderPlanExecutor executor;
    std::string error;
    const bool prepared = executor.prepareAndPublish(
        plan,
        {
            .maximumBlockSize = 64,
            .maximumMidiEventsPerNode = 16,
            .maximumMidiBytesPerNode = 64,
            .outputNode = 4U,
            .outputChannelCount = 1,
        },
        [&](const iramix::audio::NodeId id)
            -> std::shared_ptr<iramix::audio::IAudioNode> {
            switch (id) {
            case 1U:
                return fast;
            case 2U:
                return slow;
            case 3U:
                return mix;
            case 4U:
                return output;
            default:
                return {};
            }
        },
        error
    );
    require(prepared, error.c_str());
    require(
        executor.unresolvedNodeCount() == 0,
        "all render nodes resolve"
    );
    require(
        executor.enqueueRealtimeCommand({
            .sequence = 1U,
            .type = iramix::audio::RealtimeCommandType::resetNode,
            .targetNode = 3U,
        }),
        "reset-node command enqueues"
    );
    require(
        executor.enqueueRealtimeCommand({
            .sequence = 2U,
            .type = iramix::audio::RealtimeCommandType::resetNode,
            .targetNode = 999U,
        }),
        "unknown-target command reaches audio validation"
    );
    require(
        !executor.enqueueRealtimeCommand({
            .sequence = 2U,
            .type = iramix::audio::RealtimeCommandType::resetAllNodes,
        }),
        "duplicate command sequence is rejected"
    );

    iramix::realtime::resetAuditCounters();
    executor.process(
        64,
        {
            .samplePosition = 96'000,
            .seconds = 2.0,
            .quarterNotePosition = 4.0,
            .tempo = 137.5,
            .playing = true,
        }
    );
    const auto audit = iramix::realtime::auditSnapshot();
    require(audit.allocations == 0U, "zero callback allocations");
    require(audit.deallocations == 0U, "zero callback deallocations");
    require(audit.blockingLocks == 0U, "zero callback blocking locks");
    require(mix->resetCount == 1, "target node reset is applied");

    iramix::audio::RealtimeCommandCompletion completion;
    require(
        executor.tryPopCommandCompletion(completion)
            && completion.sequence == 1U
            && completion.status
                == iramix::audio::CommandCompletionStatus::applied,
        "applied command completion"
    );
    require(
        executor.tryPopCommandCompletion(completion)
            && completion.sequence == 2U
            && completion.status
                == iramix::audio::CommandCompletionStatus::
                    rejectedUnknownTarget,
        "rejected command completion"
    );
    require(
        !executor.tryPopCommandCompletion(completion),
        "command completion queue drained"
    );
    require(
        executor.rejectedRealtimeCommandCount() == 1U,
        "invalid command enqueue is counted"
    );
    require(
        executor.lostCommandCompletionCount() == 0U,
        "command completions are not lost"
    );

    iramix::audio::RealtimeBlockTelemetry telemetry;
    require(
        executor.tryPopTelemetry(telemetry)
            && telemetry.commandsApplied == 1U
            && telemetry.commandsRejected == 1U
            && telemetry.generation == 1U,
        "block telemetry reports command outcomes"
    );

    const float* const rendered = executor.outputChannel(0);
    require(rendered != nullptr, "rendered output exists");
    require(
        std::abs(rendered[3] - 2.0F) < 0.000001F,
        "PDC aligns both impulses"
    );
    for (int frame = 0; frame < 64; ++frame) {
        if (frame != 3) {
            require(
                std::abs(rendered[frame]) < 0.000001F,
                "PDC output has no stray samples"
            );
        }
    }
    require(mix->observedMidiCount == 2, "MIDI fan-in reaches node");
    require(
        mix->observedMidiOffsets[0] == 4
            && mix->observedMidiOffsets[1] == 12,
        "MIDI fan-in remains sample sorted"
    );
    require(
        mix->observedSamplePosition == 96'000,
        "transport sample position reaches node"
    );
    require(
        std::abs(mix->observedTempo - 137.5) < 0.000001,
        "transport tempo reaches node"
    );

    std::uint64_t commandSequence = 3U;
    for (std::size_t index = 0U; index < 1'024U; ++index) {
        require(
            executor.enqueueRealtimeCommand({
                .sequence = commandSequence++,
                .type =
                    iramix::audio::RealtimeCommandType::resetNode,
                .targetNode = 3U,
            }),
            "completion saturation command enqueues"
        );
    }
    for (std::size_t block = 0U; block < 16U; ++block) {
        executor.process(64, {});
    }
    require(
        executor.pendingRealtimeCommandCount() == 0U,
        "commands drain while completion capacity remains"
    );
    require(
        executor.enqueueRealtimeCommand({
            .sequence = commandSequence++,
            .type = iramix::audio::RealtimeCommandType::resetNode,
            .targetNode = 3U,
        }),
        "command remains accepted behind completion backpressure"
    );
    executor.process(64, {});
    require(
        executor.pendingRealtimeCommandCount() == 1U,
        "full completion queue leaves command pending"
    );
    require(
        executor.lostCommandCompletionCount() == 0U,
        "completion backpressure prevents lost ACK"
    );
    require(
        executor.tryPopCommandCompletion(completion),
        "control thread releases one completion slot"
    );
    executor.process(64, {});
    require(
        executor.pendingRealtimeCommandCount() == 0U,
        "pending command resumes after completion consumption"
    );
    const auto saturationAudit =
        iramix::realtime::auditSnapshot();
    require(
        saturationAudit.allocations == 0U,
        "completion backpressure has zero callback allocations"
    );
    require(
        saturationAudit.deallocations == 0U,
        "completion backpressure has zero callback deallocations"
    );
    require(
        saturationAudit.blockingLocks == 0U,
        "completion backpressure has zero callback blocking locks"
    );

    std::cout
        << "Graph render audit: allocations=" << audit.allocations
        << ", deallocations=" << audit.deallocations
        << ", blocking_locks=" << audit.blockingLocks
        << ", pdc_peak_frame=3, pdc_peak=2\n";
}

void testProductionNodeSignalChain() {
    constexpr iramix::audio::NodeId inputId = 10U;
    constexpr iramix::audio::NodeId trackId = 11U;
    constexpr iramix::audio::NodeId gainId = 12U;
    constexpr iramix::audio::NodeId mixerId = 13U;
    constexpr iramix::audio::NodeId outputId = 14U;

    iramix::audio::GraphDescription graph;
    for (const auto id : {
             inputId,
             trackId,
             gainId,
             mixerId,
             outputId,
         }) {
        require(graph.addNode(id), "production graph node");
    }
    for (const auto source : {inputId, trackId, gainId, mixerId}) {
        const auto destination = source + 1U;
        for (int channel = 0; channel < 2; ++channel) {
            require(
                graph.addConnection({
                    source,
                    channel,
                    destination,
                    channel,
                }),
                "production graph connection"
            );
        }
    }

    const iramix::audio::NodeInfoMap nodeInfo {
        {inputId, {0, 2, 0}},
        {trackId, {2, 2, 0}},
        {gainId, {2, 2, 0}},
        {mixerId, {2, 2, 0}},
        {outputId, {2, 0, 0}},
    };
    const auto plan =
        iramix::audio::compileRenderPlan(graph, nodeInfo);
    require(plan.valid, "production graph compiles");

    const std::array<iramix::audio::AudioBusLayout, 1> stereoBus {{
        {2, iramix::audio::AudioBusRole::main, true},
    }};
    const std::span<const iramix::audio::AudioBusLayout> noBuses;
    const auto prepareInfo = [](
        const std::span<const iramix::audio::AudioBusLayout> inputs,
        const std::span<const iramix::audio::AudioBusLayout> outputs
    ) {
        return iramix::audio::NodePrepareInfo {
            .sampleRate = 48'000.0,
            .maxBlockSize = 64,
            .inputBuses = inputs,
            .outputBuses = outputs,
            .maxMidiEvents = 16,
            .maxMidiBytes = 64,
        };
    };

    auto input = std::make_shared<iramix::audio::DeviceInputNode>();
    auto track = std::make_shared<iramix::audio::TrackNode>();
    auto gain = std::make_shared<iramix::audio::GainNode>(0.5F);
    auto mixer = std::make_shared<iramix::audio::MixerNode>();
    auto output = std::make_shared<iramix::audio::OutputNode>();
    input->prepare(prepareInfo(noBuses, stereoBus));
    track->prepare(prepareInfo(stereoBus, stereoBus));
    gain->prepare(prepareInfo(stereoBus, stereoBus));
    mixer->prepare(prepareInfo(stereoBus, stereoBus));
    output->prepare(prepareInfo(stereoBus, noBuses));
    track->setGain(2.0F);
    track->setPan(0.5F);
    mixer->setOutputGain(0.25F);

    iramix::audio::RenderPlanExecutor executor;
    std::string error;
    require(
        executor.prepareAndPublish(
            plan,
            {
                .maximumBlockSize = 64,
                .maximumMidiEventsPerNode = 16,
                .maximumMidiBytesPerNode = 64,
                .maximumParameterEventsPerNode = 16,
                .outputNode = outputId,
                .outputChannelCount = 2,
            },
            [&](const iramix::audio::NodeId id)
                -> std::shared_ptr<iramix::audio::IAudioNode> {
                switch (id) {
                case inputId:
                    return input;
                case trackId:
                    return track;
                case gainId:
                    return gain;
                case mixerId:
                    return mixer;
                case outputId:
                    return output;
                default:
                    return {};
                }
            },
            error
        ),
        error.c_str()
    );

    std::array<float, 4> inputLeft {1.0F, 2.0F, 3.0F, 4.0F};
    std::array<float, 4> inputRight {4.0F, 3.0F, 2.0F, 1.0F};
    std::array<const float*, 2> inputPointers {
        inputLeft.data(),
        inputRight.data(),
    };
    std::array<float, 4> outputLeft {};
    std::array<float, 4> outputRight {};
    std::array<float*, 2> outputPointers {
        outputLeft.data(),
        outputRight.data(),
    };
    input->bindInput({
        inputPointers.data(),
        2,
        4,
    });
    require(
        executor.enqueueParameterEvent(
            gainId,
            iramix::audio::GainNode::kGainParameter,
            1'026,
            1.0F
        ),
        "sample-accurate gain event enqueues"
    );
    require(
        !executor.enqueueParameterEvent(
            gainId,
            iramix::audio::GainNode::kGainParameter,
            1'025,
            0.25F
        ),
        "out-of-order automation is rejected"
    );
    require(
        executor.rejectedParameterEventCount() == 1U,
        "out-of-order automation diagnostic"
    );

    iramix::realtime::resetAuditCounters();
    executor.renderTo(
        {
            outputPointers.data(),
            2,
            4,
        },
        {
            .samplePosition = 1'024,
            .playing = true,
        }
    );
    const auto audit = iramix::realtime::auditSnapshot();
    require(audit.allocations == 0U, "production graph allocations");
    require(audit.deallocations == 0U, "production graph deallocations");
    require(audit.blockingLocks == 0U, "production graph locks");

    for (std::size_t index = 0U; index < inputLeft.size(); ++index) {
        const float gainFactor = index < 2U ? 0.125F : 0.25F;
        require(
            std::abs(outputLeft[index] - inputLeft[index] * gainFactor)
                < 0.000001F,
            "sample-accurate left gain"
        );
        require(
            std::abs(
                outputRight[index]
                - inputRight[index] * gainFactor * 2.0F
            )
                < 0.000001F,
            "sample-accurate right gain"
        );
    }

    track->setMuted(true);
    executor.renderTo(
        {
            outputPointers.data(),
            2,
            4,
        },
        {}
    );
    for (const float sample : outputLeft) {
        require(sample == 0.0F, "track mute clears left");
    }
    for (const float sample : outputRight) {
        require(sample == 0.0F, "track mute clears right");
    }
    input->unbindInput();

    std::cout
        << "Production node chain: channels=2, frames=4, "
        << "allocations=" << audit.allocations
        << ", deallocations=" << audit.deallocations
        << ", blocking_locks=" << audit.blockingLocks
        << ", automation_offset=2"
        << '\n';
}

} // namespace

int main() {
    testBufferAndBusAbi();
    testMidiCapacityAndOrdering();
    testBoundedParameterQueue();
    testDeviceBufferConversion();
    testCompilerValidation();
    testRenderPlanPdcMidiAndRealtimeAudit();
    testProductionNodeSignalChain();
    std::cout << "All Iramix audio graph tests passed.\n";
    return EXIT_SUCCESS;
}
