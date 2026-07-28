#pragma once

#include "iramix/audio/Node.hpp"
#include "iramix/audio/RealtimeControl.hpp"
#include "iramix/audio/RenderPlan.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace iramix::audio {

class PreparedRenderPlan final {
public:
    PreparedRenderPlan() = default;
    ~PreparedRenderPlan();

    PreparedRenderPlan(const PreparedRenderPlan&) = delete;
    PreparedRenderPlan& operator=(const PreparedRenderPlan&) = delete;

    static constexpr std::uint32_t kLiveMagic = 0x4952414dU;
    static constexpr std::uint32_t kDeadMagic = 0xDEADBEEFU;

    struct ExecutionOp final {
        RenderOpType type {RenderOpType::clear};
        float* source {nullptr};
        float* destination {nullptr};
        IAudioNode* node {nullptr};
        NodeId nodeId {0U};
        float* const* channels {nullptr};
        int channelCount {0};
        MidiEventBuffer* sourceMidi {nullptr};
        MidiEventBuffer* destinationMidi {nullptr};
        const ParameterEventBuffer* parameters {nullptr};
        int delayLine {-1};
        int delaySamples {0};
        bool capturesOutput {false};
    };

    std::uint32_t magic {kLiveMagic};
    std::uint64_t generation {0U};
    std::vector<ExecutionOp> operations;
    std::vector<float> audioStorage;
    std::vector<float*> bufferPointers;
    std::vector<float*> channelPointers;
    std::vector<float> delayStorage;
    std::vector<int> delayOffsets;
    std::vector<int> delayLengths;
    std::vector<int> delayWritePositions;
    std::vector<MidiEventBuffer> midiBuffers;
    std::vector<ParameterEventBuffer> parameterBuffers;
    std::vector<NodeId> parameterNodeIds;
    std::vector<const float*> outputSources;

    // Holds node lifetimes without shared_ptr traffic in the callback.
    std::vector<std::shared_ptr<IAudioNode>> nodeOwners;

    int maximumBlockSize {0};
    int unresolvedNodeCount {0};
};

// Single-audio-thread executor. Publication and reclamation occur on one
// control thread. process() performs one atomic pointer load and never
// allocates, locks, destroys, or resolves a node.
class RenderPlanExecutor final {
public:
    struct PrepareInfo final {
        int maximumBlockSize {0};
        int maximumMidiEventsPerNode {0};
        int maximumMidiBytesPerNode {0};
        int maximumParameterEventsPerNode {0};
        NodeId outputNode {0U};
        int outputChannelCount {0};
    };

    using NodeResolver =
        std::function<std::shared_ptr<IAudioNode>(NodeId)>;

    RenderPlanExecutor() = default;
    ~RenderPlanExecutor();

    RenderPlanExecutor(const RenderPlanExecutor&) = delete;
    RenderPlanExecutor& operator=(const RenderPlanExecutor&) = delete;

    [[nodiscard]] bool prepareAndPublish(
        const RenderPlan& plan,
        const PrepareInfo& info,
        const NodeResolver& resolve,
        std::string& error
    );

    // Single control-thread producer. Events must be enqueued in nondecreasing
    // absolute sample-position order. Seeking requires clearParameterEvents()
    // while the audio thread is stopped.
    [[nodiscard]] bool enqueueParameterEvent(
        NodeId targetNode,
        ParameterId parameter,
        std::int64_t samplePosition,
        float value
    ) noexcept;

    [[nodiscard]] bool enqueueParameterRamp(
        NodeId targetNode,
        ParameterId parameter,
        std::int64_t samplePosition,
        float targetValue,
        int durationSamples
    ) noexcept;

    [[nodiscard]] bool enqueueParameterModulation(
        NodeId targetNode,
        ParameterId parameter,
        std::int64_t samplePosition,
        float additiveValue
    ) noexcept;

    void clearParameterEvents() noexcept;

    // Single control-thread producer. A false return is an explicit rejection;
    // callers retain the authoritative command until enqueue succeeds.
    [[nodiscard]] bool enqueueRealtimeCommand(
        const RealtimeCommand& command
    ) noexcept;

    // Single control-thread consumers.
    [[nodiscard]] bool tryPopCommandCompletion(
        RealtimeCommandCompletion& completion
    ) noexcept;

    [[nodiscard]] bool tryPopTelemetry(
        RealtimeBlockTelemetry& telemetry
    ) noexcept;

    // All realtime/control queue participants must be stopped.
    void clearRealtimeControl() noexcept;

    void process(
        int frameCount,
        const TransportSnapshot& transport
    ) noexcept;

    // Audio-thread device boundary. Renders one plan block and copies its
    // planar output into the caller-owned destination without allocation.
    // Missing channels and frames beyond the prepared maximum are cleared.
    void renderTo(
        AudioBufferView destination,
        const TransportSnapshot& transport
    ) noexcept;

    [[nodiscard]] const float* outputChannel(
        int channel
    ) const noexcept;

    // Control thread, while process() is stopped.
    void reset() noexcept;
    void flushRetiredPlans();
    void releaseAll();

    // Control thread; safe while process() is running.
    [[nodiscard]] int reclaimRetiredPlans();
    [[nodiscard]] int retiredPlanCount() const noexcept;

