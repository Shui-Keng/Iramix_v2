#include "GraphWorkload.hpp"
#include "iramix/realtime/Audit.hpp"

#include <asiosys.h>
#include <asio.h>
#include <asiodrivers.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

extern AsioDrivers* asioDrivers;
bool loadAsioDriver(char* name);

namespace iramix::asio_probe {
namespace {

using Clock = std::chrono::steady_clock;

constexpr long kSampleRate = 48'000L;
constexpr std::size_t kOutputChannels = 2U;
constexpr std::uint64_t kWarmupCallbacks = 100U;
constexpr std::size_t kLateWakeupBucketCount = 10U;

struct Context {
    long frames {0L};
    std::uint32_t seconds {0U};
    std::array<ASIOBufferInfo, kOutputChannels> buffers {};
    std::array<ASIOSampleType, kOutputChannels> sampleTypes {};
    // Constructed before ASIOStart: the graph allocates its plan and
    // planar storage here, never inside the callback.
    std::unique_ptr<audio_probe::StereoGraphWorkload> workload;
    std::uint64_t samplePosition {0U};
    std::vector<std::uint64_t> durations;
    std::atomic<std::uint64_t> callbackCount {0U};
    std::atomic<std::uint64_t> measuredCount {0U};
    std::atomic<std::uint64_t> lateWakeups {0U};
    std::array<
        std::atomic<std::uint64_t>,
        kLateWakeupBucketCount
    > lateWakeupBuckets {};
    std::atomic<std::uint64_t> resetRequests {0U};
    std::atomic<std::uint64_t> resyncRequests {0U};
    std::atomic<std::uint64_t> sampleRateChanges {0U};
    std::atomic<bool> measurementComplete {false};
    Clock::time_point measurementStarted {};
    Clock::time_point previousWakeup {};
    bool outputReadySupported {false};
};

Context* activeContext = nullptr;

[[nodiscard]] std::uint64_t targetNanoseconds(const long frames) {
    switch (frames) {
    case 64L:
        return 930'000U;
    case 128L:
        return 1'870'000U;
    case 256L:
        return 3'730'000U;
    default:
        return 0U;
    }
}

// Only the little-endian native formats this probe can actually write
// are accepted. The right-aligned Int32LSBnn and every MSB variant are
// refused up front rather than silently filled with wrong samples,
// because a driver that reports one is a driver whose measurements
// would not mean anything.
[[nodiscard]] bool convertibleSampleType(const ASIOSampleType type) {
    switch (type) {
    case ASIOSTInt16LSB:
    case ASIOSTInt24LSB:
    case ASIOSTInt32LSB:
    case ASIOSTFloat32LSB:
    case ASIOSTFloat64LSB:
        return true;
    default:
        return false;
    }
}

void writeChannel(
    void* const destination,
    const float* const source,
    const long frames,
    const ASIOSampleType type
) noexcept {
    auto* const bytes = static_cast<unsigned char*>(destination);
    for (long frame = 0L; frame < frames; ++frame) {
        const auto sample = std::clamp(
            source[static_cast<std::size_t>(frame)],
            -1.0F,
            1.0F
        );
        switch (type) {
        case ASIOSTInt16LSB: {
            const auto value = static_cast<std::int16_t>(
                sample * 32'767.0F
            );
            std::memcpy(
                bytes + frame * 2L,
                &value,
                sizeof(value)
            );
            break;
        }
        case ASIOSTInt24LSB: {
            const auto value = static_cast<std::int32_t>(
                sample * 8'388'607.0F
            );
            bytes[frame * 3L] =
                static_cast<unsigned char>(value & 0xFF);
            bytes[frame * 3L + 1L] =
                static_cast<unsigned char>((value >> 8) & 0xFF);
            bytes[frame * 3L + 2L] =
                static_cast<unsigned char>((value >> 16) & 0xFF);
            break;
        }
        case ASIOSTInt32LSB: {
            const auto value = static_cast<std::int32_t>(
                static_cast<double>(sample) * 2'147'483'647.0
            );
            std::memcpy(
                bytes + frame * 4L,
                &value,
                sizeof(value)
            );
            break;
        }
        case ASIOSTFloat32LSB:
            std::memcpy(
                bytes + frame * 4L,
                &sample,
                sizeof(sample)
            );
            break;
        case ASIOSTFloat64LSB: {
            const auto value = static_cast<double>(sample);
            std::memcpy(
                bytes + frame * 8L,
                &value,
                sizeof(value)
            );
            break;
        }
        default:
            return;
        }
    }
}

void processBuffer(const long bufferIndex) noexcept {
    auto* context = activeContext;
    if (context == nullptr || context->measurementComplete.load(
            std::memory_order_relaxed
        )) {
        return;
    }

    const auto callbackStarted = Clock::now();
    std::uint64_t elapsedNanoseconds = 0U;
    {
        realtime::CallbackScope callback;
        const auto frames = static_cast<std::uint32_t>(context->frames);
        context->workload->renderBlock(frames, context->samplePosition);
        context->samplePosition += frames;
        const auto planar = context->workload->planarView(frames);
        for (
            std::size_t channel = 0U;
            channel < kOutputChannels;
            ++channel
        ) {
            writeChannel(
                context->buffers[channel].buffers[bufferIndex],
                planar.channel(static_cast<int>(channel)),
                context->frames,
                context->sampleTypes[channel]
            );
        }
        elapsedNanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - callbackStarted
            ).count()
        );
    }

