#include "AudioProbe.hpp"
#include "iramix/audio/DeviceBufferConversion.hpp"
#include "iramix/audio/Graph.hpp"
#include "iramix/audio/Nodes.hpp"
#include "iramix/audio/RenderPlan.hpp"
#include "iramix/audio/RenderPlanExecutor.hpp"
#include "iramix/realtime/Audit.hpp"

#include <audioclient.h>
#include <audiosessiontypes.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace iramix::audio_probe {
namespace {

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kSampleRate = 48'000U;
constexpr std::uint16_t kChannels = 2U;
constexpr std::uint32_t kWarmupCallbacks = 100U;

class StereoGraphWorkload final {
public:
    explicit StereoGraphWorkload(const std::uint32_t maximumFrames)
        : planarStorage_(
              static_cast<std::size_t>(maximumFrames) * kChannels,
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
                    "Failed to create the WASAPI graph workload"
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
                 channel < static_cast<int>(kChannels);
                 ++channel) {
                if (!graph.addConnection({
                        source,
                        channel,
                        source + 1U,
                        channel,
                    })) {
                    throw std::runtime_error {
                        "Failed to connect the WASAPI graph workload"
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
                "Failed to compile the WASAPI graph workload: "
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
                .sampleRate = static_cast<double>(kSampleRate),
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
                        static_cast<int>(kChannels),
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
                "Failed to prepare the WASAPI graph workload: " + error
            };
        }
    }

    void render(
        BYTE* const deviceBuffer,
        const std::uint32_t frameCount,
        const bool floatingPointOutput,
        const std::uint64_t samplePosition
    ) noexcept {
        lastRenderedSamplePosition_.store(
            samplePosition,
            std::memory_order_release
        );
        executor_.renderTo(
            {
                planarPointers_.data(),
                static_cast<int>(kChannels),
                static_cast<int>(frameCount),
            },
            {
                .samplePosition =
                    static_cast<std::int64_t>(samplePosition),
                .seconds =
                    static_cast<double>(samplePosition)
                    / static_cast<double>(kSampleRate),
                .quarterNotePosition =
                    static_cast<double>(samplePosition)
                    / static_cast<double>(kSampleRate)
                    * 2.0,
                .tempo = 120.0,
                .playing = true,
            }
        );

        if (floatingPointOutput) {
            audio::interleaveFloat32(
                audio::AudioBufferView {
                    planarPointers_.data(),
                    static_cast<int>(kChannels),
                    static_cast<int>(frameCount),
                }.asConst(),
                reinterpret_cast<float*>(deviceBuffer)
            );
            return;
        }

        audio::interleavePcm16(
            audio::AudioBufferView {
                planarPointers_.data(),
                static_cast<int>(kChannels),
                static_cast<int>(frameCount),
            }.asConst(),
            reinterpret_cast<std::int16_t*>(deviceBuffer)
        );
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
            .sampleRate = static_cast<double>(kSampleRate),
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
                .outputChannelCount = static_cast<int>(kChannels),
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
            static_cast<int>(kChannels),
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
    std::array<float*, kChannels> planarPointers_ {};
    std::atomic<std::uint64_t> lastRenderedSamplePosition_ {0U};
    std::uint32_t maximumFrames_ {0U};
};

struct EventHandle {
    HANDLE value {nullptr};

    ~EventHandle() {
        if (value != nullptr) {
            CloseHandle(value);
        }
    }
};

struct MmcssHandle {
    HANDLE value {nullptr};

    ~MmcssHandle() {
        if (value != nullptr) {
            AvRevertMmThreadCharacteristics(value);
        }
    }
};

struct ComScope {
    HRESULT result {CoInitializeEx(nullptr, COINIT_MULTITHREADED)};