    [[nodiscard]] bool prepared() const noexcept {
        return live_.load(std::memory_order_acquire) != nullptr;
    }

    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generationCounter_;
    }

    [[nodiscard]] std::uint64_t acknowledgedGeneration() const noexcept {
        return acknowledgedGeneration_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t renderedBlockCount() const noexcept {
        return renderedBlocks_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t observedSwapCount() const noexcept {
        return observedSwaps_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t oversizedBlockCount() const noexcept {
        return oversizedBlocks_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t rejectedParameterEventCount() const
        noexcept;

    [[nodiscard]] std::uint64_t pendingParameterEventCount()
        const noexcept {
        return parameterEvents_.size();
    }

    [[nodiscard]] std::uint64_t lateParameterEventCount() const noexcept {
        return lateParameterEvents_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t unknownParameterTargetCount()
        const noexcept {
        return unknownParameterTargets_.load(
            std::memory_order_relaxed
        );
    }

    [[nodiscard]] std::uint64_t parameterBufferOverflowCount()
        const noexcept {
        return parameterBufferOverflows_.load(
            std::memory_order_relaxed
        );
    }

    [[nodiscard]] std::uint64_t rejectedRealtimeCommandCount()
        const noexcept;

    [[nodiscard]] std::uint64_t lostCommandCompletionCount()
        const noexcept {
        return lostCommandCompletions_.load(
            std::memory_order_relaxed
        );
    }

    [[nodiscard]] std::uint64_t droppedTelemetryCount()
        const noexcept {
        return telemetry_.rejectedPushCount();
    }

    [[nodiscard]] std::uint64_t pendingRealtimeCommandCount()
        const noexcept {
        return realtimeCommands_.size();
    }

    [[nodiscard]] int useAfterFreeCount() const noexcept {
        return useAfterFree_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int totalReclaimedPlanCount() const noexcept {
        return totalReclaimedPlans_;
    }

    // Read these only while process() is stopped.
    [[nodiscard]] int unresolvedNodeCount() const noexcept;
    [[nodiscard]] std::uint64_t droppedMidiEventCount() const noexcept;

private:
    [[nodiscard]] bool enqueueParameterChange(
        NodeId targetNode,
        ParameterId parameter,
        std::int64_t samplePosition,
        float value,
        ParameterEventType type,
        int durationSamples
    ) noexcept;

    void execute(
        PreparedRenderPlan& plan,
        int frameCount,
        const TransportSnapshot& transport
    ) noexcept;

    [[nodiscard]] std::uint32_t routeParameterEvents(
        PreparedRenderPlan& plan,
        int frameCount,
        const TransportSnapshot& transport
    ) noexcept;

    struct CommandDrainResult final {
        std::uint32_t applied {0U};
        std::uint32_t rejected {0U};
    };

    [[nodiscard]] CommandDrainResult drainRealtimeCommands(
        PreparedRenderPlan& plan
    ) noexcept;

    std::atomic<PreparedRenderPlan*> live_ {nullptr};
    std::atomic<std::uint64_t> acknowledgedGeneration_ {0U};
    std::atomic<std::uint64_t> renderedBlocks_ {0U};
    std::atomic<std::uint64_t> observedSwaps_ {0U};
    std::atomic<std::uint64_t> lastObservedGeneration_ {0U};
    std::atomic<std::uint64_t> oversizedBlocks_ {0U};
    std::atomic<int> useAfterFree_ {0};
    std::atomic<std::uint64_t> invalidParameterEvents_ {0U};
    std::atomic<std::uint64_t> lateParameterEvents_ {0U};
    std::atomic<std::uint64_t> unknownParameterTargets_ {0U};
    std::atomic<std::uint64_t> parameterBufferOverflows_ {0U};

    static constexpr std::size_t kParameterQueueCapacity = 4'096U;
    SpscQueue<ScheduledParameterEvent, kParameterQueueCapacity>
        parameterEvents_;
    std::int64_t lastEnqueuedParameterSample_ {-1};
    std::uint64_t nextParameterSequence_ {1U};

    static constexpr std::size_t kRealtimeCommandCapacity = 1'024U;
    static constexpr std::size_t kCommandCompletionCapacity = 1'024U;
    static constexpr std::size_t kTelemetryCapacity = 2'048U;
    static constexpr std::uint32_t kMaximumCommandsPerBlock = 64U;
    SpscQueue<RealtimeCommand, kRealtimeCommandCapacity>
        realtimeCommands_;
    SpscQueue<
        RealtimeCommandCompletion,
        kCommandCompletionCapacity
    > commandCompletions_;
    SpscQueue<RealtimeBlockTelemetry, kTelemetryCapacity> telemetry_;
    std::uint64_t lastRealtimeCommandSequence_ {0U};
    std::atomic<std::uint64_t> invalidRealtimeCommands_ {0U};
    std::atomic<std::uint64_t> lostCommandCompletions_ {0U};

    std::unique_ptr<PreparedRenderPlan> livePlan_;

    struct RetiredPlan final {
        std::unique_ptr<PreparedRenderPlan> plan;
        std::uint64_t safeAfterGeneration {0U};
    };

    std::vector<RetiredPlan> retiredPlans_;
    std::uint64_t generationCounter_ {0U};
    int totalReclaimedPlans_ {0};

    std::vector<float> outputStorage_;
    std::vector<float*> outputPointers_;
    int outputChannelCount_ {0};
    int maximumBlockSize_ {0};
};

} // namespace iramix::audio
