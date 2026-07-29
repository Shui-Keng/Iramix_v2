#pragma once

#include "iramix/audio/Graph.hpp"
#include "iramix/audio/Nodes.hpp"
#include "iramix/audio/RenderPlan.hpp"
#include "iramix/audio/RenderPlanExecutor.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace iramix::audio_probe {

constexpr std::uint32_t kGraphSampleRate = 48'000U;
constexpr std::uint16_t kGraphChannels = 2U;

// The same production node chain for every backend probe. A backend that
// measures a different workload cannot be compared against one that does,
// and the 64-frame question is exactly a cross-backend comparison, so the
// device-format conversion is left to the caller and the graph is not.
class StereoGraphWorkload final {
public:
    explicit StereoGraphWorkload(const std::uint32_t maximumFrames)
        : planarStorage_(
              static_cast<std::size_t>(maximumFrames) * kGraphChannels,
              0.0F
          ) {
        for (std::size_t channel = 0U;
             channel < planarPointers_.size();
             ++channel) {
            planarPointers_[channel] = planarStorage_.data()
                + channel * maximumFrames;
        }

        maximumFrames_ = maximumFrames;

        audio::GraphDescription graph;
        for (const auto id : {
                 kInputId,
                 kTrackId,
                 kGainId,
                 kMixerId,
                 kOutputId,
             }) {
            if (!graph.addNode(id)) {
                throw std::runtime_error {
                    "Failed to create the probe graph workload"
                };
            }
        }
        for (const auto source : {
                 kInputId,
                 kTrackId,
                 kGainId,
                 kMixerId,
             }) {
            for (int channel = 0;
                 channel < static_cast<int>(kGraphChannels);
                 ++channel) {
                if (!graph.addConnection({
                        source,
                        channel,
                        source + 1U,
                        channel,
                    })) {
                    throw std::runtime_error {
                        "Failed to connect the probe graph workload"
                    };
                }
            }
        }

        const audio::NodeInfoMap nodeInfo {
            {kInputId, {0, 2, 0}},
            {kTrackId, {2, 2, 0}},
            {kGainId, {2, 2, 0}},
            {kMixerId, {2, 2, 0}},
            {kOutputId, {2, 0, 0}},
        };
        plan_ = audio::compileRenderPlan(graph, nodeInfo);
        if (!plan_.valid) {
            throw std::runtime_error {
                "Failed to compile the probe graph workload: "
                + plan_.error
            };
        }

        const std::span<const audio::AudioBusLayout> noBuses;
        const std::span<const audio::AudioBusLayout> stereo {
            stereoBus_
        };
        const auto prepareInfo = [maximumFrames](
            const std::span<const audio::AudioBusLayout> inputs,
            const std::span<const audio::AudioBusLayout> outputs
        ) {
            return audio::NodePrepareInfo {
                .sampleRate = static_cast<double>(kGraphSampleRate),
                .maxBlockSize = static_cast<int>(maximumFrames),
                .inputBuses = inputs,
                .outputBuses = outputs,
                .maxMidiEvents = 256,
                .maxMidiBytes = 1'024,
            };
        };

        input_->prepare(prepareInfo(noBuses, stereo));
        track_->prepare(prepareInfo(stereo, stereo));
        gain_->prepare(prepareInfo(stereo, stereo));
        mixer_->prepare(prepareInfo(stereo, stereo));
        output_->prepare(prepareInfo(stereo, noBuses));
        gain_->setGain(0.5F);

        std::string error;
        if (!executor_.prepareAndPublish(
                plan_,
                {
                    .maximumBlockSize =
                        static_cast<int>(maximumFrames),
                    .maximumMidiEventsPerNode = 256,
                    .maximumMidiBytesPerNode = 1'024,
                    .maximumParameterEventsPerNode = 256,
                    .outputNode = kOutputId,
                    .outputChannelCount =
                        static_cast<int>(kGraphChannels),
                },
                [this](const audio::NodeId id)
                    -> std::shared_ptr<audio::IAudioNode> {
                    switch (id) {
                    case kInputId:
                        return input_;
                    case kTrackId:
                        return track_;
                    case kGainId:
                        return gain_;
                    case kMixerId:
                        return mixer_;
                    case kOutputId:
                        return output_;
                    default:
                        return {};
                    }
                },
                error
            )) {
            throw std::runtime_error {
                "Failed to prepare the probe graph workload: " + error
            };
        }
    }