    ~ComScope() {
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
    }
};

struct ProbeResult {
    std::uint32_t requestedFrames {0U};
    std::uint32_t streamBufferFrames {0U};
    std::uint32_t minimumPeriodFrames {0U};
    std::uint32_t maximumPeriodFrames {0U};
    std::uint32_t fundamentalPeriodFrames {0U};
    std::uint64_t callbacks {0U};
    std::uint64_t lateWakeups {0U};
    std::uint64_t waitTimeouts {0U};
    std::uint64_t targetMisses {0U};
    std::uint64_t hardDeadlineMisses {0U};
    std::uint64_t p50Nanoseconds {0U};
    std::uint64_t p95Nanoseconds {0U};
    std::uint64_t p99Nanoseconds {0U};
    std::uint64_t maximumNanoseconds {0U};
    realtime::AuditSnapshot audit;
    bool supported {false};
    bool mmcssEnabled {false};
    std::uint64_t graphBlocks {0U};
    std::uint64_t graphGeneration {0U};
    std::uint64_t graphObservedSwaps {0U};
    std::uint64_t pendingParameterEvents {0U};
    std::uint64_t rejectedParameterEvents {0U};
    std::uint64_t lateParameterEvents {0U};
    std::uint64_t parameterBufferOverflows {0U};
    std::uint64_t rejectedRealtimeCommands {0U};
    std::uint64_t lostCommandCompletions {0U};
    std::uint64_t pendingRealtimeCommands {0U};
    std::uint64_t telemetryReceived {0U};
    std::uint64_t telemetryDropped {0U};
    int reclaimedPlans {0};
    bool hotSwapCompleted {false};
    bool automationQueued {false};
    bool resetCommandQueued {false};
    bool resetCommandApplied {false};
    bool graphControlFailed {false};
    std::string backend;
    std::string status;
};

[[noreturn]] void fail(const char* operation, const HRESULT result) {
    throw std::runtime_error(
        std::string {operation}
        + " failed with HRESULT "
        + std::to_string(static_cast<std::uint32_t>(result))
    );
}

void requireSuccess(const HRESULT result, const char* operation) {
    if (FAILED(result)) {
        fail(operation, result);
    }
}

[[nodiscard]] WAVEFORMATEXTENSIBLE makeFormat() {
    WAVEFORMATEXTENSIBLE format {};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = kChannels;
    format.Format.nSamplesPerSec = kSampleRate;
    format.Format.wBitsPerSample = 32U;
    format.Format.nBlockAlign = static_cast<WORD>(
        kChannels * sizeof(float)
    );
    format.Format.nAvgBytesPerSec =
        kSampleRate * format.Format.nBlockAlign;
    format.Format.cbSize = 22U;
    format.Samples.wValidBitsPerSample = 32U;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return format;
}

[[nodiscard]] WAVEFORMATEXTENSIBLE makePcm16Format() {
    auto format = makeFormat();
    format.Format.wBitsPerSample = 16U;
    format.Format.nBlockAlign = static_cast<WORD>(
        kChannels * sizeof(std::int16_t)
    );
    format.Format.nAvgBytesPerSec =
        kSampleRate * format.Format.nBlockAlign;
    format.Samples.wValidBitsPerSample = 16U;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    return format;
}

[[nodiscard]] std::uint64_t targetNanoseconds(
    const std::uint32_t frames
) {
    switch (frames) {
    case 64U:
        return 930'000U;
    case 128U:
        return 1'870'000U;
    case 256U:
        return 3'730'000U;
    default:
        return 0U;
    }
}

[[nodiscard]] std::uint64_t percentile(
    const std::vector<std::uint64_t>& sorted,
    const double value
) {
    const auto rank = static_cast<std::size_t>(
        std::ceil(value * static_cast<double>(sorted.size()))
    );
    return sorted[std::max<std::size_t>(1U, rank) - 1U];
}

[[nodiscard]] std::uint64_t processAudio(
    StereoGraphWorkload& workload,
    BYTE* const deviceBuffer,
    const std::uint32_t frameCount,
    const bool floatingPointOutput,
    const std::uint64_t samplePosition
) noexcept {
    const auto started = Clock::now();
    {
        realtime::CallbackScope callback;
        workload.render(
            deviceBuffer,
            frameCount,
            floatingPointOutput,
            samplePosition
        );
    }
    const auto elapsed = Clock::now() - started;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            elapsed
        ).count()
    );
}

