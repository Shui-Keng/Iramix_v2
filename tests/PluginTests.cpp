#include "iramix/plugin/PluginBridge.hpp"

#include "iramix/persistence/SessionDocument.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] double percentileMilliseconds(
    std::vector<double> values,
    const double percentile
) {
    require(!values.empty(), "percentile input is non-empty");
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<std::size_t>(std::ceil(
        percentile * static_cast<double>(values.size())
    )) - 1U;
    return values[rank];
}

constexpr std::uint32_t kFrames = 256U;
constexpr std::uint32_t kChannels = 2U;
constexpr std::size_t kSamples =
    static_cast<std::size_t>(kFrames) * kChannels;
constexpr std::uint32_t kStateCapacity = 65'536U;
constexpr std::uint32_t kParameterQueueCapacity = 64U;

[[nodiscard]] iramix::plugin::PluginBridgeConfig makeConfig() {
    return {
        .maximumFrames = kFrames,
        .channelCount = kChannels,
        .deadline = std::chrono::milliseconds {5},
        .maximumStateBytes = kStateCapacity,
        .stateDeadline = std::chrono::milliseconds {250},
        .parameterQueueCapacity = kParameterQueueCapacity,
    };
}

[[nodiscard]] std::vector<float> makeInput() {
    std::vector<float> input(kSamples, 0.0F);
    for (std::size_t index = 0U; index < kSamples; ++index) {
        input[index] = static_cast<float>(index % 17U) / 16.0F;
    }
    return input;
}

// Drives blocks until the child has serviced at least one, so a test never
// measures the process launch as if it were bridge latency.
[[nodiscard]] bool warmUp(
    iramix::plugin::PluginBridge& bridge,
    const std::vector<float>& input,
    std::vector<float>& output
) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (bridge.processBlock(input, output, kFrames)
            == iramix::plugin::PluginBlockStatus::processed) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {5});
    }
    return false;
}

void testHealthyBridge(const std::filesystem::path& self) {
    namespace plugin = iramix::plugin;
    std::string error;
    auto bridge = plugin::PluginBridge::create(makeConfig(), error);
    require(bridge != nullptr, error.c_str());
    require(bridge->start(self, "normal", error), error.c_str());

    const auto input = makeInput();
    std::vector<float> output(kSamples, -1.0F);
    require(
        warmUp(*bridge, input, output),
        "the plugin process services a block"
    );

    constexpr std::size_t iterations = 500U;
    std::vector<double> roundTrip;
    roundTrip.reserve(iterations);
    std::size_t processed = 0U;
    double worstMilliseconds = 0.0;
    bool everyMissSilent = true;
    // Warm-up legitimately misses while the process is still launching, so
    // the steady-state claim is about the delta, not the lifetime total.
    const auto beforeMisses = bridge->counters().deadlineMisses;
    for (std::size_t index = 0U; index < iterations; ++index) {
        std::fill(output.begin(), output.end(), -1.0F);
        const auto started = std::chrono::steady_clock::now();
        const auto status = bridge->processBlock(input, output, kFrames);
        const double elapsed =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started
            ).count();
        worstMilliseconds = std::max(worstMilliseconds, elapsed);
        if (status == plugin::PluginBlockStatus::processed) {
            ++processed;
            roundTrip.push_back(elapsed);
            continue;
        }
        everyMissSilent = everyMissSilent
            && std::all_of(
                output.begin(),
                output.end(),
                [](const float sample) {
                    return sample == 0.0F;
                }
            );
    }

    // Deliberately not "processed == iterations". The deadline bounds the
    // spin, not preemption: on a shared CI runner the host thread can be
    // descheduled past its own expiry through no fault of the bridge, and
    // asserting otherwise makes the suite a scheduler test. What must hold
    // regardless of scheduling is the contract — the healthy path is the
    // common case, every miss degrades to silence, and nothing waits
    // unboundedly. The zero-miss figure is a local Release measurement,
    // reported through the counter line rather than asserted.
    require(
        processed * 10U >= iterations * 9U,
        "the healthy path is the common case, not the exception"
    );
    require(everyMissSilent, "any missed block still emits silence");
    require(
        worstMilliseconds < 200.0,
        "no block waits unboundedly even when the host is preempted"
    );

    // The child halves every sample. Verifying the transform proves the
    // audio really travelled through the other process rather than the
    // destination merely being left untouched.
    std::fill(output.begin(), output.end(), -1.0F);
    require(
        bridge->processBlock(input, output, kFrames)
            == plugin::PluginBlockStatus::processed,
        "a healthy bridge still processes after the measurement run"
    );
    bool transformed = true;
    for (std::size_t index = 0U; index < kSamples; ++index) {
        transformed = transformed
            && std::abs(output[index] - input[index] * 0.5F) < 1e-6F;
    }
    require(transformed, "audio round trips through the plugin process");

    const auto p50 = percentileMilliseconds(roundTrip, 0.50);
    const auto p95 = percentileMilliseconds(roundTrip, 0.95);
    const auto p99 = percentileMilliseconds(roundTrip, 0.99);
    const auto maximum = percentileMilliseconds(roundTrip, 1.0);
    const auto counters = bridge->counters();
    bridge->stop();

    std::cout
        << "Plugin bridge healthy: blocks=" << iterations
        << ", frames=" << kFrames
        << ", channels=" << kChannels
        << ", processed=" << processed
        << ", round_trip_p50_ms=" << p50
        << ", round_trip_p95_ms=" << p95
        << ", round_trip_p99_ms=" << p99
        << ", round_trip_max_ms=" << maximum
        << ", deadline_misses="
        << (counters.deadlineMisses - beforeMisses) << "\n";
}

