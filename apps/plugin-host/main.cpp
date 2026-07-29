#include "iramix/plugin/PluginBridge.hpp"
#include "iramix/plugin/PluginScanner.hpp"
#include "iramix/plugin/Vst3Host.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::uint32_t kFrames = 256U;
constexpr std::uint32_t kChannels = 2U;
constexpr double kSampleRate = 48'000.0;
constexpr std::uint32_t kStateCapacity = 65'536U;

[[nodiscard]] std::vector<float> makeInput() {
    std::vector<float> input(
        static_cast<std::size_t>(kFrames) * kChannels,
        0.0F
    );
    for (std::uint32_t frame = 0U; frame < kFrames; ++frame) {
        // A quiet sine rather than noise, so a plugin that filters or
        // gates has something predictable to act on.
        const auto value = 0.25F * std::sin(
            6.2831853F * 440.0F * static_cast<float>(frame)
                / static_cast<float>(kSampleRate)
        );
        for (std::uint32_t channel = 0U; channel < kChannels; ++channel) {
            input[frame * kChannels + channel] = value;
        }
    }
    return input;
}

[[nodiscard]] double peak(const std::vector<float>& block) {
    double highest = 0.0;
    for (const auto sample : block) {
        highest = std::max(highest, std::abs(static_cast<double>(sample)));
    }
    return highest;
}

// Peak alone proves nothing: an allpass filter changes the signal without
// changing its peak, and a plugin that returned its input untouched would
// look identical. The fraction of samples that actually differ is what
// distinguishes "the audio went through the plugin's DSP" from "the
// buffer was copied".
[[nodiscard]] double changedFraction(
    const std::vector<float>& before,
    const std::vector<float>& after
) {
    if (before.size() != after.size() || before.empty()) {
        return 0.0;
    }
    std::size_t changed = 0U;
    for (std::size_t index = 0U; index < before.size(); ++index) {
        if (std::abs(before[index] - after[index]) > 1e-6F) {
            ++changed;
        }
    }
    return static_cast<double>(changed)
        / static_cast<double>(before.size());
}

// Drives blocks until either one is serviced or the child is confirmed
// gone, so a plugin that failed to open in the child fails fast rather
// than spinning through hundreds of dead attempts.
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
        if (!bridge.childRunning()) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {5});
    }
    return false;
}

// Proves the bridge's crash/hang recovery contract when the process that
// fails was, until the moment it does, genuinely running a real plugin's
// own DSP rather than only the synthetic stand-in
// (testPluginProcessTermination / testPluginProcessHang in
// iramix_plugin_tests exercise the stand-in only).
[[nodiscard]] int runFaultProbe(
    const std::filesystem::path& self,
    const std::filesystem::path& target,
    const std::uint32_t classIndex,
    const bool hang
) {
    namespace plugin = iramix::plugin;

    plugin::PluginBridgeConfig config {
        .maximumFrames = kFrames,
        .channelCount = kChannels,
        .deadline = std::chrono::milliseconds {20},
        .maximumStateBytes = kStateCapacity,
        .stateDeadline = std::chrono::milliseconds {250},
        .parameterQueueCapacity = 0U,
    };

    std::string error;
    auto bridge = plugin::PluginBridge::create(config, error);
    if (bridge == nullptr) {
        std::cout << "Plugin bridge fault: created=0, reason="
                   << error << "\n";
        return EXIT_FAILURE;
    }
    if (!bridge->startVst3Fault(
            self,
            target,
            classIndex,
            kSampleRate,
            hang,
            error
        )) {
        std::cout << "Plugin bridge fault: started=0, reason="
                   << error << "\n";
        return EXIT_FAILURE;
    }

    const auto input = makeInput();
    std::vector<float> output(input.size(), 0.0F);
    if (!warmUp(*bridge, input, output)) {
        std::cout
            << "Plugin bridge fault: opened=0, "
               "reason=child_exited_before_first_block\n";
        return EXIT_FAILURE;
    }

    std::size_t processed = 0U;
    std::size_t degraded = 0U;
    bool everyDegradedSilent = true;
    double worstMilliseconds = 0.0;
    constexpr std::size_t attempts = 100U;
    for (std::size_t index = 0U; index < attempts; ++index) {
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
        everyDegradedSilent = everyDegradedSilent
            && std::all_of(
                output.begin(),
                output.end(),
                [](const float sample) {
                    return sample == 0.0F;
                }
            );
    }

    const auto counters = bridge->counters();
    bridge->stop();

    // "processed" here is what this driving process observed during the
    // measured window, not how many blocks the plugin actually rendered:
    // the fault always fires after exactly three genuine blocks by
    // construction (runChild() only reaches the "handled == 3" check
    // after the real plugin's own DSP has run), but Win32 auto-reset
    // event coalescing means several of the warm-up loop's own retries
    // during the plugin's multi-hundred-millisecond load time can be
    // serviced — and the fault triggered — before this process observes
    // its first success. A low or zero "processed" figure is therefore
    // expected, not a failure; degraded/silent/survived are what this
    // probe actually asserts.
    //
    // Reaching this line at all is part of the evidence: the host process
    // is still executing normally after stop(), including after the hang
    // case, whose only recovery path is stop()'s bounded
    // wait-then-terminate.
    std::cout
        << "Plugin bridge fault: module=" << target.filename().string()
        << ", fault=" << (hang ? "hang" : "crash")
        << ", attempts=" << attempts
        << ", processed=" << processed
        << ", degraded=" << degraded
        << ", every_degraded_silent=" << (everyDegradedSilent ? 1 : 0)
        << ", worst_block_ms=" << worstMilliseconds
        << ", deadline_misses=" << counters.deadlineMisses
        << ", exited_blocks=" << counters.exitedBlocks
        << ", host_survived=1\n";
    return (degraded > 0U && everyDegradedSilent)
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}