    // Renders one block into the workload's own planar storage. The
    // caller converts that into whatever the device expects, which is
    // the only part that differs between WASAPI and ASIO.
    void renderBlock(
        const std::uint32_t frameCount,
        const std::uint64_t samplePosition
    ) noexcept {
        lastRenderedSamplePosition_.store(
            samplePosition,
            std::memory_order_release
        );
        executor_.renderTo(
            planarView(frameCount),
            {
                .samplePosition =
                    static_cast<std::int64_t>(samplePosition),
                .seconds =
                    static_cast<double>(samplePosition)
                    / static_cast<double>(kGraphSampleRate),
                .quarterNotePosition =
                    static_cast<double>(samplePosition)
                    / static_cast<double>(kGraphSampleRate)
                    * 2.0,
                .tempo = 120.0,
                .playing = true,
            }
        );
    }

    [[nodiscard]] audio::AudioBufferView planarView(
        const std::uint32_t frameCount
    ) noexcept {
        return {
            planarPointers_.data(),
            static_cast<int>(kGraphChannels),
            static_cast<int>(frameCount),
        };
    }

    [[nodiscard]] std::uint64_t renderedBlocks() const noexcept {
        return executor_.renderedBlockCount();
    }

    [[nodiscard]] std::uint64_t generation() const noexcept {
        return executor_.generation();
    }

    [[nodiscard]] bool publishGainVariant(const float gain) {
        auto replacement = std::make_shared<audio::GainNode>(gain);
        const std::span<const audio::AudioBusLayout> stereo {
            stereoBus_
        };
        replacement->prepare({
            .sampleRate = static_cast<double>(kGraphSampleRate),
            .maxBlockSize = static_cast<int>(maximumFrames_),
            .inputBuses = stereo,
            .outputBuses = stereo,
            .maxMidiEvents = 256,
            .maxMidiBytes = 1'024,
        });

        std::string error;
        return executor_.prepareAndPublish(
            plan_,
            {
                .maximumBlockSize =
                    static_cast<int>(maximumFrames_),
                .maximumMidiEventsPerNode = 256,
                .maximumMidiBytesPerNode = 1'024,
                .maximumParameterEventsPerNode = 256,
                .outputNode = kOutputId,
                .outputChannelCount =
                    static_cast<int>(kGraphChannels),
            },
            [this, replacement = std::move(replacement)](
                const audio::NodeId id
            ) -> std::shared_ptr<audio::IAudioNode> {
                switch (id) {
                case kInputId:
                    return input_;
                case kTrackId:
                    return track_;
                case kGainId:
                    return replacement;
                case kMixerId:
                    return mixer_;
                case kOutputId:
                    return output_;
                default:
                    return {};
                }
            },
            error
        );
    }

