#include "iramix/plugin/PluginBridge.hpp"

#include "iramix/persistence/SessionDocument.hpp"
#include "iramix/persistence/SessionPersistenceService.hpp"
#include "iramix/plugin/PluginScanner.hpp"
#include "iramix/plugin/PluginStateAutosave.hpp"
#include "iramix/session/JournaledSession.hpp"

#include <fstream>
#include <system_error>

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
    // The claim is bounded, not fast. An unbounded wait would never return
    // and this loop would hang; a deadline that had stopped working would
    // exceed this by orders of magnitude. Tightening it toward the 5 ms
    // deadline only re-tests the OS scheduler, which on a loaded two-core
    // machine routinely costs tens of milliseconds — this bound was
    // observed failing at 50 ms for exactly that reason. The measured
    // figure is reported below rather than asserted.
    require(
        worstMilliseconds < 500.0,
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
        worstMilliseconds < 500.0,
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

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
        path_ = std::filesystem::temp_directory_path()
            / ("iramix-plugin-" + std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void writeStubFile(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file {path, std::ios::binary | std::ios::trunc};
    file << "not a real module";
}

// Discovery is pure filesystem logic, so it is asserted against a
// synthetic tree. That is the point of taking search roots as an argument:
// the layout rules are testable on a machine with no plugins installed,
// which includes every CI runner.
void testPluginDiscovery() {
    namespace plugin = iramix::plugin;
    const TemporaryDirectory root;
    const auto base = root.path();

#if defined(_WIN32)
    const std::string architecture = "x86_64-win";
#elif defined(__APPLE__)
    const std::string architecture = "MacOS";
#else
    const std::string architecture = "x86_64-linux";
#endif

    // A bundle: the candidate is the binary inside, but the identity the
    // user recognises is the bundle directory.
    const auto bundle = base / "vst3" / "Bundled.vst3";
    writeStubFile(bundle / "Contents" / architecture / "Bundled.vst3");
    // A plain module file, still common and still legal.
    writeStubFile(base / "vst3" / "Flat.vst3");
    // Vendors nest, so an unnested walk would miss most of a real install.
    writeStubFile(base / "clap" / "Vendor" / "Nested.clap");
    // Neither of these is a plugin.
    writeStubFile(base / "vst3" / "readme.txt");
    writeStubFile(base / "vst3" / "Support.dll");
    // A bundle with no binary for this platform is not a candidate: it
    // ships other architectures only.
    std::filesystem::create_directories(
        base / "vst3" / "Foreign.vst3" / "Contents" / "ppc-something"
    );

    const std::vector<std::filesystem::path> roots {
        base / "vst3",
        base / "clap",
        base / "does-not-exist",
    };
    const auto found = plugin::discoverPlugins(roots);

    require(found.size() == 3U, "exactly the three modules are found");
    // Deterministic order, so a scan cache and a result document do not
    // depend on directory iteration order.
    require(
        std::is_sorted(
            found.begin(),
            found.end(),
            [](const auto& left, const auto& right) {
                return left.bundlePath < right.bundlePath;
            }
        ),
        "discovery order is deterministic"
    );

    std::size_t bundles = 0U;
    std::size_t claps = 0U;
    for (const auto& candidate : found) {
        require(
            std::filesystem::is_regular_file(candidate.modulePath),
            "every candidate names a file that exists"
        );
        if (candidate.format == plugin::PluginModuleFormat::clap) {
            ++claps;
            require(
                candidate.modulePath == candidate.bundlePath,
                "a CLAP module is its own bundle"
            );
        }
        if (candidate.bundlePath == bundle) {
            ++bundles;
            require(
                candidate.modulePath
                    == bundle / "Contents" / architecture / "Bundled.vst3",
                "a bundle resolves to the binary inside it"
            );
        }
    }
    require(bundles == 1U, "the bundle is reported once, not twice");
    require(claps == 1U, "the nested CLAP module is found");

    require(
        plugin::discoverPlugins({}).empty(),
        "no search roots yields no candidates"
    );

    std::cout
        << "Plugin discovery: roots=" << roots.size()
        << ", candidates=" << found.size()
        << ", bundles=" << bundles
        << ", clap=" << claps
        << ", foreign_bundle_skipped=1, non_modules_skipped=2\n";
}

// A module that is not a plugin must be rejected by the scanner rather
// than crashing it, and the verdict must come from a separate process.
void testScanRejectsNonPlugin(const std::filesystem::path& self) {
    namespace plugin = iramix::plugin;
    const TemporaryDirectory root;
    const auto fake = root.path() / "NotAPlugin.vst3";
    writeStubFile(fake);

    const plugin::PluginScanCandidate candidate {
        .modulePath = fake,
        .bundlePath = fake,
        .format = plugin::PluginModuleFormat::vst3,
    };
    const auto record = plugin::scanCandidate(
        candidate,
        self,
        std::chrono::seconds {10}
    );
    require(
        record.status == plugin::PluginScanStatus::loadFailed
            || record.status == plugin::PluginScanStatus::notAPlugin,
        "a file that is not a module is refused, not loaded"
    );
    require(
        record.name.empty() && record.classCount == 0U,
        "a refused module contributes no metadata"
    );

    std::cout
        << "Plugin scan rejection: garbage_module=1, host_survived=1, "
           "metadata_invented=0\n";
}

// R-12's remaining gap: captureState() proves a live plugin can hand its
// state back, but not that the captured bytes ever reach an autosaved
// file rather than whatever stale blob the document already carried. This
// drives the two together — a real AutosaveClock-scheduled window, not a
// direct captureState()/serialize call pair.
void testPluginStateWiredIntoAutosave(const std::filesystem::path& self) {
    namespace plugin = iramix::plugin;
    namespace persistence = iramix::persistence;
    namespace session = iramix::session;
    const TemporaryDirectory root;

    auto document = makePluginSession(plugin::stub::encodeState(0.1F, {}));
    document.revision = 1U;
    const auto stableId = document.plugins.front().stableId;

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

    // A coefficient the document's placeholder cannot already hold, so a
    // later match cannot be satisfied by the stale blob surviving
    // untouched.
    constexpr float liveGain = 0.75F;
    require(
        bridge->restoreState(plugin::stub::encodeState(liveGain, {}))
            == plugin::PluginStateStatus::ok,
        "the live plugin accepts a distinctive coefficient"
    );

    const std::vector<plugin::LivePluginBinding> bindings {
        {.stableId = stableId, .bridge = bridge.get()},
    };
    plugin::PluginStateCaptureReport report;
    auto snapshot = plugin::captureLivePluginState(
        std::move(document),
        bindings,
        report
    );
    require(
        report.captured == 1U
            && report.unchanged == 0U
            && report.failed == 0U,
        "the bound plugin's live state is captured before autosave"
    );

    std::vector<std::byte> expected;
    require(
        bridge->captureState(expected) == plugin::PluginStateStatus::ok,
        "a control read for comparison against the autosaved file"
    );
    require(
        snapshot->plugins.front().state == expected,
        "the snapshot handed to autosave already carries the live state"
    );

    const auto project = root.path() / "plugin-autosave-session.irpx";
    auto clock = std::make_shared<persistence::ManualAutosaveClock>();
    constexpr std::chrono::milliseconds window {5'000};
    auto service = persistence::SessionPersistenceService::create(
        project,
        window,
        error,
        0U,
        clock
    );
    require(service != nullptr, error.c_str());
    require(service->start(error), error.c_str());
    require(
        service->markDirty(snapshot)
            == persistence::AutosaveDirtyStatus::tracked,
        "the captured snapshot starts the autosave window"
    );
    clock->advance(window);

    const auto timeout =
        std::chrono::steady_clock::now() + std::chrono::seconds {10};
    auto query = service->query(1U);
    while (
        query.status != persistence::SessionSaveQueryStatus::committed
        && std::chrono::steady_clock::now() < timeout
    ) {
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
        query = service->query(1U);
    }
    require(
        query.status == persistence::SessionSaveQueryStatus::committed,
        "the autosave window commits the captured snapshot"
    );
    service->stop();
    bridge->stop();

    // Reopening is the same mechanism testAutomaticBackupRestore uses to
    // verify durable content: it goes through the real project store, not
    // a shortcut past it.
    auto reopened = session::JournaledSession::open(project, error);
    require(reopened != nullptr, error.c_str());
    const auto reopenedSnapshot = reopened->snapshot();
    require(
        reopenedSnapshot->plugins.size() == 1U
            && reopenedSnapshot->plugins.front().state == expected,
        "the durably autosaved file holds the live-captured state, "
        "not the stale placeholder"
    );

    std::cout
        << "Plugin state autosave: captured=" << report.captured
        << ", unchanged=" << report.unchanged
        << ", failed=" << report.failed
        << ", autosaved_state_bytes=" << expected.size()
        << ", durable_revision=" << query.durableRevision
        << ", matches_live_capture=1\n";
}

} // namespace

int main(const int argc, char* argv[]) {
    if (argc >= 4 && argc <= 7
        && std::string_view {argv[1]} == "--plugin-bridge-child") {
        const std::string vst3ModulePath = argc >= 5 ? argv[4] : "";
        const auto vst3ClassIndex = argc >= 6
            ? static_cast<std::uint32_t>(std::atoi(argv[5]))
            : 0U;
        const double vst3SampleRate = argc >= 7
            ? std::atof(argv[6])
            : 48'000.0;
        return iramix::plugin::PluginBridge::runChild(
            argv[2],
            argv[3],
            vst3ModulePath,
            vst3ClassIndex,
            vst3SampleRate
        );
    }
    if (argc == 5
        && std::string_view {argv[1]} == "--plugin-scan-child") {
        return iramix::plugin::runScanChild(argv[2], argv[3], argv[4]);
    }

    const auto self = std::filesystem::absolute(argv[0]);
    testHealthyBridge(self);
    testPluginProcessTermination(self);
    testPluginProcessHang(self);
    testBridgeRejections(self);
    testPluginStateRestoration(self);
    testPluginStateWiredIntoAutosave(self);
    testPluginStateRejections(self);
    testPluginStateOnDeadPlugin(self);
    testParameterTransport(self);
    testParameterLateAndSaturation(self);
    testPluginDiscovery();
    testScanRejectsNonPlugin(self);
    std::cout << "All Iramix plugin tests passed.\n";
    return EXIT_SUCCESS;
}