// Proves a real plugin's own IEditController parameter — never the
// stand-in's synthetic gain/bypass — is what the bridge's existing
// parameter transport now delivers. Correctness is judged by the
// plugin's own state, not a guessed audio effect: an arbitrary
// parameter is not guaranteed to be audible against a fixed test tone
// (and may address something silent, like a bypass or mode switch), but
// a plugin that reflects controller-driven changes into getState()
// output at all can only show a different blob if the change reached
// real plugin code, not merely the transport.
[[nodiscard]] int runParameterProbe(
    const std::filesystem::path& self,
    const std::filesystem::path& target,
    const std::uint32_t classIndex
) {
    namespace plugin = iramix::plugin;

    plugin::PluginBridgeConfig config {
        .maximumFrames = kFrames,
        .channelCount = kChannels,
        .deadline = std::chrono::milliseconds {20},
        .maximumStateBytes = kStateCapacity,
        .stateDeadline = std::chrono::milliseconds {250},
        .parameterQueueCapacity = 64U,
    };

    std::string error;
    auto bridge = plugin::PluginBridge::create(config, error);
    if (bridge == nullptr) {
        std::cout << "Plugin bridge parameters: created=0, reason="
                   << error << "\n";
        return EXIT_FAILURE;
    }
    if (!bridge->startVst3(self, target, classIndex, kSampleRate, error)) {
        std::cout << "Plugin bridge parameters: started=0, reason="
                   << error << "\n";
        return EXIT_FAILURE;
    }

    const auto input = makeInput();
    std::vector<float> output(input.size(), 0.0F);
    if (!warmUp(*bridge, input, output)) {
        std::cout
            << "Plugin bridge parameters: opened=0, "
               "reason=child_exited_before_first_block\n";
        return EXIT_FAILURE;
    }

    const auto metadata = bridge->parameterMetadata();
    if (metadata.count == 0U) {
        std::cout
            << "Plugin bridge parameters: module="
            << target.filename().string()
            << ", parameter_count=0, "
               "reason=no_ieditcontroller_or_no_parameters\n";
        bridge->stop();
        return EXIT_SUCCESS;
    }

    std::vector<std::byte> before;
    const bool capturedBefore =
        bridge->captureState(before) == plugin::PluginStateStatus::ok;

    // Away from the default and still in range: a value equal to the
    // plugin's own default could leave state unchanged for a legitimate
    // reason having nothing to do with whether delivery worked.
    const float requestedValue =
        metadata.firstParameterDefault > 0.5F ? 0.0F : 1.0F;
    const auto setStatus = bridge->setParameterById(
        metadata.firstParameterId,
        requestedValue,
        bridge->samplePosition()
    );

    // Several blocks, not one: the change is scheduled for "now" by
    // sample position, and giving it more than one block's worth of
    // margin keeps this probe from depending on exactly which block the
    // scheduler lands it in.
    for (int index = 0; index < 10; ++index) {
        static_cast<void>(bridge->processBlock(input, output, kFrames));
    }

    std::vector<std::byte> after;
    const bool capturedAfter =
        bridge->captureState(after) == plugin::PluginStateStatus::ok;

    const auto counters = bridge->counters();
    bridge->stop();

    const bool stateChanged =
        capturedBefore && capturedAfter && before != after;

    std::cout
        << "Plugin bridge parameters: module="
        << target.filename().string()
        << ", parameter_count=" << metadata.count
        << ", first_parameter_id=" << metadata.firstParameterId
        << ", first_parameter_default=" << metadata.firstParameterDefault
        << ", requested_value=" << requestedValue
        << ", set_status="
        << (setStatus == plugin::PluginParameterStatus::accepted
                ? "accepted"
                : "rejected")
        << ", parameters_applied=" << counters.parametersApplied
        << ", state_before_bytes=" << before.size()
        << ", state_after_bytes=" << after.size()
        << ", state_changed_by_parameter=" << (stateChanged ? 1 : 0)
        << "\n";
    return (setStatus == plugin::PluginParameterStatus::accepted
            && counters.parametersApplied > 0U)
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}

} // namespace