    const auto callbackNumber = context->callbackCount.fetch_add(
        1U,
        std::memory_order_relaxed
    );
    if (callbackNumber < kWarmupCallbacks) {
        if (callbackNumber + 1U == kWarmupCallbacks) {
            context->measurementStarted = Clock::now();
            context->previousWakeup = context->measurementStarted;
        }
    } else {
        const auto now = Clock::now();
        const auto period = std::chrono::nanoseconds {
            static_cast<std::int64_t>(
                static_cast<std::uint64_t>(context->frames)
                * 1'000'000'000U
                / static_cast<std::uint64_t>(kSampleRate)
            )
        };
        if (now - context->previousWakeup > period + period / 2) {
            context->lateWakeups.fetch_add(1U, std::memory_order_relaxed);
            const auto elapsed = std::chrono::duration_cast<
                std::chrono::nanoseconds
            >(now - context->measurementStarted).count();
            const auto duration = static_cast<std::uint64_t>(
                context->seconds
            ) * 1'000'000'000U;
            const auto bucket = duration == 0U
                ? 0U
                : std::min<std::uint64_t>(
                    kLateWakeupBucketCount - 1U,
                    static_cast<std::uint64_t>(elapsed)
                        * kLateWakeupBucketCount
                        / duration
                );
            context->lateWakeupBuckets[
                static_cast<std::size_t>(bucket)
            ].fetch_add(1U, std::memory_order_relaxed);
        }
        context->previousWakeup = now;

        const auto measurementIndex = context->measuredCount.fetch_add(
            1U,
            std::memory_order_relaxed
        );
        if (measurementIndex < context->durations.size()) {
            context->durations[
                static_cast<std::size_t>(measurementIndex)
            ] = elapsedNanoseconds;
        } else {
            context->measurementComplete.store(
                true,
                std::memory_order_release
            );
        }

        if (
            now - context->measurementStarted
            >= std::chrono::seconds {context->seconds}
        ) {
            context->measurementComplete.store(
                true,
                std::memory_order_release
            );
        }
    }

    if (context->outputReadySupported) {
        ASIOOutputReady();
    }
}

ASIOTime* bufferSwitchTimeInfo(
    ASIOTime* parameters,
    const long index,
    ASIOBool
) {
    processBuffer(index);
    return parameters;
}

void bufferSwitch(const long index, ASIOBool) {
    processBuffer(index);
}

void sampleRateChanged(ASIOSampleRate) {
    if (activeContext != nullptr) {
        activeContext->sampleRateChanges.fetch_add(
            1U,
            std::memory_order_relaxed
        );
    }
}