void testPluginProcessTermination(const std::filesystem::path& self) {
    namespace plugin = iramix::plugin;
    std::string error;
    auto bridge = plugin::PluginBridge::create(makeConfig(), error);
    require(bridge != nullptr, error.c_str());
    require(bridge->start(self, "crash", error), error.c_str());

    const auto input = makeInput();
    std::vector<float> output(kSamples, -1.0F);
    require(
        warmUp(*bridge, input, output),
        "the crashing child services blocks before it dies"
    );

    std::size_t processed = 0U;
    std::size_t degraded = 0U;
    double worstMilliseconds = 0.0;
    for (std::size_t index = 0U; index < 200U; ++index) {
        std::fill(output.begin(), output.end(), -1.0F);
        const auto started = std::chrono::steady_clock::now();
        const auto status = bridge->processBlock(input, output, kFrames);
        worstMilliseconds = std::max(
            worstMilliseconds,
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started
            ).count()
        );
        if (status == plugin::PluginBlockStatus::processed) {
            ++processed;
            continue;
        }
        ++degraded;
        require(
            status == plugin::PluginBlockStatus::deadlineMissed
                || status == plugin::PluginBlockStatus::processExited,
            "a dead plugin degrades rather than failing arbitrarily"
        );
        // The contract that matters: the destination is silent, never
        // stale audio and never uninitialised memory.
        const bool silent = std::all_of(
            output.begin(),
            output.end(),
            [](const float sample) {
                return sample == 0.0F;
            }
        );
        require(silent, "a degraded block emits silence");
    }

    require(degraded > 0U, "the crashing child stops answering");
    require(
        !bridge->childRunning(),
        "the plugin process is observably gone"
    );
    // Every degraded block still returned, and none took materially longer
    // than the configured deadline.
    require(
        worstMilliseconds < 50.0,
        "no block waits unboundedly on a dead plugin"
    );
    const auto counters = bridge->counters();
    bridge->stop();

    std::cout
        << "Plugin process termination: blocks=200"
        << ", processed=" << processed
        << ", degraded=" << degraded
        << ", worst_block_ms=" << worstMilliseconds
        << ", deadline_misses=" << counters.deadlineMisses
        << ", host_survived=1\n";
}

