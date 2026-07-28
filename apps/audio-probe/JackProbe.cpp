#include "AudioProbe.hpp"
#include "iramix/realtime/Audit.hpp"

#include <jack/jack.h>

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
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace iramix::audio_probe {
namespace {

using Clock = std::chrono::steady_clock;

constexpr jack_nframes_t kSampleRate = 48'000U;
constexpr std::size_t kChannels = 2U;
constexpr std::uint64_t kWarmupCallbacks = 100U;
constexpr std::size_t kLateWakeupBucketCount = 10U;

struct Context {
    jack_nframes_t frames {0U};
    std::uint32_t seconds {0U};
    std::array<jack_port_t*, kChannels> ports {};
    std::vector<float> input;
    std::vector<float> output;
    std::vector<std::uint64_t> durations;
    std::atomic<std::uint64_t> callbackCount {0U};
    std::atomic<std::uint64_t> measuredCount {0U};
    std::atomic<std::uint64_t> lateWakeups {0U};
    std::array<
        std::atomic<std::uint64_t>,
        kLateWakeupBucketCount
    > lateWakeupBuckets {};
    std::atomic<std::uint64_t> frameMismatches {0U};
    std::atomic<std::uint64_t> xruns {0U};
    std::atomic<std::uint64_t> sampleRateChanges {0U};
    std::atomic<bool> serverShutdown {false};
    std::atomic<bool> measurementComplete {false};
    Clock::time_point measurementStarted {};
    Clock::time_point previousWakeup {};
};

struct Result {
    jack_nframes_t frames {0U};
    std::uint64_t callbacks {0U};
    std::uint64_t expectedCallbacks {0U};
    std::uint64_t lateWakeups {0U};
    std::array<std::uint64_t, kLateWakeupBucketCount> lateBuckets {};
    std::uint64_t frameMismatches {0U};
    std::uint64_t xruns {0U};
    std::uint64_t sampleRateChanges {0U};
    std::uint32_t physicalConnections {0U};
    std::uint64_t p50 {0U};
    std::uint64_t p95 {0U};
    std::uint64_t p99 {0U};
    std::uint64_t maximum {0U};
    std::uint64_t targetMisses {0U};
    std::uint64_t hardDeadlineMisses {0U};
    realtime::AuditSnapshot audit;
};

struct JackClient {
    jack_client_t* value {nullptr};

    ~JackClient() {
        if (value != nullptr) {
            jack_client_close(value);
        }
    }
};

[[nodiscard]] std::uint64_t targetNanoseconds(
    const jack_nframes_t frames
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

void recordLateWakeup(
    Context& context,
    const Clock::time_point now
) noexcept {
    context.lateWakeups.fetch_add(1U, std::memory_order_relaxed);
    const auto elapsed = std::chrono::duration_cast<
        std::chrono::nanoseconds
    >(now - context.measurementStarted).count();
    const auto duration = static_cast<std::uint64_t>(
        context.seconds
    ) * 1'000'000'000U;
    const auto bucket = duration == 0U
        ? 0U
        : std::min<std::uint64_t>(
            kLateWakeupBucketCount - 1U,
            static_cast<std::uint64_t>(elapsed)
                * kLateWakeupBucketCount
                / duration
        );
    context.lateWakeupBuckets[
        static_cast<std::size_t>(bucket)
    ].fetch_add(1U, std::memory_order_relaxed);
}

int processCallback(
    const jack_nframes_t frameCount,
    void* argument
) noexcept {
    auto& context = *static_cast<Context*>(argument);
    auto* left = static_cast<jack_default_audio_sample_t*>(
        jack_port_get_buffer(context.ports[0], frameCount)
    );
    auto* right = static_cast<jack_default_audio_sample_t*>(
        jack_port_get_buffer(context.ports[1], frameCount)
    );
    if (context.measurementComplete.load(std::memory_order_relaxed)) {
        std::memset(left, 0, frameCount * sizeof(*left));
        std::memset(right, 0, frameCount * sizeof(*right));
        return 0;
    }

    const auto callbackStarted = Clock::now();
    std::uint64_t elapsedNanoseconds = 0U;
    {
        realtime::CallbackScope callbackScope;
        const auto samples = std::min<std::size_t>(
            context.output.size(),
            static_cast<std::size_t>(frameCount) * kChannels
        );
        for (std::size_t index = 0U; index < samples; ++index) {
            context.output[index] = context.input[index] * 0.5F;
        }
        std::memset(left, 0, frameCount * sizeof(*left));
        std::memset(right, 0, frameCount * sizeof(*right));
        elapsedNanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - callbackStarted
            ).count()
        );
    }

    if (frameCount != context.frames) {
        context.frameMismatches.fetch_add(1U, std::memory_order_relaxed);
    }
    const auto callbackNumber = context.callbackCount.fetch_add(
        1U,
        std::memory_order_relaxed
    );
    if (callbackNumber < kWarmupCallbacks) {
        if (callbackNumber + 1U == kWarmupCallbacks) {
            context.measurementStarted = Clock::now();
            context.previousWakeup = context.measurementStarted;
        }
        return 0;
    }

    const auto now = Clock::now();
    const auto period = std::chrono::nanoseconds {
        static_cast<std::int64_t>(
            static_cast<std::uint64_t>(context.frames)
            * 1'000'000'000U
            / kSampleRate
        )
    };
    if (now - context.previousWakeup > period + period / 2) {
        recordLateWakeup(context, now);
    }
    context.previousWakeup = now;

    const auto measurementIndex = context.measuredCount.fetch_add(
        1U,
        std::memory_order_relaxed
    );
    if (measurementIndex < context.durations.size()) {
        context.durations[
            static_cast<std::size_t>(measurementIndex)
        ] = elapsedNanoseconds;
    } else {
        context.measurementComplete.store(true, std::memory_order_release);
    }
    if (
        now - context.measurementStarted
        >= std::chrono::seconds {context.seconds}
    ) {
        context.measurementComplete.store(true, std::memory_order_release);
    }
    return 0;
}

int xrunCallback(void* argument) noexcept {
    auto& context = *static_cast<Context*>(argument);
    context.xruns.fetch_add(1U, std::memory_order_relaxed);
    return 0;
}

int sampleRateCallback(
    const jack_nframes_t rate,
    void* argument
) noexcept {
    auto& context = *static_cast<Context*>(argument);
    if (rate != kSampleRate) {
        context.sampleRateChanges.fetch_add(1U, std::memory_order_relaxed);
    }
    return 0;
}

void shutdownCallback(void* argument) noexcept {
    auto& context = *static_cast<Context*>(argument);
    context.serverShutdown.store(true, std::memory_order_release);
    context.measurementComplete.store(true, std::memory_order_release);
}

[[nodiscard]] std::uint32_t connectPhysicalOutputs(
    jack_client_t* client,
    const Context& context
) {
    const auto** destinations = jack_get_ports(
        client,
        nullptr,
        JACK_DEFAULT_AUDIO_TYPE,
        JackPortIsPhysical | JackPortIsInput
    );
    if (destinations == nullptr) {
        return 0U;
    }

    std::uint32_t connected = 0U;
    for (
        std::size_t channel = 0U;
        channel < kChannels && destinations[channel] != nullptr;
        ++channel
    ) {
        if (
            jack_connect(
                client,
                jack_port_name(context.ports[channel]),
                destinations[channel]
            ) == 0
        ) {
            ++connected;
        }
    }
    jack_free(destinations);
    return connected;
}

Result runConfiguration(
    const jack_nframes_t frames,
    const std::uint32_t seconds
) {
    jack_status_t status {};
    JackClient client {
        jack_client_open(
            "iramix_audio_probe",
            JackNoStartServer,
            &status
        )
    };
    if (client.value == nullptr) {
        throw std::runtime_error(
            "JACK server unavailable; status="
            + std::to_string(static_cast<unsigned int>(status))
        );
    }
    if (jack_get_sample_rate(client.value) != kSampleRate) {
        throw std::runtime_error("JACK server is not running at 48 kHz");
    }
    const auto originalFrames = jack_get_buffer_size(client.value);
    if (jack_set_buffer_size(client.value, frames) != 0) {
        throw std::runtime_error("jack_set_buffer_size failed");
    }
    if (jack_get_buffer_size(client.value) != frames) {
        throw std::runtime_error(
            "JACK did not retain the requested buffer size"
        );
    }

    Context context;
    context.frames = frames;
    context.seconds = seconds;
    const auto samples = static_cast<std::size_t>(frames) * kChannels;
    context.input.assign(samples, 0.0F);
    context.output.assign(samples, 0.0F);
    const auto maximumCallbacks =
        static_cast<std::size_t>(seconds) * kSampleRate / frames
        + 4096U;
    context.durations.assign(maximumCallbacks, 0U);

    if (
        jack_set_process_callback(
            client.value,
            &processCallback,
            &context
        ) != 0
        || jack_set_xrun_callback(
            client.value,
            &xrunCallback,
            &context
        ) != 0
        || jack_set_sample_rate_callback(
            client.value,
            &sampleRateCallback,
            &context
        ) != 0
    ) {
        throw std::runtime_error("JACK callback registration failed");
    }
    jack_on_shutdown(client.value, &shutdownCallback, &context);

    context.ports[0] = jack_port_register(
        client.value,
        "output_left",
        JACK_DEFAULT_AUDIO_TYPE,
        JackPortIsOutput,
        0U
    );
    context.ports[1] = jack_port_register(
        client.value,
        "output_right",
        JACK_DEFAULT_AUDIO_TYPE,
        JackPortIsOutput,
        0U
    );
    if (context.ports[0] == nullptr || context.ports[1] == nullptr) {
        throw std::runtime_error("JACK output port registration failed");
    }

    realtime::resetAuditCounters();
    if (jack_activate(client.value) != 0) {
        throw std::runtime_error("jack_activate failed");
    }
    const auto physicalConnections = connectPhysicalOutputs(
        client.value,
        context
    );

    const auto safetyDeadline = Clock::now()
        + std::chrono::seconds {seconds + 10U};
    while (
        !context.measurementComplete.load(std::memory_order_acquire)
        && Clock::now() < safetyDeadline
    ) {
        std::this_thread::sleep_for(std::chrono::milliseconds {10});
    }
    const auto stoppedInTime = context.measurementComplete.load(
        std::memory_order_acquire
    );
    const auto serverShutdown = context.serverShutdown.load(
        std::memory_order_acquire
    );
    if (!serverShutdown && originalFrames != frames) {
        jack_set_buffer_size(client.value, originalFrames);
    }
    if (!serverShutdown) {
        jack_deactivate(client.value);
    }
    if (!stoppedInTime) {
        throw std::runtime_error("JACK callback timed out");
    }
    if (serverShutdown) {
        throw std::runtime_error("JACK server shut down during measurement");
    }

    Result result;
    result.frames = frames;
    result.callbacks = std::min<std::uint64_t>(
        context.measuredCount.load(std::memory_order_relaxed),
        context.durations.size()
    );
    result.expectedCallbacks =
        static_cast<std::uint64_t>(seconds) * kSampleRate / frames;
    result.lateWakeups = context.lateWakeups.load(
        std::memory_order_relaxed
    );
    for (
        std::size_t index = 0U;
        index < result.lateBuckets.size();
        ++index
    ) {
        result.lateBuckets[index] = context.lateWakeupBuckets[index].load(
            std::memory_order_relaxed
        );
    }
    result.frameMismatches = context.frameMismatches.load(
        std::memory_order_relaxed
    );
    result.xruns = context.xruns.load(std::memory_order_relaxed);
    result.sampleRateChanges = context.sampleRateChanges.load(
        std::memory_order_relaxed
    );
    result.physicalConnections = physicalConnections;
    result.audit = realtime::auditSnapshot();

    context.durations.resize(static_cast<std::size_t>(result.callbacks));
    if (context.durations.empty()) {
        throw std::runtime_error("JACK produced no callbacks");
    }
    std::sort(context.durations.begin(), context.durations.end());
    result.p50 = percentile(context.durations, 0.50);
    result.p95 = percentile(context.durations, 0.95);
    result.p99 = percentile(context.durations, 0.99);
    result.maximum = context.durations.back();
    const auto target = targetNanoseconds(frames);
    result.targetMisses = static_cast<std::uint64_t>(
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
        / kSampleRate
    );
    result.hardDeadlineMisses = static_cast<std::uint64_t>(
        std::count_if(
            context.durations.begin(),
            context.durations.end(),
            [hardDeadline](const auto duration) {
                return duration > hardDeadline;
            }
        )
    );
    return result;
}

void printResult(const Result& result) {
    const auto milliseconds = [](const std::uint64_t nanoseconds) {
        return static_cast<double>(nanoseconds) / 1'000'000.0;
    };
    const auto coverage = result.expectedCallbacks == 0U
        ? 0.0
        : static_cast<double>(result.callbacks)
            / static_cast<double>(result.expectedCallbacks);
    std::cout
        << std::fixed << std::setprecision(6)
        << "buffer=" << result.frames
        << " status=measured backend=JACK"
        << " callbacks=" << result.callbacks
        << " expected_callbacks=" << result.expectedCallbacks
        << " callback_coverage=" << coverage
        << " p50_ms=" << milliseconds(result.p50)
        << " p95_ms=" << milliseconds(result.p95)
        << " p99_ms=" << milliseconds(result.p99)
        << " max_ms=" << milliseconds(result.maximum)
        << " target_misses=" << result.targetMisses
        << " hard_deadline_misses=" << result.hardDeadlineMisses
        << " late_wakeups=" << result.lateWakeups
        << " late_wakeup_buckets_10=";
    for (
        std::size_t index = 0U;
        index < result.lateBuckets.size();
        ++index
    ) {
        if (index != 0U) {
            std::cout << ',';
        }
        std::cout << result.lateBuckets[index];
    }
    std::cout
        << " frame_mismatches=" << result.frameMismatches
        << " xruns=" << result.xruns
        << " sample_rate_changes=" << result.sampleRateChanges
        << " physical_connections=" << result.physicalConnections
        << " callback_allocations=" << result.audit.allocations
        << " callback_deallocations=" << result.audit.deallocations
        << " callback_blocking_locks=" << result.audit.blockingLocks
        << " callback_denormal_mode_entries="
        << result.audit.denormalModeEntries
        << " callback_subnormal_samples_flushed="
        << result.audit.subnormalSamplesFlushed
        << '\n' << std::flush;
}

} // namespace

int run(const std::uint32_t secondsPerBuffer) {
    std::cout
        << "audio_probe backend=JACK"
        << " sample_rate=" << kSampleRate
        << " seconds_per_buffer=" << secondsPerBuffer
        << " callback_workload=stereo_gain_silence\n";

    bool allMeasured = true;
    bool auditClean = true;
    for (const auto frames : std::array {64U, 128U, 256U}) {
        try {
            const auto result = runConfiguration(
                frames,
                secondsPerBuffer
            );
            printResult(result);
            auditClean = auditClean
                && result.audit.allocations == 0U
                && result.audit.deallocations == 0U
                && result.audit.blockingLocks == 0U;
        } catch (const std::exception& exception) {
            allMeasured = false;
            std::cout
                << "buffer=" << frames
                << " status=error backend=JACK message=\""
                << exception.what() << "\"\n" << std::flush;
        }
    }

    if (!auditClean) {
        std::cerr << "Real-time callback audit failed.\n";
        return 5;
    }
    return allMeasured ? 0 : 4;
}

} // namespace iramix::audio_probe
