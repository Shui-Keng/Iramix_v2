#include "AudioProbe.hpp"
#include "iramix/realtime/Audit.hpp"

#include <CoreAudio/CoreAudio.h>

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

constexpr std::uint32_t kSampleRate = 48'000U;
constexpr std::size_t kChannels = 2U;
constexpr std::uint64_t kWarmupCallbacks = 100U;
constexpr std::size_t kLateWakeupBucketCount = 10U;

struct Context {
    std::uint32_t frames {0U};
    std::uint32_t seconds {0U};
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
    std::atomic<bool> measurementComplete {false};
    Clock::time_point measurementStarted {};
    Clock::time_point previousWakeup {};
};

struct Result {
    std::uint32_t frames {0U};
    std::uint64_t callbacks {0U};
    std::uint64_t expectedCallbacks {0U};
    std::uint64_t lateWakeups {0U};
    std::array<std::uint64_t, kLateWakeupBucketCount> lateBuckets {};
    std::uint64_t p50 {0U};
    std::uint64_t p95 {0U};
    std::uint64_t p99 {0U};
    std::uint64_t maximum {0U};
    std::uint64_t targetMisses {0U};
    std::uint64_t hardDeadlineMisses {0U};
    realtime::AuditSnapshot audit;
};

[[nodiscard]] AudioObjectPropertyAddress property(
    const AudioObjectPropertySelector selector,
    const AudioObjectPropertyScope scope =
        kAudioObjectPropertyScopeGlobal
) {
    return {
        selector,
        scope,
        kAudioObjectPropertyElementMain,
    };
}

[[noreturn]] void fail(const char* operation, const OSStatus status) {
    throw std::runtime_error(
        std::string {operation}
        + " failed with OSStatus "
        + std::to_string(status)
    );
}

void requireSuccess(const OSStatus status, const char* operation) {
    if (status != noErr) {
        fail(operation, status);
    }
}

template<typename Value>
[[nodiscard]] Value getProperty(
    const AudioObjectID object,
    const AudioObjectPropertyAddress& address,
    const char* operation
) {
    Value value {};
    auto size = static_cast<UInt32>(sizeof(value));
    requireSuccess(
        AudioObjectGetPropertyData(
            object,
            &address,
            0U,
            nullptr,
            &size,
            &value
        ),
        operation
    );
    return value;
}

template<typename Value>
void setProperty(
    const AudioObjectID object,
    const AudioObjectPropertyAddress& address,
    const Value& value,
    const char* operation
) {
    requireSuccess(
        AudioObjectSetPropertyData(
            object,
            &address,
            0U,
            nullptr,
            static_cast<UInt32>(sizeof(value)),
            &value
        ),
        operation
    );
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

OSStatus audioCallback(
    AudioDeviceID,
    const AudioTimeStamp*,
    const AudioBufferList*,
    const AudioTimeStamp*,
    AudioBufferList* outputData,
    const AudioTimeStamp*,
    void* clientData
) noexcept {
    auto& context = *static_cast<Context*>(clientData);
    if (context.measurementComplete.load(std::memory_order_relaxed)) {
        if (outputData != nullptr) {
            for (
                UInt32 index = 0U;
                index < outputData->mNumberBuffers;
                ++index
            ) {
                auto& buffer = outputData->mBuffers[index];
                if (buffer.mData != nullptr) {
                    std::memset(buffer.mData, 0, buffer.mDataByteSize);
                }
            }
        }
        return noErr;
    }

    const auto callbackStarted = Clock::now();
    std::uint64_t elapsedNanoseconds = 0U;
    {
        realtime::CallbackScope callbackScope;
        for (
            std::size_t index = 0U;
            index < context.output.size();
            ++index
        ) {
            context.output[index] = context.input[index] * 0.5F;
        }
        if (outputData != nullptr) {
            for (
                UInt32 index = 0U;
                index < outputData->mNumberBuffers;
                ++index
            ) {
                auto& buffer = outputData->mBuffers[index];
                if (buffer.mData != nullptr) {
                    std::memset(buffer.mData, 0, buffer.mDataByteSize);
                }
            }
        }
        elapsedNanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - callbackStarted
            ).count()
        );
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
        return noErr;
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
    return noErr;
}