[[nodiscard]] bool validPeriod(
    const std::uint32_t requested,
    const std::uint32_t minimum,
    const std::uint32_t maximum,
    const std::uint32_t fundamental
) {
    return fundamental != 0U
        && requested >= minimum
        && requested <= maximum
        && ((requested - minimum) % fundamental) == 0U;
}

ProbeResult runConfiguration(
    IMMDevice* device,
    const std::uint32_t requestedFrames,
    const std::uint32_t seconds
) {
    ProbeResult result {};
    result.requestedFrames = requestedFrames;
    ComPtr<IAudioClient3> client;
    requireSuccess(
        device->Activate(
            __uuidof(IAudioClient3),
            CLSCTX_ALL,
            nullptr,
            &client
        ),
        "IMMDevice::Activate"
    );

    AudioClientProperties properties {};
    properties.cbSize = sizeof(properties);
    properties.eCategory = AudioCategory_Media;
    requireSuccess(
        client->SetClientProperties(&properties),
        "IAudioClient2::SetClientProperties"
    );

    auto format = makeFormat();
    auto pcm16Format = makePcm16Format();
    std::uint32_t defaultPeriod = 0U;
    requireSuccess(
        client->GetSharedModeEnginePeriod(
            &format.Format,
            &defaultPeriod,
            &result.fundamentalPeriodFrames,
            &result.minimumPeriodFrames,
            &result.maximumPeriodFrames
        ),
        "IAudioClient3::GetSharedModeEnginePeriod"
    );

    const auto sharedPeriodSupported = validPeriod(
            requestedFrames,
            result.minimumPeriodFrames,
            result.maximumPeriodFrames,
            result.fundamentalPeriodFrames
        );

    EventHandle event {
        CreateEventW(nullptr, FALSE, FALSE, nullptr)
    };
    if (event.value == nullptr) {
        fail("CreateEventW", HRESULT_FROM_WIN32(GetLastError()));
    }

    bool exclusive = false;
    bool floatingPointOutput = true;
    if (sharedPeriodSupported) {
        result.backend = "WASAPI_shared";
        requireSuccess(
            client->InitializeSharedAudioStream(
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                    | AUDCLNT_STREAMFLAGS_NOPERSIST,
                requestedFrames,
                &format.Format,
                nullptr
            ),
            "IAudioClient3::InitializeSharedAudioStream"
        );
    } else {
        exclusive = true;
        result.backend = "WASAPI_exclusive";
        auto* exclusiveFormat = &format.Format;
        auto formatSupport = client->IsFormatSupported(
            AUDCLNT_SHAREMODE_EXCLUSIVE,
            exclusiveFormat,
            nullptr
        );
        if (formatSupport != S_OK) {
            exclusiveFormat = &pcm16Format.Format;
            floatingPointOutput = false;
            formatSupport = client->IsFormatSupported(
                AUDCLNT_SHAREMODE_EXCLUSIVE,
                exclusiveFormat,
                nullptr
            );
        }
        if (formatSupport != S_OK) {
            result.status = "unsupported_exclusive_format";
            return result;
        }

        const auto period = static_cast<REFERENCE_TIME>(
            std::llround(
                static_cast<long double>(requestedFrames)
                * 10'000'000.0L
                / static_cast<long double>(kSampleRate)
            )
        );
        const auto initializeResult = client->Initialize(
            AUDCLNT_SHAREMODE_EXCLUSIVE,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                | AUDCLNT_STREAMFLAGS_NOPERSIST,
            period,
            period,
            exclusiveFormat,
            nullptr
        );
        if (initializeResult == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            requireSuccess(
                client->GetBufferSize(&result.streamBufferFrames),
                "IAudioClient::GetBufferSize(aligned)"
            );
            result.status = "unsupported_buffer_alignment";
            return result;
        }
        if (initializeResult == AUDCLNT_E_INVALID_DEVICE_PERIOD) {
            result.status = "unsupported_device_period";
            return result;
        }
        requireSuccess(
            initializeResult,
            "IAudioClient::Initialize(exclusive)"
        );
    }
    requireSuccess(
        client->SetEventHandle(event.value),
        "IAudioClient::SetEventHandle"
    );
    requireSuccess(
        client->GetBufferSize(&result.streamBufferFrames),
        "IAudioClient::GetBufferSize"
    );
    if (result.streamBufferFrames != requestedFrames) {
        result.status = "unsupported_actual_buffer_size";
        return result;
    }

    ComPtr<IAudioRenderClient> renderer;
    requireSuccess(
        client->GetService(__uuidof(IAudioRenderClient), &renderer),
        "IAudioClient::GetService"
    );

    BYTE* initialBuffer = nullptr;
    requireSuccess(
        renderer->GetBuffer(result.streamBufferFrames, &initialBuffer),
        "IAudioRenderClient::GetBuffer(initial)"
    );
    requireSuccess(
        renderer->ReleaseBuffer(
            result.streamBufferFrames,
            AUDCLNT_BUFFERFLAGS_SILENT
        ),
        "IAudioRenderClient::ReleaseBuffer(initial)"
    );

    StereoGraphWorkload workload {requestedFrames};
    std::atomic<bool> requestGraphControl {false};
    std::atomic<bool> hotSwapCompleted {false};
    std::atomic<bool> automationQueued {false};
    std::atomic<bool> resetCommandQueued {false};
    std::atomic<bool> resetCommandApplied {false};
    std::atomic<bool> graphControlFailed {false};
    std::atomic<std::uint64_t> telemetryReceived {0U};
    std::atomic<int> reclaimedPlans {0};
    std::jthread graphControlThread {
        [&](const std::stop_token stopToken) {
            while (!stopToken.stop_requested()
                   && !requestGraphControl.load(
                       std::memory_order_acquire
                   )) {
                std::this_thread::yield();
            }
            if (stopToken.stop_requested()) {
                return;
            }

            try {
                if (!workload.publishGainVariant(0.75F)) {
                    graphControlFailed.store(
                        true,
                        std::memory_order_release
                    );
                    return;
                }

                const auto targetGeneration = workload.generation();
                while (!stopToken.stop_requested()
                       && workload.acknowledgedGeneration()
                           < targetGeneration) {
                    std::this_thread::yield();
                }
                if (stopToken.stop_requested()) {
                    return;
                }

                hotSwapCompleted.store(
                    true,
                    std::memory_order_release
                );

                resetCommandQueued.store(
                    workload.enqueueMixerReset(1U),
                    std::memory_order_release
                );
                if (!resetCommandQueued.load(std::memory_order_acquire)) {
                    graphControlFailed.store(
                        true,
                        std::memory_order_release
                    );
                    return;
                }

                audio::RealtimeCommandCompletion completion;
                while (!stopToken.stop_requested()) {
                    telemetryReceived.fetch_add(
                        workload.drainTelemetry(),
                        std::memory_order_relaxed
                    );
                    if (workload.tryPopCommandCompletion(completion)) {
                        const bool applied =
                            completion.sequence == 1U
                            && completion.targetNode
                                == workload.mixerNodeId()
                            && completion.status
                                == audio::CommandCompletionStatus::applied
                            && completion.appliedGeneration
                                == targetGeneration;
                        resetCommandApplied.store(
                            applied,
                            std::memory_order_release
                        );
                        if (!applied) {
                            graphControlFailed.store(
                                true,
                                std::memory_order_release
                            );
                        }
                        break;
                    }
                    std::this_thread::yield();
                }
                if (stopToken.stop_requested()
                    || !resetCommandApplied.load(
                        std::memory_order_acquire
                    )) {
                    return;
                }

                const auto eventPosition =
                    workload.lastRenderedSamplePosition()
                    + static_cast<std::uint64_t>(requestedFrames) * 2U;
                automationQueued.store(
                    workload.enqueueGainAutomation(
                        static_cast<std::int64_t>(eventPosition),
                        static_cast<int>(requestedFrames)
                    ),
                    std::memory_order_release
                );
                reclaimedPlans.store(
                    workload.reclaimRetiredPlans(),
                    std::memory_order_release
                );
                while (!stopToken.stop_requested()) {
                    const auto drained = workload.drainTelemetry();
                    telemetryReceived.fetch_add(
                        drained,
                        std::memory_order_relaxed
                    );
                    if (drained == 0U) {
                        std::this_thread::yield();
                    }
                }
            } catch (...) {
                graphControlFailed.store(
                    true,
                    std::memory_order_release
                );
            }
        }
    };
    const auto maximumCallbacks =
        static_cast<std::size_t>(seconds) * kSampleRate
            / requestedFrames
        + 4096U;
    std::vector<std::uint64_t> durations(maximumCallbacks, 0U);

    DWORD taskIndex = 0U;
    MmcssHandle mmcss {
        AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex)
    };
    if (mmcss.value != nullptr) {
        result.mmcssEnabled =
            AvSetMmThreadPriority(mmcss.value, AVRT_PRIORITY_CRITICAL)
            != FALSE;
    }

    realtime::resetAuditCounters();
    requireSuccess(client->Start(), "IAudioClient::Start");

    std::uint32_t warmups = 0U;
    std::size_t measured = 0U;
    auto measurementStarted = Clock::time_point {};
    auto previousWakeup = Clock::time_point {};
    const auto periodNanoseconds = std::chrono::nanoseconds {
        static_cast<std::int64_t>(
            static_cast<std::uint64_t>(requestedFrames)
            * 1'000'000'000U
            / kSampleRate
        )
    };
    const auto target = targetNanoseconds(requestedFrames);
    std::uint64_t samplePosition = 0U;

    while (
        measurementStarted == Clock::time_point {}
        || Clock::now() - measurementStarted
            < std::chrono::seconds {seconds}
    ) {
        const auto wait = WaitForSingleObject(event.value, 2'000U);
        if (wait == WAIT_TIMEOUT) {
            ++result.waitTimeouts;
            continue;
        }
        if (wait != WAIT_OBJECT_0) {
            requireSuccess(
                HRESULT_FROM_WIN32(GetLastError()),
                "WaitForSingleObject"
            );
        }

        std::uint32_t padding = 0U;
        if (!exclusive) {
            requireSuccess(
                client->GetCurrentPadding(&padding),
                "IAudioClient::GetCurrentPadding"
            );
        }
        const auto available = exclusive
            ? requestedFrames
            : result.streamBufferFrames - padding;
        if (available < requestedFrames) {
            continue;
        }

        BYTE* buffer = nullptr;
        requireSuccess(
            renderer->GetBuffer(requestedFrames, &buffer),
            "IAudioRenderClient::GetBuffer"
        );
        const auto duration = processAudio(
            workload,
            buffer,
            requestedFrames,
            floatingPointOutput,
            samplePosition
        );
        requireSuccess(
            renderer->ReleaseBuffer(
                requestedFrames,
                0U
            ),
            "IAudioRenderClient::ReleaseBuffer"
        );
        samplePosition += requestedFrames;

        if (warmups < kWarmupCallbacks) {
            ++warmups;
            if (warmups == kWarmupCallbacks) {
                measurementStarted = Clock::now();
                previousWakeup = measurementStarted;
                requestGraphControl.store(
                    true,
                    std::memory_order_release
                );
            }
            continue;
        }

        const auto wakeup = Clock::now();
        if (
            previousWakeup != Clock::time_point {}
            && wakeup - previousWakeup
                > periodNanoseconds + periodNanoseconds / 2
        ) {
            ++result.lateWakeups;
        }
        previousWakeup = wakeup;

        if (measured >= durations.size()) {
            requireSuccess(client->Stop(), "IAudioClient::Stop");
            throw std::runtime_error("Duration storage exhausted");
        }
        durations[measured] = duration;
        ++measured;
        if (duration > target) {
            ++result.targetMisses;
        }
        if (
            duration
            > static_cast<std::uint64_t>(periodNanoseconds.count())
        ) {
            ++result.hardDeadlineMisses;
        }
    }

    requireSuccess(client->Stop(), "IAudioClient::Stop");
    graphControlThread.request_stop();
    graphControlThread.join();
    result.audit = realtime::auditSnapshot();
    result.graphBlocks = workload.renderedBlocks();
    result.graphGeneration = workload.generation();
    result.graphObservedSwaps = workload.observedSwaps();
    result.pendingParameterEvents =
        workload.pendingParameterEvents();
    result.rejectedParameterEvents =
        workload.rejectedParameterEvents();
    result.lateParameterEvents = workload.lateParameterEvents();
    result.parameterBufferOverflows =
        workload.parameterBufferOverflows();
    result.rejectedRealtimeCommands =
        workload.rejectedRealtimeCommands();
    result.lostCommandCompletions =
        workload.lostCommandCompletions();
    result.pendingRealtimeCommands =
        workload.pendingRealtimeCommands();
    result.telemetryReceived = telemetryReceived.load(
        std::memory_order_acquire
    ) + workload.drainTelemetry();
    result.telemetryDropped = workload.droppedTelemetry();
    result.reclaimedPlans = reclaimedPlans.load(
        std::memory_order_acquire
    );
    result.hotSwapCompleted = hotSwapCompleted.load(
        std::memory_order_acquire
    );
    result.automationQueued = automationQueued.load(
        std::memory_order_acquire
    );
    result.resetCommandQueued = resetCommandQueued.load(
        std::memory_order_acquire
    );
    result.resetCommandApplied = resetCommandApplied.load(
        std::memory_order_acquire
    );
    result.graphControlFailed = graphControlFailed.load(
        std::memory_order_acquire
    );
    result.callbacks = measured;
    result.supported = true;
    result.status = "measured";

    durations.resize(measured);
    std::sort(durations.begin(), durations.end());
    result.p50Nanoseconds = percentile(durations, 0.50);
    result.p95Nanoseconds = percentile(durations, 0.95);
    result.p99Nanoseconds = percentile(durations, 0.99);
    result.maximumNanoseconds = durations.back();
    return result;
}