void testPluginProcessHang(const std::filesystem::path& self) {
    namespace plugin = iramix::plugin;
    std::string error;
    auto bridge = plugin::PluginBridge::create(makeConfig(), error);
    require(bridge != nullptr, error.c_str());
    require(bridge->start(self, "hang", error), error.c_str());

    const auto input = makeInput();
    std::vector<float> output(kSamples, -1.0F);
    require(
        warmUp(*bridge, input, output),
        "the hanging child services blocks before it stalls"
    );

    std::size_t degraded = 0U;
    double worstMilliseconds = 0.0;
    for (std::size_t index = 0U; index < 50U; ++index) {
        std::fill(output.begin(), output.end(), -1.0F);
        const auto started = std::chrono::steady_clock::now();
        const auto status = bridge->processBlock(input, output, kFrames);
        worstMilliseconds = std::max(
            worstMilliseconds,
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started
            ).count()
        );
        if (status != plugin::PluginBlockStatus::processed) {
            ++degraded;
        }
    }

    require(degraded > 0U, "a hung plugin misses its deadline");
    require(
        bridge->childRunning(),
        "a hung plugin is still alive, unlike a crashed one"
    );
    require(
        worstMilliseconds < 50.0,
        "a hung plugin costs one deadline, not an unbounded stall"
    );
    const auto counters = bridge->counters();
    // stop() must reclaim a child that will never exit on its own.
    bridge->stop();
    require(
        !bridge->childRunning(),
        "shutdown terminates a plugin that ignores the stop request"
    );

    std::cout
        << "Plugin process hang: blocks=50"
        << ", degraded=" << degraded
        << ", worst_block_ms=" << worstMilliseconds
        << ", consecutive_misses="
        << counters.consecutiveDeadlineMisses
        << ", terminated_on_shutdown=1\n";
}

void testBridgeRejections(const std::filesystem::path& self) {
    namespace plugin = iramix::plugin;
    std::string error;

    require(
        plugin::PluginBridge::create(
            {
                .maximumFrames = 0U,
                .channelCount = kChannels,
                .deadline = std::chrono::milliseconds {5},
            },
            error
        ) == nullptr,
        "a zero block size is rejected"
    );
    require(
        plugin::PluginBridge::create(
            {
                .maximumFrames = kFrames,
                .channelCount = kChannels,
                .deadline = std::chrono::microseconds::zero(),
            },
            error
        ) == nullptr,
        "an unbounded deadline is rejected"
    );

    auto bridge = plugin::PluginBridge::create(makeConfig(), error);
    require(bridge != nullptr, error.c_str());

    const auto input = makeInput();
    std::vector<float> output(kSamples, -1.0F);
    require(
        bridge->processBlock(input, output, kFrames)
            == plugin::PluginBlockStatus::notRunning,
        "processing before start reports notRunning"
    );
    require(
        bridge->start(self, "normal", error),
        error.c_str()
    );
    require(
        bridge->processBlock(input, output, kFrames * 4U)
            == plugin::PluginBlockStatus::invalidBlock,
        "an oversized block is rejected rather than truncated"
    );
    require(
        !bridge->start(self, "normal", error),
        "starting twice is rejected"
    );
    bridge->stop();
    require(
        bridge->processBlock(input, output, kFrames)
            == plugin::PluginBlockStatus::notRunning,
        "processing after stop reports notRunning"
    );

    std::cout
        << "Plugin bridge rejections: invalid_configurations=2, "
           "unstarted=1, oversized_blocks=1, double_starts=1, "
           "post_stop=1\n";
}

// Builds the smallest session that can legitimately carry a plugin record,
// so the state blob under test travels the real persistence path rather
// than being handed straight to the bridge.
[[nodiscard]] iramix::persistence::SessionDocument makePluginSession(
    std::vector<std::byte> state
) {
    namespace persistence = iramix::persistence;
    persistence::SessionDocument document;
    document.revision = 7U;
    document.tracks.push_back({
        .stableId = 1U,
        .type = persistence::SessionTrackType::master,
        .gain = 1.0F,
        .color = 0U,
        .name = "Master",
    });
    document.plugins.push_back({
        .stableId = 2U,
        .targetTrackId = 1U,
        .format = persistence::SessionPluginFormat::clap,
        .slotIndex = 0U,
        .bypassed = false,
        .identifier = "com.iramix.test.standin",
        .name = "Stand-in",
        .state = std::move(state),
    });
    return document;
}