long asioMessage(
    const long selector,
    const long value,
    void*,
    double*
) {
    switch (selector) {
    case kAsioSelectorSupported:
        return value == kAsioResetRequest
                || value == kAsioResyncRequest
                || value == kAsioLatenciesChanged
                || value == kAsioEngineVersion
                || value == kAsioSupportsTimeInfo
            ? 1L
            : 0L;
    case kAsioEngineVersion:
        return 2L;
    case kAsioSupportsTimeInfo:
        return 1L;
    case kAsioResyncRequest:
        if (activeContext != nullptr) {
            activeContext->resyncRequests.fetch_add(
                1U,
                std::memory_order_relaxed
            );
        }
        return 1L;
    case kAsioResetRequest:
        if (activeContext != nullptr) {
            activeContext->resetRequests.fetch_add(
                1U,
                std::memory_order_relaxed
            );
        }
        return 1L;
    case kAsioLatenciesChanged:
        return 1L;
    default:
        return 0L;
    }
}

ASIOCallbacks callbacks {
    .bufferSwitch = &bufferSwitch,
    .sampleRateDidChange = &sampleRateChanged,
    .asioMessage = &asioMessage,
    .bufferSwitchTimeInfo = &bufferSwitchTimeInfo,
};

[[nodiscard]] bool bufferSizeSupported(
    const long requested,
    const long minimum,
    const long maximum,
    const long granularity
) {
    if (requested < minimum || requested > maximum) {
        return false;
    }
    if (granularity == 0L) {
        return requested == minimum;
    }
    if (granularity == -1L) {
        return (requested & (requested - 1L)) == 0L;
    }
    return granularity > 0L
        && ((requested - minimum) % granularity) == 0L;
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

void printUnsupported(
    const long frames,
    const long minimum,
    const long maximum,
    const long preferred,
    const long granularity
) {
    std::cout
        << "buffer=" << frames
        << " status=unsupported_driver_buffer"
        << " backend=ASIO"
        << " driver_min=" << minimum
        << " driver_max=" << maximum
        << " driver_preferred=" << preferred
        << " driver_granularity=" << granularity
        << '\n' << std::flush;
}

bool runConfiguration(
    const long frames,
    const std::uint32_t seconds,
    const long outputChannels
) {
    Context context;
    context.frames = frames;
    context.seconds = seconds;
    context.workload =
        std::make_unique<audio_probe::StereoGraphWorkload>(
            static_cast<std::uint32_t>(frames)
        );
    const auto maximumCallbacks =
        static_cast<std::size_t>(seconds)
            * static_cast<std::size_t>(kSampleRate)
            / static_cast<std::size_t>(frames)
        + 4096U;
    context.durations.assign(maximumCallbacks, 0U);

    const auto channelsToOpen = std::min<long>(
        static_cast<long>(kOutputChannels),
        outputChannels
    );
    if (channelsToOpen != static_cast<long>(kOutputChannels)) {
        throw std::runtime_error("ASIO driver has fewer than two outputs");
    }

    for (
        std::size_t channel = 0U;
        channel < kOutputChannels;
        ++channel
    ) {
        context.buffers[channel].isInput = ASIOFalse;
        context.buffers[channel].channelNum =
            static_cast<long>(channel);
    }

    const auto createResult = ASIOCreateBuffers(
        context.buffers.data(),
        static_cast<long>(context.buffers.size()),
        frames,
        &callbacks
    );
    if (createResult != ASE_OK) {
        std::cout
            << "buffer=" << frames
            << " status=create_buffers_failed"
            << " backend=ASIO"
            << " asio_error=" << createResult
            << '\n' << std::flush;
        return false;
    }

    bool success = false;
    try {
        for (
            std::size_t channel = 0U;
            channel < kOutputChannels;
            ++channel
        ) {
            ASIOChannelInfo channelInfo {};
            channelInfo.channel = static_cast<long>(channel);
            channelInfo.isInput = ASIOFalse;
            if (ASIOGetChannelInfo(&channelInfo) != ASE_OK) {
                throw std::runtime_error("ASIOGetChannelInfo failed");
            }
            if (!convertibleSampleType(channelInfo.type)) {
                throw std::runtime_error(
                    "ASIO sample type cannot be written by this probe"
                );
            }
            context.sampleTypes[channel] = channelInfo.type;
        }

        context.outputReadySupported = ASIOOutputReady() == ASE_OK;
        activeContext = &context;
        realtime::resetAuditCounters();
        if (ASIOStart() != ASE_OK) {
            throw std::runtime_error("ASIOStart failed");
        }

        const auto safetyDeadline = Clock::now()
            + std::chrono::seconds {seconds + 10U};
        while (
            !context.measurementComplete.load(std::memory_order_acquire)
            && Clock::now() < safetyDeadline
        ) {
            // The telemetry queue is bounded, so a consumer that never
            // runs turns every block into a counted drop and hides
            // whether the engine actually lost anything. Draining here
            // keeps that counter meaningful; it is the control thread.
            static_cast<void>(context.workload->drainTelemetry());
            Sleep(10U);
        }
        static_cast<void>(context.workload->drainTelemetry());
        const auto stoppedInTime = context.measurementComplete.load(
            std::memory_order_acquire
        );
        ASIOStop();
        activeContext = nullptr;
        if (!stoppedInTime) {
            throw std::runtime_error("ASIO callback timed out");
        }

        auto measured = context.measuredCount.load(
            std::memory_order_relaxed
        );
        measured = std::min<std::uint64_t>(
            measured,
            context.durations.size()
        );
        context.durations.resize(static_cast<std::size_t>(measured));
        std::sort(context.durations.begin(), context.durations.end());
        const auto audit = realtime::auditSnapshot();
        const auto milliseconds = [](const std::uint64_t nanoseconds) {
            return static_cast<double>(nanoseconds) / 1'000'000.0;
        };
        const auto p50 = percentile(context.durations, 0.50);
        const auto p95 = percentile(context.durations, 0.95);
        const auto p99 = percentile(context.durations, 0.99);
        const auto maximum = context.durations.back();
        const auto target = targetNanoseconds(frames);
        const auto targetMisses = static_cast<std::uint64_t>(
            std::count_if(
                context.durations.begin(),
                context.durations.end(),
                [target](const auto duration) {
                    return duration > target;
                }
            )
        );
        const auto hardDeadline = static_cast<std::uint64_t>(
            static_cast<long double>(frames)
            * 1'000'000'000.0L
            / static_cast<long double>(kSampleRate)
        );
        const auto hardMisses = static_cast<std::uint64_t>(
            std::count_if(
                context.durations.begin(),
                context.durations.end(),
                [hardDeadline](const auto duration) {
                    return duration > hardDeadline;
                }
            )
        );
        const auto expectedCallbacks =
            static_cast<std::uint64_t>(seconds)
            * static_cast<std::uint64_t>(kSampleRate)
            / static_cast<std::uint64_t>(frames);
        const auto callbackCoverage = expectedCallbacks == 0U
            ? 0.0
            : static_cast<double>(measured)
                / static_cast<double>(expectedCallbacks);

        std::cout
            << std::fixed << std::setprecision(6)
            << "buffer=" << frames
            << " status=measured backend=ASIO"
            << " callbacks=" << measured
            << " expected_callbacks=" << expectedCallbacks
            << " callback_coverage=" << callbackCoverage
            << " p50_ms=" << milliseconds(p50)
            << " p95_ms=" << milliseconds(p95)
            << " p99_ms=" << milliseconds(p99)
            << " max_ms=" << milliseconds(maximum)
            << " target_misses=" << targetMisses
            << " hard_deadline_misses=" << hardMisses
            << " late_wakeups="
            << context.lateWakeups.load(std::memory_order_relaxed)
            << " late_wakeup_buckets_10=";
        for (
            std::size_t bucket = 0U;
            bucket < context.lateWakeupBuckets.size();
            ++bucket
        ) {
            if (bucket != 0U) {
                std::cout << ',';
            }
            std::cout << context.lateWakeupBuckets[bucket].load(
                std::memory_order_relaxed
            );
        }
        std::cout
            << " reset_requests="
            << context.resetRequests.load(std::memory_order_relaxed)
            << " resync_requests="
            << context.resyncRequests.load(std::memory_order_relaxed)
            << " sample_rate_changes="
            << context.sampleRateChanges.load(std::memory_order_relaxed)
            << " callback_allocations=" << audit.allocations
            << " callback_deallocations=" << audit.deallocations
            << " callback_blocking_locks=" << audit.blockingLocks
            << " callback_denormal_mode_entries="
            << audit.denormalModeEntries
            << " callback_subnormal_samples_flushed="
            << audit.subnormalSamplesFlushed
            << " graph_blocks=" << context.workload->renderedBlocks()
            << " graph_generation=" << context.workload->generation()
            << " telemetry_dropped="
            << context.workload->droppedTelemetry()
            << '\n' << std::flush;
        success = audit.allocations == 0U
            && audit.deallocations == 0U
            && audit.blockingLocks == 0U;
    } catch (...) {
        activeContext = nullptr;
        ASIODisposeBuffers();
        throw;
    }

    ASIODisposeBuffers();
    return success;
}

} // namespace