int main(const int argc, char* argv[]) {
    namespace plugin = iramix::plugin;

    // Self-exec dispatch: this same binary becomes the bridge's child
    // process. Vst3Host only ever runs on that side — the point being
    // measured here is that hosting happens inside the isolated child
    // rather than in this driving process, which is what distinguishes
    // this from the earlier in-process probe.
    if (argc >= 4 && argc <= 7
        && std::string_view {argv[1]} == "--plugin-bridge-child") {
        const std::string vst3ModulePath = argc >= 5 ? argv[4] : "";
        const auto vst3ClassIndex = argc >= 6
            ? static_cast<std::uint32_t>(std::atoi(argv[5]))
            : 0U;
        const double vst3SampleRate = argc >= 7
            ? std::atof(argv[6])
            : kSampleRate;
        return plugin::PluginBridge::runChild(
            argv[2],
            argv[3],
            vst3ModulePath,
            vst3ClassIndex,
            vst3SampleRate
        );
    }

    if (!plugin::Vst3Host::available()) {
        std::cout
            << "Plugin bridge host: available=0, "
               "reason=built_without_vst3_sdk\n";
        return EXIT_SUCCESS;
    }
    if (argc < 2) {
        std::cerr
            << "usage: iramix_plugin_host <module-or-bundle> "
               "[class-index] [--crash|--hang|--params]\n";
        return EXIT_FAILURE;
    }

    std::filesystem::path target {argv[1]};
    std::uint32_t classIndex = 0U;
    std::string faultFlag;
    bool paramsFlag = false;
    for (int index = 2; index < argc; ++index) {
        const std::string_view value {argv[index]};
        if (value == "--crash" || value == "--hang") {
            faultFlag = value;
        } else if (value == "--params") {
            paramsFlag = true;
        } else {
            classIndex = static_cast<std::uint32_t>(std::atoi(argv[index]));
        }
    }

    // Accept a bundle directory as well as a binary, so the same path that
    // came out of the scanner can be pasted straight in.
    if (std::filesystem::is_directory(target)) {
        const std::vector<std::filesystem::path> roots {
            target.parent_path(),
        };
        for (const auto& candidate : plugin::discoverPlugins(roots)) {
            if (candidate.bundlePath == target) {
                target = candidate.modulePath;
                break;
            }
        }
    }

    if (!faultFlag.empty()) {
        return runFaultProbe(
            std::filesystem::absolute(argv[0]),
            target,
            classIndex,
            faultFlag == "--hang"
        );
    }
    if (paramsFlag) {
        return runParameterProbe(
            std::filesystem::absolute(argv[0]),
            target,
            classIndex
        );
    }

    plugin::PluginBridgeConfig config {
        .maximumFrames = kFrames,
        .channelCount = kChannels,
        // Generous rather than tight: this measures whether real hosting
        // works through the bridge, not scheduler behaviour under a tight
        // budget — that is PluginBridgeTests' healthy-path job, run
        // against the stand-in plugin.
        .deadline = std::chrono::milliseconds {20},
        .maximumStateBytes = kStateCapacity,
        .stateDeadline = std::chrono::milliseconds {250},
        .parameterQueueCapacity = 0U,
    };

    std::string error;
    auto bridge = plugin::PluginBridge::create(config, error);
    if (bridge == nullptr) {
        std::cout << "Plugin bridge host: created=0, reason="
                   << error << "\n";
        return EXIT_FAILURE;
    }

    const auto self = std::filesystem::absolute(argv[0]);
    const auto openStarted = std::chrono::steady_clock::now();
    if (!bridge->startVst3(self, target, classIndex, kSampleRate, error)) {
        std::cout << "Plugin bridge host: started=0, reason="
                   << error << "\n";
        return EXIT_FAILURE;
    }

    const auto input = makeInput();
    std::vector<float> output(input.size(), 0.0F);
    if (!warmUp(*bridge, input, output)) {
        std::cout
            << "Plugin bridge host: opened=0, "
               "reason=child_exited_before_first_block\n";
        return EXIT_FAILURE;
    }
    const double openMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - openStarted
        ).count();

    // Steady-state processing. The first blocks are excluded from the
    // timing because a plugin may still be building tables, and this
    // round trip additionally includes the bridge's own IPC, unlike the
    // in-process probe's timing.
    constexpr std::size_t warmUpBlocks = 32U;
    constexpr std::size_t measuredBlocks = 500U;
    std::size_t processed = 0U;
    for (std::size_t index = 0U; index < warmUpBlocks; ++index) {
        if (bridge->processBlock(input, output, kFrames)
            == plugin::PluginBlockStatus::processed) {
            ++processed;
        }
    }

    std::vector<double> blockMilliseconds;
    blockMilliseconds.reserve(measuredBlocks);
    for (std::size_t index = 0U; index < measuredBlocks; ++index) {
        const auto started = std::chrono::steady_clock::now();
        const auto status = bridge->processBlock(input, output, kFrames);
        blockMilliseconds.push_back(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started
            ).count()
        );
        if (status == plugin::PluginBlockStatus::processed) {
            ++processed;
        }
    }
    std::sort(blockMilliseconds.begin(), blockMilliseconds.end());
    const auto percentile = [&blockMilliseconds](const double fraction) {
        const auto rank = static_cast<std::size_t>(std::ceil(
            fraction * static_cast<double>(blockMilliseconds.size())
        )) - 1U;
        return blockMilliseconds[rank];
    };

    const auto outputPeak = peak(output);

    // State round trip through the plugin's own format, driven entirely
    // through the bridge's restoreState()/captureState() rather than
    // calling Vst3Host directly.
    std::vector<std::byte> saved;
    const auto saveStarted = std::chrono::steady_clock::now();
    const bool savedOk =
        bridge->captureState(saved) == plugin::PluginStateStatus::ok;
    const double saveMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - saveStarted
        ).count();

    const auto loadStarted = std::chrono::steady_clock::now();
    const bool loadedOk = savedOk
        && bridge->restoreState(saved) == plugin::PluginStateStatus::ok;
    const double loadMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - loadStarted
        ).count();

    // Audio must still flow after the state was pushed back in.
    std::fill(output.begin(), output.end(), 0.0F);
    const bool processedAfterRestore =
        bridge->processBlock(input, output, kFrames)
            == plugin::PluginBlockStatus::processed;
    const auto peakAfterRestore = peak(output);

    std::vector<std::byte> resaved;
    const bool stableState = savedOk
        && bridge->captureState(resaved) == plugin::PluginStateStatus::ok
        && resaved == saved;

    const auto counters = bridge->counters();
    bridge->stop();

    std::cout
        << "Plugin bridge host: module=" << target.filename().string()
        << ", in_bridge_child=1"
        << ", open_ms=" << openMilliseconds
        << ", blocks=" << (warmUpBlocks + measuredBlocks)
        << ", processed=" << processed
        << ", block_p50_ms=" << percentile(0.50)
        << ", block_p95_ms=" << percentile(0.95)
        << ", block_p99_ms=" << percentile(0.99)
        << ", block_max_ms=" << percentile(1.0)
        << ", deadline_misses=" << counters.deadlineMisses
        << ", input_peak=" << peak(input)
        << ", output_peak=" << outputPeak
        << ", samples_changed=" << changedFraction(input, output)
        << ", state_bytes=" << saved.size()
        << ", state_saved=" << (savedOk ? 1 : 0)
        << ", state_restored=" << (loadedOk ? 1 : 0)
        << ", state_stable=" << (stableState ? 1 : 0)
        << ", save_ms=" << saveMilliseconds
        << ", load_ms=" << loadMilliseconds
        << ", audio_after_restore=" << (processedAfterRestore ? 1 : 0)
        << ", peak_after_restore=" << peakAfterRestore
        << "\n";
    return EXIT_SUCCESS;
}
