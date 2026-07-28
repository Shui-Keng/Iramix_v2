#include "iramix/audio/RenderPlanExecutor.hpp"

#include "iramix/realtime/Audit.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace iramix::audio {

PreparedRenderPlan::~PreparedRenderPlan() {
    magic = kDeadMagic;
}

RenderPlanExecutor::~RenderPlanExecutor() {
    live_.store(nullptr, std::memory_order_release);
    retiredPlans_.clear();
    livePlan_.reset();
}

bool RenderPlanExecutor::prepareAndPublish(
    const RenderPlan& plan,
    const PrepareInfo& info,
    const NodeResolver& resolve,
    std::string& error
) {
    error.clear();
    if (!plan.valid) {
        error = "the render plan is invalid: " + plan.error;
        return false;
    }
    if (info.maximumBlockSize <= 0) {
        error = "maximumBlockSize must be positive";
        return false;
    }
    if (info.outputChannelCount <= 0) {
        error = "outputChannelCount must be positive";
        return false;
    }
    if (livePlan_ != nullptr
        && (info.outputChannelCount != outputChannelCount_
            || info.maximumBlockSize != maximumBlockSize_)) {
        error = "the device format changed under a live executor";
        return false;
    }

    auto fresh = std::make_unique<PreparedRenderPlan>();
    fresh->maximumBlockSize = info.maximumBlockSize;
    fresh->audioStorage.assign(
        static_cast<std::size_t>(plan.bufferCount)
            * static_cast<std::size_t>(info.maximumBlockSize),
        0.0F
    );
    fresh->bufferPointers.resize(
        static_cast<std::size_t>(plan.bufferCount)
    );
    for (int index = 0; index < plan.bufferCount; ++index) {
        fresh->bufferPointers[static_cast<std::size_t>(index)] =
            fresh->audioStorage.data()
            + static_cast<std::size_t>(index)
                * static_cast<std::size_t>(info.maximumBlockSize);
    }

    fresh->channelPointers.resize(plan.channelBuffers.size());
    for (std::size_t index = 0U;
         index < plan.channelBuffers.size();
         ++index) {
        const int buffer = plan.channelBuffers[index];
        if (buffer < 0 || buffer >= plan.bufferCount) {
            error = "the plan references a buffer outside its pool";
            return false;
        }
        fresh->channelPointers[index] =
            fresh->bufferPointers[static_cast<std::size_t>(buffer)];
    }

    fresh->delayLengths.assign(
        static_cast<std::size_t>(plan.delayLineCount),
        0
    );
    for (const auto& operation : plan.operations) {
        if (operation.type == RenderOpType::delay
            && operation.delayLine >= 0
            && operation.delayLine < plan.delayLineCount) {
            auto& length = fresh->delayLengths[
                static_cast<std::size_t>(operation.delayLine)
            ];
            length = std::max(length, operation.delaySamples);
        }
    }

    fresh->delayOffsets.assign(
        static_cast<std::size_t>(plan.delayLineCount),
        0
    );
    int totalDelaySamples = 0;
    for (int index = 0; index < plan.delayLineCount; ++index) {
        fresh->delayOffsets[static_cast<std::size_t>(index)] =
            totalDelaySamples;
        totalDelaySamples +=
            fresh->delayLengths[static_cast<std::size_t>(index)];
    }
    fresh->delayStorage.assign(
        static_cast<std::size_t>(std::max(1, totalDelaySamples)),
        0.0F
    );
    fresh->delayWritePositions.assign(
        static_cast<std::size_t>(plan.delayLineCount),
        0
    );

    fresh->midiBuffers.resize(
        static_cast<std::size_t>(plan.midiBufferCount)
    );
    for (auto& buffer : fresh->midiBuffers) {
        buffer.reserve(
            info.maximumMidiEventsPerNode,
            info.maximumMidiBytesPerNode
        );
    }
    fresh->parameterBuffers.resize(
        static_cast<std::size_t>(plan.midiBufferCount)
    );
    fresh->parameterNodeIds.assign(
        static_cast<std::size_t>(plan.midiBufferCount),
        0U
    );
    for (auto& buffer : fresh->parameterBuffers) {
        buffer.reserve(info.maximumParameterEventsPerNode);
    }

    if (livePlan_ == nullptr) {
        outputChannelCount_ = info.outputChannelCount;
        maximumBlockSize_ = info.maximumBlockSize;
        outputStorage_.assign(
            static_cast<std::size_t>(outputChannelCount_)
                * static_cast<std::size_t>(maximumBlockSize_),
            0.0F
        );
        outputPointers_.resize(
            static_cast<std::size_t>(outputChannelCount_)
        );
        for (int channel = 0;
             channel < outputChannelCount_;
             ++channel) {
            outputPointers_[static_cast<std::size_t>(channel)] =
                outputStorage_.data()
                + static_cast<std::size_t>(channel)
                    * static_cast<std::size_t>(maximumBlockSize_);
        }
    }
    fresh->outputSources.assign(
        static_cast<std::size_t>(outputChannelCount_),
        nullptr
    );

    const auto audioPointer =
        [&fresh, &plan](const int buffer) -> float* {
        if (buffer < 0 || buffer >= plan.bufferCount) {
            return nullptr;
        }
        return fresh->bufferPointers[static_cast<std::size_t>(buffer)];
    };
    const auto midiPointer =
        [&fresh, &plan](const int buffer) -> MidiEventBuffer* {
        if (buffer < 0 || buffer >= plan.midiBufferCount) {
            return nullptr;
        }
        return &fresh->midiBuffers[static_cast<std::size_t>(buffer)];
    };

    fresh->operations.reserve(plan.operations.size());
    fresh->nodeOwners.reserve(
        static_cast<std::size_t>(plan.midiBufferCount)
    );
    bool foundOutputNode = false;

    for (const auto& operation : plan.operations) {
        PreparedRenderPlan::ExecutionOp execution {
            .type = operation.type,
            .source = audioPointer(operation.sourceBuffer),
            .destination =
                audioPointer(operation.destinationBuffer),
            .sourceMidi = midiPointer(operation.sourceMidi),
            .destinationMidi =
                midiPointer(operation.destinationMidi),
            .delayLine = operation.delayLine,
            .delaySamples = operation.delaySamples,
        };

        switch (operation.type) {
        case RenderOpType::clear:
            if (execution.destination == nullptr) {
                error = "a clear operation has no destination";
                return false;
            }
            break;
        case RenderOpType::copy:
        case RenderOpType::add:
            if (execution.source == nullptr
                || execution.destination == nullptr) {
                error = "a copy/add operation references an invalid buffer";
                return false;
            }
            break;
        case RenderOpType::delay:
            if (execution.destination == nullptr
                || operation.delayLine < 0
                || operation.delayLine >= plan.delayLineCount
                || operation.delaySamples <= 0) {
                error = "a delay operation is invalid";
                return false;
            }
            break;
        case RenderOpType::midiClear:
            if (execution.destinationMidi == nullptr) {
                error = "a MIDI clear operation has no destination";
                return false;
            }
            break;
        case RenderOpType::midiMerge:
            if (execution.sourceMidi == nullptr
                || execution.destinationMidi == nullptr) {
                error = "a MIDI merge operation references an invalid buffer";
                return false;
            }
            break;
        case RenderOpType::process: {
            if (operation.firstChannel < 0
                || operation.channelCount < 0
                || operation.firstChannel + operation.channelCount
                    > static_cast<int>(
                        fresh->channelPointers.size()
                    )) {
                error = "a process operation references invalid channels";
                return false;
            }

            auto owner = resolve(operation.node);
            execution.node = owner.get();
            execution.nodeId = operation.node;
            execution.channelCount = operation.channelCount;
            execution.channels = operation.channelCount > 0
                ? fresh->channelPointers.data()
                    + operation.firstChannel
                : nullptr;
            if (owner != nullptr) {
                fresh->nodeOwners.push_back(std::move(owner));
            } else {
                ++fresh->unresolvedNodeCount;
            }

            if (operation.destinationMidi < 0
                || operation.destinationMidi
                    >= static_cast<int>(
                        fresh->parameterBuffers.size()
                    )) {
                error =
                    "a process operation has no parameter-event route";
                return false;
            }
            const auto parameterIndex = static_cast<std::size_t>(
                operation.destinationMidi
            );
            fresh->parameterNodeIds[parameterIndex] = operation.node;
            execution.parameters =
                &fresh->parameterBuffers[parameterIndex];

            if (operation.node == info.outputNode) {
                foundOutputNode = true;
                execution.capturesOutput = true;
                if (operation.channelCount < outputChannelCount_) {
                    error = "the output node has too few channels";
                    return false;
                }
                for (int channel = 0;
                     channel < outputChannelCount_;
                     ++channel) {
                    fresh->outputSources[
                        static_cast<std::size_t>(channel)
                    ] = fresh->channelPointers[
                        static_cast<std::size_t>(
                            operation.firstChannel + channel
                        )
                    ];
                }
            }
            break;
        }
        }

        fresh->operations.push_back(execution);
    }

    if (!foundOutputNode) {
        error = "the requested output node is absent from the plan";
        return false;
    }

    fresh->generation = ++generationCounter_;
    if (livePlan_ != nullptr) {
        retiredPlans_.push_back({
            .plan = std::move(livePlan_),
            .safeAfterGeneration = fresh->generation,
        });
    }
    livePlan_ = std::move(fresh);
    live_.store(livePlan_.get(), std::memory_order_release);
    static_cast<void>(reclaimRetiredPlans());
    return true;
}