int listDrivers() {
    AsioDrivers drivers;
    std::array<std::array<char, 64U>, 32U> storage {};
    std::array<char*, 32U> names {};
    for (std::size_t index = 0U; index < names.size(); ++index) {
        names[index] = storage[index].data();
    }
    const auto count = drivers.getDriverNames(
        names.data(),
        static_cast<long>(names.size())
    );
    for (long index = 0L; index < count; ++index) {
        std::cout << storage[static_cast<std::size_t>(index)].data()
                  << '\n';
    }
    return 0;
}

int run(const std::string& driverName, const std::uint32_t seconds) {
    auto mutableDriverName = std::vector<char>(
        driverName.begin(),
        driverName.end()
    );
    mutableDriverName.push_back('\0');
    if (!loadAsioDriver(mutableDriverName.data())) {
        std::cerr << "Failed to load ASIO driver: " << driverName << '\n';
        return 4;
    }

    ASIODriverInfo driverInfo {};
    driverInfo.asioVersion = 2L;
    if (ASIOInit(&driverInfo) != ASE_OK) {
        asioDrivers->removeCurrentDriver();
        std::cerr << "ASIOInit failed: " << driverInfo.errorMessage << '\n';
        return 4;
    }

    int exitCode = 0;
    try {
        long inputChannels = 0L;
        long outputChannels = 0L;
        if (ASIOGetChannels(&inputChannels, &outputChannels) != ASE_OK) {
            throw std::runtime_error("ASIOGetChannels failed");
        }
        long minimum = 0L;
        long maximum = 0L;
        long preferred = 0L;
        long granularity = 0L;
        if (
            ASIOGetBufferSize(
                &minimum,
                &maximum,
                &preferred,
                &granularity
            ) != ASE_OK
        ) {
            throw std::runtime_error("ASIOGetBufferSize failed");
        }
        if (ASIOCanSampleRate(48'000.0) != ASE_OK) {
            throw std::runtime_error("ASIO driver rejects 48 kHz");
        }
        if (ASIOSetSampleRate(48'000.0) != ASE_OK) {
            throw std::runtime_error("ASIOSetSampleRate failed");
        }
        ASIOSampleRate actualSampleRate = 0.0;
        if (ASIOGetSampleRate(&actualSampleRate) != ASE_OK) {
            throw std::runtime_error("ASIOGetSampleRate failed");
        }
        if (std::abs(actualSampleRate - 48'000.0) > 0.5) {
            throw std::runtime_error(
                "ASIO driver did not retain the requested 48 kHz rate"
            );
        }

        std::cout
            << "audio_probe backend=ASIO"
            << " driver=\"" << driverInfo.name << "\""
            << " sample_rate=" << actualSampleRate
            << " seconds_per_buffer=" << seconds
            << " driver_min=" << minimum
            << " driver_max=" << maximum
            << " driver_preferred=" << preferred
            << " driver_granularity=" << granularity
            << " callback_workload=immutable_graph_production_nodes"
            << '\n' << std::flush;

        for (const auto frames : std::array {64L, 128L, 256L}) {
            if (!bufferSizeSupported(
                    frames,
                    minimum,
                    maximum,
                    granularity
                )) {
                printUnsupported(
                    frames,
                    minimum,
                    maximum,
                    preferred,
                    granularity
                );
                exitCode = 4;
                continue;
            }
            if (!runConfiguration(frames, seconds, outputChannels)) {
                exitCode = 5;
            }
        }
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        exitCode = 4;
    }

    ASIOExit();
    asioDrivers->removeCurrentDriver();
    return exitCode;
}

} // namespace iramix::asio_probe