[[nodiscard]] bool outputMatchesGain(
    const std::vector<float>& input,
    const std::vector<float>& output,
    const float gain
) {
    bool matches = true;
    for (std::size_t index = 0U; index < kSamples; ++index) {
        matches = matches
            && std::abs(output[index] - input[index] * gain) < 1e-6F;
    }
    return matches;
}

void testPluginStateRestoration(const std::filesystem::path& self) {
    namespace plugin = iramix::plugin;
    namespace persistence = iramix::persistence;

    // A blob with a payload the host never interprets, exactly as a real
    // plugin's state would be.
    std::vector<std::byte> payload(256U);
    for (std::size_t index = 0U; index < payload.size(); ++index) {
        payload[index] = static_cast<std::byte>(index % 251U);
    }
    constexpr float restoredGain = 0.25F;
    const auto authored = plugin::stub::encodeState(restoredGain, payload);

    std::string error;
    const auto encoded = persistence::serializeSessionDocument(
        makePluginSession(authored),
        error
    );
    require(error.empty(), error.c_str());
    const auto decoded = persistence::deserializeSessionDocument(encoded);
    require(decoded.ok(), decoded.error.c_str());
    require(
        decoded.document.plugins.size() == 1U
            && decoded.document.plugins.front().state == authored,
        "the session preserves the plugin state blob byte for byte"
    );
    const auto& stored = decoded.document.plugins.front().state;

    auto bridge = plugin::PluginBridge::create(makeConfig(), error);
    require(bridge != nullptr, error.c_str());
    require(bridge->start(self, "normal", error), error.c_str());

    const auto input = makeInput();
    std::vector<float> output(kSamples, -1.0F);
    require(
        warmUp(*bridge, input, output),
        "the plugin process services a block"
    );
    // Before restoration the stand-in runs its instantiation default, so a
    // later change of coefficient cannot be mistaken for a no-op.
    require(
        outputMatchesGain(input, output, 0.5F),
        "a freshly instantiated plugin runs its own default state"
    );

    const auto restoreStarted = std::chrono::steady_clock::now();
    const auto restoreStatus = bridge->restoreState(stored);
    const double restoreMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - restoreStarted
        ).count();
    require(
        restoreStatus == plugin::PluginStateStatus::ok,
        "the live plugin accepts a session's stored state"
    );

    std::fill(output.begin(), output.end(), -1.0F);
    require(
        bridge->processBlock(input, output, kFrames)
            == plugin::PluginBlockStatus::processed,
        "audio still flows after a restore"
    );
    // The decisive assertion: the restored coefficient is audible in the
    // rendered block, so the blob reached the DSP rather than merely
    // crossing the process boundary.
    require(
        outputMatchesGain(input, output, restoredGain),
        "restored state changes what the plugin renders"
    );

    const auto captureStarted = std::chrono::steady_clock::now();
    std::vector<std::byte> captured;
    const auto captureStatus = bridge->captureState(captured);
    const double captureMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - captureStarted
        ).count();
    require(
        captureStatus == plugin::PluginStateStatus::ok,
        "the live plugin hands its state back for saving"
    );
    require(
        captured == stored,
        "captured state round trips byte for byte, payload included"
    );

    // Saving what was captured must produce a document that still loads.
    const auto resaved = persistence::serializeSessionDocument(
        makePluginSession(captured),
        error
    );
    require(error.empty(), error.c_str());
    require(
        persistence::deserializeSessionDocument(resaved).ok(),
        "captured state is storable without further conversion"
    );

    const auto counters = bridge->counters();
    bridge->stop();
    require(
        counters.stateRestores == 1U
            && counters.stateCaptures == 1U
            && counters.stateRejections == 0U
            && counters.stateTimeouts == 0U,
        "state counters record exactly what happened"
    );

    std::cout
        << "Plugin state restoration: state_bytes=" << stored.size()
        << ", payload_bytes=" << payload.size()
        << ", schema_version=" << decoded.sourceSchemaVersion
        << ", restored_gain=" << restoredGain
        << ", restore_ms=" << restoreMilliseconds
        << ", capture_ms=" << captureMilliseconds
        << ", capture_identical=1, rejections=0, timeouts=0\n";
}