bool RenderPlanExecutor::enqueueParameterEvent(
    const NodeId targetNode,
    const ParameterId parameter,
    const std::int64_t samplePosition,
    const float value
) noexcept {
    if (targetNode == 0U
        || parameter == 0U
        || samplePosition < 0
        || !std::isfinite(value)
        || samplePosition < lastEnqueuedParameterSample_) {
        invalidParameterEvents_.fetch_add(
            1U,
            std::memory_order_relaxed
        );
        return false;
    }

    const ScheduledParameterEvent event {
        .targetNode = targetNode,
        .parameter = parameter,
        .samplePosition = samplePosition,
        .value = value,
        .sequence = nextParameterSequence_++,
    };
    if (!parameterEvents_.tryPush(event)) {
        return false;
    }
    lastEnqueuedParameterSample_ = samplePosition;
    return true;
}

void RenderPlanExecutor::clearParameterEvents() noexcept {
    parameterEvents_.clear();
    lastEnqueuedParameterSample_ = -1;
    nextParameterSequence_ = 1U;
    invalidParameterEvents_.store(0U, std::memory_order_relaxed);
    lateParameterEvents_.store(0U, std::memory_order_relaxed);
    unknownParameterTargets_.store(0U, std::memory_order_relaxed);
    parameterBufferOverflows_.store(0U, std::memory_order_relaxed);
}

