#include "iramix/plugin/PluginBridge.hpp"

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

[[nodiscard]] iramix::plugin::PluginBridgeConfig makeConfig() {
    return {
        .maximumFrames = kFrames,
        .channelCount = kChannels,
        .deadline = std::chrono::milliseconds {5},
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
        if (status == plugin::PluginBlockStatus::processed) {
            ++processed;
            roundTrip.push_back(elapsed);
        }
    }
    require(
        processed == iterations,
        "every block crosses the process boundary within the deadline"
    );

    // The child halves every sample. Verifying the transform proves the
    // audio really travelled through the other process rather than the
    // destination merely being left untouched.
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
    require(
        counters.processedBlocks >= iterations
            && counters.deadlineMisses == beforeMisses,
        "a warmed bridge misses no deadline"
    );
    bridge->stop();

    std::cout
        << "Plugin bridge healthy: blocks=" << iterations
        << ", frames=" << kFrames
        << ", channels=" << kChannels
        << ", round_trip_p50_ms=" << p50
        << ", round_trip_p95_ms=" << p95
        << ", round_trip_p99_ms=" << p99
        << ", round_trip_max_ms=" << maximum
        << ", deadline_misses=0\n";
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
    std::cout << "All Iramix plugin tests passed.\n";
    return EXIT_SUCCESS;
}
