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
#include <vector>

namespace {

constexpr std::uint32_t kFrames = 256U;
constexpr std::uint32_t kChannels = 2U;
constexpr double kSampleRate = 48'000.0;

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

} // namespace

int main(const int argc, char* argv[]) {
    namespace plugin = iramix::plugin;

    if (!plugin::Vst3Host::available()) {
        std::cout
            << "Plugin host: available=0, "
               "reason=built_without_vst3_sdk\n";
        return EXIT_SUCCESS;
    }
    if (argc < 2) {
        std::cerr
            << "usage: iramix_plugin_host <module-or-bundle> "
               "[class-index]\n";
        return EXIT_FAILURE;
    }

    std::filesystem::path target {argv[1]};
    const auto classIndex = argc >= 3
        ? static_cast<std::uint32_t>(std::atoi(argv[2]))
        : 0U;

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

    plugin::Vst3Host host;
    std::string error;
    const auto openStarted = std::chrono::steady_clock::now();
    if (!host.open(
            target,
            classIndex,
            kFrames,
            kChannels,
            kSampleRate,
            error
        )) {
        std::cout
            << "Plugin host: opened=0, reason=" << error << "\n";
        return EXIT_FAILURE;
    }
    const double openMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - openStarted
        ).count();

    const auto input = makeInput();
    std::vector<float> output(input.size(), 0.0F);

    // Steady-state processing. The first blocks are excluded from the
    // timing because a plugin may still be building tables.
    constexpr std::size_t warmUpBlocks = 32U;
    constexpr std::size_t measuredBlocks = 500U;
    std::size_t processed = 0U;
    for (std::size_t index = 0U; index < warmUpBlocks; ++index) {
        if (host.process(input.data(), output.data(), kFrames)) {
            ++processed;
        }
    }

    std::vector<double> blockMilliseconds;
    blockMilliseconds.reserve(measuredBlocks);
    for (std::size_t index = 0U; index < measuredBlocks; ++index) {
        const auto started = std::chrono::steady_clock::now();
        const bool ok = host.process(input.data(), output.data(), kFrames);
        blockMilliseconds.push_back(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started
            ).count()
        );
        if (ok) {
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

    // State round trip through the plugin's own format.
    std::vector<std::byte> saved;
    const auto saveStarted = std::chrono::steady_clock::now();
    const bool savedOk = host.saveState(saved);
    const double saveMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - saveStarted
        ).count();

    const auto loadStarted = std::chrono::steady_clock::now();
    const bool loadedOk = savedOk && host.loadState(saved);
    const double loadMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - loadStarted
        ).count();

    // Audio must still flow after the state was pushed back in.
    std::fill(output.begin(), output.end(), 0.0F);
    const bool processedAfterRestore =
        host.process(input.data(), output.data(), kFrames);
    const auto peakAfterRestore = peak(output);

    std::vector<std::byte> resaved;
    const bool stableState = savedOk
        && host.saveState(resaved)
        && resaved == saved;

    const auto& info = host.info();
    std::cout
        << "Plugin host: name=" << info.name
        << ", in_channels=" << info.inputChannels
        << ", out_channels=" << info.outputChannels
        << ", open_ms=" << openMilliseconds
        << ", blocks=" << (warmUpBlocks + measuredBlocks)
        << ", processed=" << processed
        << ", block_p50_ms=" << percentile(0.50)
        << ", block_p95_ms=" << percentile(0.95)
        << ", block_p99_ms=" << percentile(0.99)
        << ", block_max_ms=" << percentile(1.0)
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