bool RenderPlanExecutor::enqueueRealtimeCommand(
    const RealtimeCommand& command
) noexcept {
    const bool validTypeAndTarget =
        (command.type == RealtimeCommandType::resetAllNodes
            && command.targetNode == 0U)
        || (command.type == RealtimeCommandType::resetNode
            && command.targetNode != 0U);
    if (command.sequence == 0U
        || command.sequence <= lastRealtimeCommandSequence_
        || !validTypeAndTarget) {
        invalidRealtimeCommands_.fetch_add(
            1U,
            std::memory_order_relaxed
        );
        return false;
    }
    if (!realtimeCommands_.tryPush(command)) {
        return false;
    }
    lastRealtimeCommandSequence_ = command.sequence;
    return true;
}

bool RenderPlanExecutor::tryPopCommandCompletion(
    RealtimeCommandCompletion& completion
) noexcept {
    return commandCompletions_.tryPop(completion);
}

bool RenderPlanExecutor::tryPopTelemetry(
    RealtimeBlockTelemetry& telemetry
) noexcept {
    return telemetry_.tryPop(telemetry);
}

void RenderPlanExecutor::clearRealtimeControl() noexcept {
    realtimeCommands_.clear();
    commandCompletions_.clear();
    telemetry_.clear();
    lastRealtimeCommandSequence_ = 0U;
    invalidRealtimeCommands_.store(0U, std::memory_order_relaxed);
    lostCommandCompletions_.store(0U, std::memory_order_relaxed);
}