    [[nodiscard]] bool enqueueGainAutomation(
        const std::int64_t samplePosition,
        const int rampSamples
    ) noexcept {
        if (!executor_.enqueueParameterRamp(
                kGainId,
                audio::GainNode::kGainParameter,
                samplePosition,
                1.0F,
                rampSamples
            )) {
            return false;
        }

        constexpr std::array<float, 4> modulation {
            0.0F,
            0.125F,
            -0.125F,
            0.0F,
        };
        for (int index = 0; index < 4; ++index) {
            if (!executor_.enqueueParameterModulation(
                    kGainId,
                    audio::GainNode::kGainParameter,
                    samplePosition + rampSamples + index,
                    modulation[static_cast<std::size_t>(index)]
                )) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool enqueueMixerReset(
        const std::uint64_t sequence
    ) noexcept {
        return executor_.enqueueRealtimeCommand({
            .sequence = sequence,
            .type = audio::RealtimeCommandType::resetNode,
            .targetNode = kMixerId,
        });
    }

    [[nodiscard]] static constexpr audio::NodeId mixerNodeId()
        noexcept {
        return kMixerId;
    }

    [[nodiscard]] bool tryPopCommandCompletion(
        audio::RealtimeCommandCompletion& completion
    ) noexcept {
        return executor_.tryPopCommandCompletion(completion);
    }

    [[nodiscard]] std::uint64_t drainTelemetry() noexcept {
        std::uint64_t count = 0U;
        audio::RealtimeBlockTelemetry telemetry;
        while (executor_.tryPopTelemetry(telemetry)) {
            ++count;
        }
        return count;
    }

    [[nodiscard]] std::uint64_t acknowledgedGeneration()
        const noexcept {
        return executor_.acknowledgedGeneration();
    }

    [[nodiscard]] std::uint64_t lastRenderedSamplePosition()
        const noexcept {
        return lastRenderedSamplePosition_.load(
            std::memory_order_acquire
        );
    }

    [[nodiscard]] std::uint64_t observedSwaps() const noexcept {
        return executor_.observedSwapCount();
    }

    [[nodiscard]] std::uint64_t pendingParameterEvents()
        const noexcept {
        return executor_.pendingParameterEventCount();
    }

    [[nodiscard]] std::uint64_t rejectedParameterEvents()
        const noexcept {
        return executor_.rejectedParameterEventCount();
    }

    [[nodiscard]] std::uint64_t lateParameterEvents() const noexcept {
        return executor_.lateParameterEventCount();
    }

    [[nodiscard]] std::uint64_t parameterBufferOverflows()
        const noexcept {
        return executor_.parameterBufferOverflowCount();
    }

    [[nodiscard]] std::uint64_t rejectedRealtimeCommands()
        const noexcept {
        return executor_.rejectedRealtimeCommandCount();
    }

    [[nodiscard]] std::uint64_t lostCommandCompletions()
        const noexcept {
        return executor_.lostCommandCompletionCount();
    }

    [[nodiscard]] std::uint64_t pendingRealtimeCommands()
        const noexcept {
        return executor_.pendingRealtimeCommandCount();
    }

    [[nodiscard]] std::uint64_t droppedTelemetry()
        const noexcept {
        return executor_.droppedTelemetryCount();
    }

    [[nodiscard]] int reclaimRetiredPlans() {
        return executor_.reclaimRetiredPlans();
    }

private:
    static constexpr audio::NodeId kInputId = 1U;
    static constexpr audio::NodeId kTrackId = 2U;
    static constexpr audio::NodeId kGainId = 3U;
    static constexpr audio::NodeId kMixerId = 4U;
    static constexpr audio::NodeId kOutputId = 5U;

    std::array<audio::AudioBusLayout, 1> stereoBus_ {{
        {
            static_cast<int>(kGraphChannels),
            audio::AudioBusRole::main,
            true,
        },
    }};
    std::shared_ptr<audio::DeviceInputNode> input_ {
        std::make_shared<audio::DeviceInputNode>()
    };
    std::shared_ptr<audio::TrackNode> track_ {
        std::make_shared<audio::TrackNode>()
    };
    std::shared_ptr<audio::GainNode> gain_ {
        std::make_shared<audio::GainNode>()
    };
    std::shared_ptr<audio::MixerNode> mixer_ {
        std::make_shared<audio::MixerNode>()
    };
    std::shared_ptr<audio::OutputNode> output_ {
        std::make_shared<audio::OutputNode>()
    };
    audio::RenderPlanExecutor executor_;
    audio::RenderPlan plan_;
    std::vector<float> planarStorage_;
    std::array<float*, kGraphChannels> planarPointers_ {};
    std::atomic<std::uint64_t> lastRenderedSamplePosition_ {0U};
    std::uint32_t maximumFrames_ {0U};
};

} // namespace iramix::audio_probe