void printResult(const ProbeResult& result) {
    std::cout
        << std::fixed << std::setprecision(6)
        << "buffer=" << result.requestedFrames
        << " status=" << result.status
        << " backend=" << result.backend
        << " stream_buffer=" << result.streamBufferFrames
        << " period_min=" << result.minimumPeriodFrames
        << " period_max=" << result.maximumPeriodFrames
        << " period_fundamental=" << result.fundamentalPeriodFrames;

    if (result.supported) {
        const auto milliseconds = [](const std::uint64_t nanoseconds) {
            return static_cast<double>(nanoseconds) / 1'000'000.0;
        };
        std::cout
            << " callbacks=" << result.callbacks
            << " p50_ms=" << milliseconds(result.p50Nanoseconds)
            << " p95_ms=" << milliseconds(result.p95Nanoseconds)
            << " p99_ms=" << milliseconds(result.p99Nanoseconds)
            << " max_ms=" << milliseconds(result.maximumNanoseconds)
            << " target_misses=" << result.targetMisses
            << " hard_deadline_misses=" << result.hardDeadlineMisses
            << " late_wakeups=" << result.lateWakeups
            << " wait_timeouts=" << result.waitTimeouts
            << " callback_allocations=" << result.audit.allocations
            << " callback_deallocations=" << result.audit.deallocations
            << " callback_blocking_locks=" << result.audit.blockingLocks
            << " callback_denormal_mode_entries="
            << result.audit.denormalModeEntries
            << " callback_subnormal_samples_flushed="
            << result.audit.subnormalSamplesFlushed
            << " graph_blocks=" << result.graphBlocks
            << " graph_generation=" << result.graphGeneration
            << " graph_observed_swaps=" << result.graphObservedSwaps
            << " reclaimed_plans=" << result.reclaimedPlans
            << " hot_swap="
            << (result.hotSwapCompleted ? "completed" : "incomplete")
            << " automation="
            << (result.automationQueued ? "queued" : "not_queued")
            << " parameter_pending=" << result.pendingParameterEvents
            << " parameter_rejected="
            << result.rejectedParameterEvents
            << " parameter_late=" << result.lateParameterEvents
            << " parameter_overflow="
            << result.parameterBufferOverflows
            << " reset_command="
            << (result.resetCommandQueued ? "queued" : "not_queued")
            << " reset_completion="
            << (result.resetCommandApplied ? "applied" : "not_applied")
            << " command_pending=" << result.pendingRealtimeCommands
            << " command_rejected=" << result.rejectedRealtimeCommands
            << " completion_lost=" << result.lostCommandCompletions
            << " telemetry_received=" << result.telemetryReceived
            << " telemetry_dropped=" << result.telemetryDropped
            << " graph_control_failed="
            << (result.graphControlFailed ? "true" : "false")
            << " mmcss=" << (result.mmcssEnabled ? "enabled" : "disabled");
    }
    std::cout << '\n' << std::flush;
}

} // namespace