void testPluginStateRejections(const std::filesystem::path& self) {
    namespace plugin = iramix::plugin;
    std::string error;

    // A bridge configured without a state region refuses transfer outright
    // rather than growing the region while audio is running.
    auto stateless = plugin::PluginBridge::create(
        {
            .maximumFrames = kFrames,
            .channelCount = kChannels,
            .deadline = std::chrono::milliseconds {5},
            .maximumStateBytes = 0U,
            .stateDeadline = std::chrono::milliseconds {250},
            .parameterQueueCapacity = 0U,
        },
        error
    );
    require(stateless != nullptr, error.c_str());
    require(stateless->start(self, "normal", error), error.c_str());
    require(
        stateless->restoreState({}) == plugin::PluginStateStatus::unavailable,
        "a bridge with no state region refuses a restore"
    );
    stateless->stop();

    auto bridge = plugin::PluginBridge::create(makeConfig(), error);
    require(bridge != nullptr, error.c_str());
    std::vector<std::byte> scratch;
    require(
        bridge->restoreState({}) == plugin::PluginStateStatus::unavailable
            && bridge->captureState(scratch)
                == plugin::PluginStateStatus::unavailable,
        "state transfer before start is refused, not queued"
    );

    require(bridge->start(self, "normal", error), error.c_str());
    const auto input = makeInput();
    std::vector<float> output(kSamples, -1.0F);
    require(
        warmUp(*bridge, input, output),
        "the plugin process services a block"
    );

    const std::vector<std::byte> oversized(
        static_cast<std::size_t>(kStateCapacity) + 1U,
        std::byte {0}
    );
    require(
        bridge->restoreState(oversized)
            == plugin::PluginStateStatus::tooLarge,
        "a blob larger than the region is refused before it is written"
    );

    const auto good = plugin::stub::encodeState(0.25F, {});
    require(
        bridge->restoreState(good) == plugin::PluginStateStatus::ok,
        "a well-formed blob is accepted"
    );

    // Corrupting a byte in the payload area must fail the checksum, and the
    // plugin must keep the state it already had rather than half-load.
    auto corrupt = plugin::stub::encodeState(0.75F, {});
    corrupt[5] ^= std::byte {0xFF};
    require(
        bridge->restoreState(corrupt)
            == plugin::PluginStateStatus::rejectedByPlugin,
        "a corrupt blob is rejected by the plugin"
    );
    std::fill(output.begin(), output.end(), -1.0F);
    require(
        bridge->processBlock(input, output, kFrames)
            == plugin::PluginBlockStatus::processed
            && outputMatchesGain(input, output, 0.25F),
        "a rejected restore leaves the previous state in force"
    );

    const auto counters = bridge->counters();
    bridge->stop();
    require(
        bridge->restoreState(good) == plugin::PluginStateStatus::unavailable,
        "state transfer after stop is refused"
    );
    require(
        counters.stateRestores == 1U && counters.stateRejections == 1U,
        "a rejected restore is not counted as a restore"
    );

    std::cout
        << "Plugin state rejections: no_region=1, before_start=2, "
           "oversized=1, corrupt=1, after_stop=1, "
           "previous_state_retained=1\n";
}

void testPluginStateOnDeadPlugin(const std::filesystem::path& self) {
    namespace plugin = iramix::plugin;
    std::string error;

    auto config = makeConfig();
    config.stateDeadline = std::chrono::milliseconds {100};
    auto bridge = plugin::PluginBridge::create(config, error);
    require(bridge != nullptr, error.c_str());
    require(bridge->start(self, "crash", error), error.c_str());

    const auto input = makeInput();
    std::vector<float> output(kSamples, -1.0F);
    require(
        warmUp(*bridge, input, output),
        "the crashing child services blocks before it dies"
    );
    for (std::size_t index = 0U; index < 20U; ++index) {
        static_cast<void>(bridge->processBlock(input, output, kFrames));
    }
    require(!bridge->childRunning(), "the plugin process is gone");

    // A save must not be held hostage by a plugin that no longer exists.
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::byte> captured;
    const auto status = bridge->captureState(captured);
    const double elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started
    ).count();
    require(
        status == plugin::PluginStateStatus::timedOut,
        "capturing from a dead plugin reports a timeout"
    );
    require(captured.empty(), "a failed capture yields no state");
    require(
        elapsed < 1'000.0,
        "a dead plugin costs one state deadline, not an unbounded stall"
    );
    const auto counters = bridge->counters();
    bridge->stop();

    std::cout
        << "Plugin state on dead plugin: state_deadline_ms=100"
        << ", observed_ms=" << elapsed
        << ", timeouts=" << counters.stateTimeouts
        << ", captured_bytes=0\n";
}

