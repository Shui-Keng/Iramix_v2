#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace iramix::plugin {

struct PluginBridgeConfig final {
    std::uint32_t maximumFrames {0U};
    std::uint32_t channelCount {0U};
    // Hard upper bound on how long the audio callback will wait for the
    // plugin process. Exceeding it is a normal, counted outcome, never a
    // reason to keep waiting.
    std::chrono::microseconds deadline {0};
};

enum class PluginBlockStatus : std::uint32_t {
    // The plugin process returned audio within the deadline.
    processed = 1U,
    // It did not. The destination holds silence and the block is counted.
    deadlineMissed = 2U,
    // The process is known to be gone. The destination holds silence.
    processExited = 3U,
    // The bridge was never started, or has been stopped.
    notRunning = 4U,
    // The caller passed a block the bridge cannot describe.
    invalidBlock = 5U,
};

struct PluginBridgeCounters final {
    std::uint64_t processedBlocks {0U};
    std::uint64_t deadlineMisses {0U};
    std::uint64_t exitedBlocks {0U};
    std::uint64_t consecutiveDeadlineMisses {0U};
};

// Runs plugin audio in a separate process over shared memory.
//
// The design property being validated is negative: nothing the child does —
// crashing, hanging, or returning late — may block the audio callback or
// take the host down with it. processBlock() therefore has a hard deadline
// and degrades to silence rather than waiting.
class PluginBridge final {
public:
    ~PluginBridge();

    PluginBridge(const PluginBridge&) = delete;
    PluginBridge& operator=(const PluginBridge&) = delete;

    [[nodiscard]] static std::unique_ptr<PluginBridge> create(
        PluginBridgeConfig config,
        std::string& error
    );

    // Launches the child. `arguments` are appended after the shared-memory
    // name, so a caller chooses which behaviour the child adopts.
    [[nodiscard]] bool start(
        const std::filesystem::path& childExecutable,
        const std::string& childMode,
        std::string& error
    );

    void stop() noexcept;

    // Audio-callback entry point. Performs no allocation, acquires no lock,
    // and makes no filesystem or logging call. Bounded by config.deadline.
    [[nodiscard]] PluginBlockStatus processBlock(
        std::span<const float> interleavedInput,
        std::span<float> interleavedOutput,
        std::uint32_t frameCount
    ) noexcept;

    [[nodiscard]] PluginBridgeCounters counters() const noexcept;
    [[nodiscard]] bool childRunning() const noexcept;
    [[nodiscard]] std::string sharedMemoryName() const;

    // Child-process entry point. Attaches to the named shared memory and
    // services blocks until asked to stop.
    [[nodiscard]] static int runChild(
        const std::string& sharedMemoryName,
        const std::string& mode
    );

private:
    struct Impl;
    explicit PluginBridge(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace iramix::plugin