int run(const std::uint32_t secondsPerBuffer) {
    ComScope com;
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE) {
        fail("CoInitializeEx", com.result);
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    requireSuccess(
        CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator)
        ),
        "CoCreateInstance(MMDeviceEnumerator)"
    );
    ComPtr<IMMDevice> device;
    requireSuccess(
        enumerator->GetDefaultAudioEndpoint(
            eRender,
            eConsole,
            &device
        ),
        "IMMDeviceEnumerator::GetDefaultAudioEndpoint"
    );

    std::cout
        << "audio_probe backend=WASAPI_shared_with_exclusive_fallback "
        << "sample_rate="
        << kSampleRate
        << " seconds_per_buffer=" << secondsPerBuffer
        << " callback_workload=immutable_graph_production_nodes\n";

    bool allMeasured = true;
    bool auditClean = true;
    bool graphControlClean = true;
    for (const auto frames : std::array {64U, 128U, 256U}) {
        try {
            const auto result =
                runConfiguration(device.Get(), frames, secondsPerBuffer);
            printResult(result);
            allMeasured = allMeasured && result.supported;
            auditClean = auditClean
                && result.audit.allocations == 0U
                && result.audit.deallocations == 0U
                && result.audit.blockingLocks == 0U;
            graphControlClean = graphControlClean
                && (!result.supported
                    || (result.hotSwapCompleted
                        && result.automationQueued
                        && result.resetCommandQueued
                        && result.resetCommandApplied
                        && result.pendingRealtimeCommands == 0U
                        && result.rejectedRealtimeCommands == 0U
                        && result.lostCommandCompletions == 0U
                        && result.telemetryReceived > 0U
                        && result.pendingParameterEvents == 0U
                        && result.rejectedParameterEvents == 0U
                        && result.parameterBufferOverflows == 0U
                        && !result.graphControlFailed));
        } catch (const std::exception& exception) {
            allMeasured = false;
            std::cout
                << "buffer=" << frames
                << " status=error message=\"" << exception.what()
                << "\"\n" << std::flush;
        }
    }

    if (!auditClean) {
        std::cerr << "Real-time callback audit failed.\n";
        return 5;
    }
    if (!graphControlClean) {
        std::cerr << "Graph control integration failed.\n";
        return 6;
    }
    return allMeasured ? 0 : 4;
}

} // namespace iramix::audio_probe