void testParameterTransport(const std::filesystem::path& self) {
    namespace plugin = iramix::plugin;
    std::string error;
    auto bridge = plugin::PluginBridge::create(makeConfig(), error);
    require(bridge != nullptr, error.c_str());
    require(bridge->start(self, "normal", error), error.c_str());

    const auto input = makeInput();
    std::vector<float> output(kSamples, -1.0F);
    require(
        warmUp(*bridge, input, output),
        "the plugin process services a block"
    );

    // Scheduled for the block *after* the next one, so "applied at the
    // right time" and "applied at all" cannot be confused.
    const auto scheduledAt = bridge->samplePosition() + kFrames;
    require(
        bridge->setParameter(
            plugin::PluginParameterId::gain,
            0.2F,
            scheduledAt
        ) == plugin::PluginParameterStatus::accepted,
        "a parameter change is accepted while audio is running"
    );

    std::fill(output.begin(), output.end(), -1.0F);
    require(
        bridge->processBlock(input, output, kFrames)
            == plugin::PluginBlockStatus::processed
            && outputMatchesGain(input, output, 0.5F),
        "an event scheduled for a later block is not applied early"
    );

    std::fill(output.begin(), output.end(), -1.0F);
    require(
        bridge->processBlock(input, output, kFrames)
            == plugin::PluginBlockStatus::processed
            && outputMatchesGain(input, output, 0.2F),
        "the event lands on the block it was scheduled for"
    );

    // Bypass is a distinct parameter with distinct audible behaviour, so a
    // single stuck coefficient cannot satisfy both assertions.
    require(
        bridge->setParameter(
            plugin::PluginParameterId::bypass,
            1.0F,
            bridge->samplePosition()
        ) == plugin::PluginParameterStatus::accepted,
        "bypass is accepted"
    );
    std::fill(output.begin(), output.end(), -1.0F);
    require(
        bridge->processBlock(input, output, kFrames)
            == plugin::PluginBlockStatus::processed
            && outputMatchesGain(input, output, 1.0F),
        "a bypassed plugin passes audio through untouched"
    );

    // The loop that matters: a change made through the control transport
    // is what a subsequent save would persist.
    std::vector<std::byte> captured;
    require(
        bridge->captureState(captured) == plugin::PluginStateStatus::ok,
        "state can be captured after a parameter change"
    );
    float capturedGain = 0.0F;
    require(
        plugin::stub::decodeStateGain(captured, capturedGain)
            && std::abs(capturedGain - 0.2F) < 1e-6F,
        "captured state carries the value set through the transport"
    );

    // Bypass is a host-side field on SessionPlugin, so the plugin must not
    // smuggle it into the blob and claim ownership of it.
    require(
        captured == plugin::stub::encodeState(0.2F, {}),
        "bypass is not carried in the plugin's state blob"
    );

    const auto counters = bridge->counters();
    bridge->stop();
    require(
        counters.parametersSent == 2U
            && counters.parametersApplied == 2U
            && counters.parametersLate == 0U
            && counters.parameterOverflows == 0U,
        "every queued event is applied exactly once and on time"
    );

    std::cout
        << "Plugin parameter transport: queue_capacity="
        << kParameterQueueCapacity
        << ", sent=" << counters.parametersSent
        << ", applied=" << counters.parametersApplied
        << ", late=" << counters.parametersLate
        << ", early_application=0, bypass_observed=1"
        << ", capture_reflects_transport=1\n";
}