void RenderPlanExecutor::process(
    int frameCount,
    const TransportSnapshot& transport
) noexcept {
    const realtime::CallbackScope callbackScope;
    auto* const plan = live_.load(std::memory_order_acquire);
    if (plan == nullptr || frameCount <= 0) {
        return;
    }
    if (plan->magic != PreparedRenderPlan::kLiveMagic) {
        useAfterFree_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (frameCount > plan->maximumBlockSize) {
        frameCount = plan->maximumBlockSize;
        oversizedBlocks_.fetch_add(1U, std::memory_order_relaxed);
    }

    execute(*plan, frameCount, transport);
    const auto previous = lastObservedGeneration_.exchange(
        plan->generation,
        std::memory_order_relaxed
    );
    if (previous != plan->generation) {
        observedSwaps_.fetch_add(1U, std::memory_order_relaxed);
    }
    renderedBlocks_.fetch_add(1U, std::memory_order_relaxed);
    acknowledgedGeneration_.store(
        plan->generation,
        std::memory_order_release
    );
}

void RenderPlanExecutor::renderTo(
    const AudioBufferView destination,
    const TransportSnapshot& transport
) noexcept {
    const realtime::CallbackScope callbackScope;
    const int requestedFrames = destination.frameCount();
    process(requestedFrames, transport);

    const int renderedFrames = std::min(
        requestedFrames,
        maximumBlockSize_
    );
    const bool hasLivePlan =
        live_.load(std::memory_order_acquire) != nullptr;

    for (int channel = 0;
         channel < destination.channelCount();
         ++channel) {
        float* const output = destination.channel(channel);
        const float* const source =
            hasLivePlan && channel < outputChannelCount_
            ? outputChannel(channel)
            : nullptr;
        if (source != nullptr && renderedFrames > 0) {
            std::memcpy(
                output,
                source,
                static_cast<std::size_t>(renderedFrames)
                    * sizeof(float)
            );
        } else if (renderedFrames > 0) {
            std::fill_n(output, renderedFrames, 0.0F);
        }
        if (renderedFrames < requestedFrames) {
            std::fill(
                output + renderedFrames,
                output + requestedFrames,
                0.0F
            );
        }
    }
}

void RenderPlanExecutor::execute(
    PreparedRenderPlan& plan,
    const int frameCount,
    const TransportSnapshot& transport
) noexcept {
    const auto bytes =
        static_cast<std::size_t>(frameCount) * sizeof(float);
    const auto commandResult = drainRealtimeCommands(plan);
    const auto routedParameterEvents =
        routeParameterEvents(plan, frameCount, transport);

    for (const auto& operation : plan.operations) {
        switch (operation.type) {
        case RenderOpType::clear:
            std::memset(operation.destination, 0, bytes);
            break;
        case RenderOpType::copy:
            std::memcpy(operation.destination, operation.source, bytes);
            break;
        case RenderOpType::add:
            for (int frame = 0; frame < frameCount; ++frame) {
                operation.destination[frame] += operation.source[frame];
            }
            break;
        case RenderOpType::delay: {
            float* const line = plan.delayStorage.data()
                + plan.delayOffsets[
                    static_cast<std::size_t>(operation.delayLine)
                ];
            int writePosition = plan.delayWritePositions[
                static_cast<std::size_t>(operation.delayLine)
            ];
            for (int frame = 0; frame < frameCount; ++frame) {
                const float delayed = line[writePosition];
                line[writePosition] = operation.destination[frame];
                operation.destination[frame] = delayed;
                writePosition =
                    writePosition + 1 < operation.delaySamples
                    ? writePosition + 1
                    : 0;
            }
            plan.delayWritePositions[
                static_cast<std::size_t>(operation.delayLine)
            ] = writePosition;
            break;
        }
        case RenderOpType::midiClear:
            operation.destinationMidi->clear();
            break;
        case RenderOpType::midiMerge:
            static_cast<void>(
                operation.destinationMidi->mergeFrom(
                    *operation.sourceMidi
                )
            );
            break;
        case RenderOpType::process:
            if (operation.node != nullptr) {
                operation.node->process({
                    .audio = AudioBufferView {
                        operation.channels,
                        operation.channelCount,
                        frameCount,
                    },
                    .midi = operation.destinationMidi,
                    .parameters = operation.parameters,
                    .transport = &transport,
                });
            }
            if (operation.capturesOutput) {
                for (int channel = 0;
                     channel < outputChannelCount_;
                     ++channel) {
                    std::memcpy(
                        outputPointers_[
                            static_cast<std::size_t>(channel)
                        ],
                        plan.outputSources[
                            static_cast<std::size_t>(channel)
                        ],
                        bytes
                    );
                }
            }
            break;
        }
    }

    static_cast<void>(telemetry_.tryPush({
        .generation = plan.generation,
        .samplePosition = transport.samplePosition,
        .frameCount = static_cast<std::uint32_t>(frameCount),
        .commandsApplied = commandResult.applied,
        .commandsRejected = commandResult.rejected,
        .parameterEventsRouted = routedParameterEvents,
    }));
}

std::uint32_t RenderPlanExecutor::routeParameterEvents(
    PreparedRenderPlan& plan,
    const int frameCount,
    const TransportSnapshot& transport
) noexcept {
    for (auto& buffer : plan.parameterBuffers) {
        buffer.clear();
    }

    const auto blockStart = transport.samplePosition;
    const auto maximumPosition =
        std::numeric_limits<std::int64_t>::max();
    const auto blockEnd =
        blockStart > maximumPosition - frameCount
        ? maximumPosition
        : blockStart + frameCount;

    ScheduledParameterEvent scheduled;
    std::uint32_t routed = 0U;
    while (parameterEvents_.tryPeek(scheduled)) {
        if (scheduled.samplePosition >= blockEnd) {
            break;
        }
        if (!parameterEvents_.tryPop(scheduled)) {
            break;
        }

        int offset = 0;
        if (scheduled.samplePosition < blockStart) {
            lateParameterEvents_.fetch_add(
                1U,
                std::memory_order_relaxed
            );
        } else {
            offset = static_cast<int>(
                scheduled.samplePosition - blockStart
            );
        }

        std::size_t route = plan.parameterNodeIds.size();
        for (std::size_t index = 0U;
             index < plan.parameterNodeIds.size();
             ++index) {
            if (plan.parameterNodeIds[index]
                == scheduled.targetNode) {
                route = index;
                break;
            }
        }
        if (route == plan.parameterNodeIds.size()) {
            unknownParameterTargets_.fetch_add(
                1U,
                std::memory_order_relaxed
            );
            continue;
        }

        if (!plan.parameterBuffers[route].add({
                .parameter = scheduled.parameter,
                .sampleOffset = offset,
                .value = scheduled.value,
                .sequence = scheduled.sequence,
            })) {
            parameterBufferOverflows_.fetch_add(
                1U,
                std::memory_order_relaxed
            );
        } else {
            ++routed;
        }
    }
    return routed;
}

RenderPlanExecutor::CommandDrainResult
RenderPlanExecutor::drainRealtimeCommands(
    PreparedRenderPlan& plan
) noexcept {
    CommandDrainResult result;
    RealtimeCommand command;
    for (std::uint32_t index = 0U;
         index < kMaximumCommandsPerBlock
            && !commandCompletions_.full()
            && realtimeCommands_.tryPop(command);
         ++index) {
        auto status = CommandCompletionStatus::applied;

        if (command.type == RealtimeCommandType::resetAllNodes) {
            std::fill(
                plan.delayStorage.begin(),
                plan.delayStorage.end(),
                0.0F
            );
            std::fill(
                plan.delayWritePositions.begin(),
                plan.delayWritePositions.end(),
                0
            );
            for (const auto& operation : plan.operations) {
                if (operation.type == RenderOpType::process
                    && operation.node != nullptr) {
                    operation.node->reset();
                }
            }
        } else if (command.type == RealtimeCommandType::resetNode) {
            bool found = false;
            for (const auto& operation : plan.operations) {
                if (operation.type == RenderOpType::process
                    && operation.node != nullptr
                    && operation.nodeId == command.targetNode) {
                    operation.node->reset();
                    found = true;
                    break;
                }
            }
            if (!found) {
                status =
                    CommandCompletionStatus::rejectedUnknownTarget;
            }
        } else {
            status =
                CommandCompletionStatus::rejectedInvalidCommand;
        }

        if (status == CommandCompletionStatus::applied) {
            ++result.applied;
        } else {
            ++result.rejected;
        }

        if (!commandCompletions_.tryPush({
                .sequence = command.sequence,
                .status = status,
                .targetNode = command.targetNode,
                .appliedGeneration = plan.generation,
            })) {
            lostCommandCompletions_.fetch_add(
                1U,
                std::memory_order_relaxed
            );
        }
    }
    return result;
}

const float* RenderPlanExecutor::outputChannel(
    const int channel
) const noexcept {
    if (channel < 0 || channel >= outputChannelCount_) {
        return nullptr;
    }
    return outputPointers_[static_cast<std::size_t>(channel)];
}

void RenderPlanExecutor::reset() noexcept {
    auto* const plan = live_.load(std::memory_order_acquire);
    if (plan == nullptr) {
        return;
    }
    std::fill(
        plan->audioStorage.begin(),
        plan->audioStorage.end(),
        0.0F
    );
    std::fill(
        plan->delayStorage.begin(),
        plan->delayStorage.end(),
        0.0F
    );
    std::fill(
        plan->delayWritePositions.begin(),
        plan->delayWritePositions.end(),
        0
    );
    std::fill(
        outputStorage_.begin(),
        outputStorage_.end(),
        0.0F
    );
    for (auto& buffer : plan->midiBuffers) {
        buffer.clear();
        buffer.resetDroppedEventCount();
    }
    for (const auto& operation : plan->operations) {
        if (operation.type == RenderOpType::process
            && operation.node != nullptr) {
            operation.node->reset();
        }
    }
}

int RenderPlanExecutor::reclaimRetiredPlans() {
    const auto acknowledged =
        acknowledgedGeneration_.load(std::memory_order_acquire);
    const auto previousCount = retiredPlans_.size();
    retiredPlans_.erase(
        std::remove_if(
            retiredPlans_.begin(),
            retiredPlans_.end(),
            [acknowledged](const RetiredPlan& retired) {
                return retired.safeAfterGeneration <= acknowledged;
            }
        ),
        retiredPlans_.end()
    );
    const int reclaimed = static_cast<int>(
        previousCount - retiredPlans_.size()
    );
    totalReclaimedPlans_ += reclaimed;
    return reclaimed;
}

int RenderPlanExecutor::retiredPlanCount() const noexcept {
    return static_cast<int>(retiredPlans_.size());
}

void RenderPlanExecutor::flushRetiredPlans() {
    retiredPlans_.clear();
}

void RenderPlanExecutor::releaseAll() {
    live_.store(nullptr, std::memory_order_release);
    retiredPlans_.clear();
    livePlan_.reset();
    outputStorage_.clear();
    outputPointers_.clear();
    outputChannelCount_ = 0;
    maximumBlockSize_ = 0;
    clearParameterEvents();
    clearRealtimeControl();
}

int RenderPlanExecutor::unresolvedNodeCount() const noexcept {
    return livePlan_ != nullptr
        ? livePlan_->unresolvedNodeCount
        : 0;
}

std::uint64_t RenderPlanExecutor::droppedMidiEventCount() const noexcept {
    if (livePlan_ == nullptr) {
        return 0U;
    }
    std::uint64_t result = 0U;
    for (const auto& buffer : livePlan_->midiBuffers) {
        result += buffer.droppedEventCount();
    }
    return result;
}

std::uint64_t RenderPlanExecutor::rejectedParameterEventCount()
    const noexcept {
    return invalidParameterEvents_.load(std::memory_order_relaxed)
        + parameterEvents_.rejectedPushCount();
}

std::uint64_t RenderPlanExecutor::rejectedRealtimeCommandCount()
    const noexcept {
    return invalidRealtimeCommands_.load(std::memory_order_relaxed)
        + realtimeCommands_.rejectedPushCount();
}

} // namespace iramix::audio