Result runConfiguration(
    const AudioDeviceID device,
    const std::uint32_t frames,
    const std::uint32_t seconds
) {
    const auto frameAddress = property(
        kAudioDevicePropertyBufferFrameSize
    );
    const auto requestedFrames = static_cast<UInt32>(frames);
    setProperty(
        device,
        frameAddress,
        requestedFrames,
        "set buffer frame size"
    );
    const auto actualFrames = getProperty<UInt32>(
        device,
        frameAddress,
        "read buffer frame size"
    );
    if (actualFrames != requestedFrames) {
        throw std::runtime_error(
            "Core Audio did not retain the requested buffer size"
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

    AudioDeviceIOProcID callbackId = nullptr;
    requireSuccess(
        AudioDeviceCreateIOProcID(
            device,
            &audioCallback,
            &context,
            &callbackId
        ),
        "AudioDeviceCreateIOProcID"
    );

    try {
        realtime::resetAuditCounters();
        requireSuccess(
            AudioDeviceStart(device, callbackId),
            "AudioDeviceStart"
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
        requireSuccess(
            AudioDeviceStop(device, callbackId),
            "AudioDeviceStop"
        );
        if (!stoppedInTime) {
            throw std::runtime_error("Core Audio callback timed out");
        }
    } catch (...) {
        AudioDeviceStop(device, callbackId);
        AudioDeviceDestroyIOProcID(device, callbackId);
        throw;
    }
    requireSuccess(
        AudioDeviceDestroyIOProcID(device, callbackId),
        "AudioDeviceDestroyIOProcID"
    );

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
    result.audit = realtime::auditSnapshot();

    context.durations.resize(static_cast<std::size_t>(result.callbacks));
    if (context.durations.empty()) {
        throw std::runtime_error("Core Audio produced no callbacks");
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
        << " status=measured backend=CoreAudio"
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
    const auto defaultDeviceAddress = property(
        kAudioHardwarePropertyDefaultOutputDevice
    );
    const auto device = getProperty<AudioDeviceID>(
        kAudioObjectSystemObject,
        defaultDeviceAddress,
        "read default output device"
    );
    if (device == kAudioObjectUnknown) {
        throw std::runtime_error("No Core Audio output device");
    }

    const auto sampleRateAddress = property(
        kAudioDevicePropertyNominalSampleRate
    );
    const auto frameAddress = property(
        kAudioDevicePropertyBufferFrameSize
    );
    const auto originalSampleRate = getProperty<Float64>(
        device,
        sampleRateAddress,
        "read nominal sample rate"
    );
    const auto originalFrames = getProperty<UInt32>(
        device,
        frameAddress,
        "read original buffer frame size"
    );

    int exitCode = 0;
    try {
        const Float64 requestedSampleRate = kSampleRate;
        setProperty(
            device,
            sampleRateAddress,
            requestedSampleRate,
            "set nominal sample rate"
        );
        const auto actualSampleRate = getProperty<Float64>(
            device,
            sampleRateAddress,
            "verify nominal sample rate"
        );
        if (std::abs(actualSampleRate - requestedSampleRate) > 0.5) {
            throw std::runtime_error(
                "Core Audio did not retain the requested 48 kHz rate"
            );
        }

        std::cout
            << "audio_probe backend=CoreAudio"
            << " sample_rate=" << actualSampleRate
            << " seconds_per_buffer=" << secondsPerBuffer
            << " callback_workload=stereo_gain_silence\n";

        bool allMeasured = true;
        bool auditClean = true;
        for (const auto frames : std::array {64U, 128U, 256U}) {
            try {
                const auto result = runConfiguration(
                    device,
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
                    << " status=error backend=CoreAudio message=\""
                    << exception.what() << "\"\n" << std::flush;
            }
        }
        if (!auditClean) {
            std::cerr << "Real-time callback audit failed.\n";
            exitCode = 5;
        } else if (!allMeasured) {
            exitCode = 4;
        }
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        exitCode = 4;
    }

    try {
        setProperty(
            device,
            frameAddress,
            originalFrames,
            "restore buffer frame size"
        );
        setProperty(
            device,
            sampleRateAddress,
            originalSampleRate,
            "restore nominal sample rate"
        );
    } catch (const std::exception& exception) {
        std::cerr << "Core Audio restore failed: "
                  << exception.what() << '\n';
        exitCode = 4;
    }
    return exitCode;
}

// Core Audio enumeration is not implemented yet. Reporting unsupported is
// deliberate: an empty inventory would be indistinguishable from a machine
// with no audio hardware, and the resolver would then report a missing
// backend for a session that is perfectly restorable here.
DeviceInventory enumerateDevices() {
    return {
        .supported = false,
        .devices = {},
        .error = "Core Audio device enumeration is not implemented",
    };
}

int runRestoredDevice(
    const std::filesystem::path&,
    const std::uint32_t
) {
    std::cerr
        << "Core Audio device restoration is not implemented.\n";
    return 3;
}

} // namespace iramix::audio_probe