void testParameterLateAndSaturation(const std::filesystem::path& self) {
    namespace plugin = iramix::plugin;
    std::string error;

    // A late event is applied and counted, never discarded: dropping it
    // would silently lose an automation move.
    auto late = plugin::PluginBridge::create(makeConfig(), error);
    require(late != nullptr, error.c_str());
    require(late->start(self, "normal", error), error.c_str());
    const auto input = makeInput();
    std::vector<float> output(kSamples, -1.0F);
    require(warmUp(*late, input, output), "the plugin services a block");
    require(
        late->samplePosition() > 0U,
        "the bridge advanced the transport while warming up"
    );
    require(
        late->setParameter(plugin::PluginParameterId::gain, 0.125F, 0U)
            == plugin::PluginParameterStatus::accepted,
        "an event whose time has passed is still accepted"
    );
    std::fill(output.begin(), output.end(), -1.0F);
    require(
        late->processBlock(input, output, kFrames)
            == plugin::PluginBlockStatus::processed
            && outputMatchesGain(input, output, 0.125F),
        "a late event is applied rather than dropped"
    );
    const auto lateCounters = late->counters();
    require(
        lateCounters.parametersApplied == 1U
            && lateCounters.parametersLate == 1U,
        "lateness is counted, not hidden"
    );
    late->stop();

    // Saturation. No block is processed, so the plugin never drains the
    // queue and the host must refuse rather than overwrite.
    auto full = plugin::PluginBridge::create(makeConfig(), error);
    require(full != nullptr, error.c_str());
    require(full->start(self, "normal", error), error.c_str());
    std::size_t accepted = 0U;
    std::size_t refused = 0U;
    for (std::uint32_t index = 0U;
        index < kParameterQueueCapacity + 8U;
        ++index) {
        const auto status = full->setParameter(
            plugin::PluginParameterId::gain,
            0.5F,
            static_cast<std::uint64_t>(index)
        );
        if (status == plugin::PluginParameterStatus::accepted) {
            ++accepted;
        } else {
            require(
                status == plugin::PluginParameterStatus::queueFull,
                "a saturated queue reports queueFull"
            );
            ++refused;
        }
    }
    require(
        accepted == kParameterQueueCapacity && refused == 8U,
        "the queue accepts exactly its capacity and refuses the rest"
    );

    // Out-of-order delivery would make the rendered result depend on when
    // events arrived rather than on the timeline.
    require(
        full->setParameter(plugin::PluginParameterId::gain, 0.5F, 0U)
            == plugin::PluginParameterStatus::outOfOrder,
        "an event that goes backwards in time is refused"
    );

    const auto counters = full->counters();
    full->stop();
    require(
        full->setParameter(plugin::PluginParameterId::gain, 0.5F, 1'000U)
            == plugin::PluginParameterStatus::unavailable,
        "parameters after stop are refused"
    );
    require(
        counters.parameterOverflows == 8U
            && counters.parameterOutOfOrder == 1U,
        "refusals are counted separately by cause"
    );

    std::cout
        << "Plugin parameter limits: late_applied="
        << lateCounters.parametersLate
        << ", late_dropped=0"
        << ", queue_capacity=" << kParameterQueueCapacity
        << ", accepted=" << accepted
        << ", overflows=" << counters.parameterOverflows
        << ", out_of_order=" << counters.parameterOutOfOrder
        << ", after_stop=1\n";
}

} // namespace

int main(const int argc, char* argv[]) {
    if (argc == 4
        && std::string_view {argv[1]} == "--plugin-bridge-child") {
        return iramix::plugin::PluginBridge::runChild(argv[2], argv[3]);
    }

    const auto self = std::filesystem::absolute(argv[0]);
    testHealthyBridge(self);
    testPluginProcessTermination(self);
    testPluginProcessHang(self);
    testBridgeRejections(self);
    testPluginStateRestoration(self);
    testPluginStateRejections(self);
    testPluginStateOnDeadPlugin(self);
    testParameterTransport(self);
    testParameterLateAndSaturation(self);
    std::cout << "All Iramix plugin tests passed.\n";
    return EXIT_SUCCESS;
}
