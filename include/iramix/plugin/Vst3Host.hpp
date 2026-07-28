#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace iramix::plugin {

struct Vst3HostInfo final {
    std::string name;
    std::uint32_t inputChannels {0U};
    std::uint32_t outputChannels {0U};
    std::uint32_t parameterCount {0U};
    bool acceptsFloat32 {false};
};

// Hosts one real VST3 plugin instance.
//
// Intended to run inside the plugin child process, never in the host: the
// entire point of the bridge is that third-party code executes somewhere a
// crash cannot reach the audio callback. Nothing here is safe to call from
// the host's real-time thread.
class Vst3Host final {
public:
    Vst3Host();
    ~Vst3Host();

    Vst3Host(const Vst3Host&) = delete;
    Vst3Host& operator=(const Vst3Host&) = delete;

    // Loads the module, instantiates the classIndex-th audio class, and
    // brings it to a processing-ready state. Every failure reports why
    // rather than leaving a half-initialised plugin behind.
    [[nodiscard]] bool open(
        const std::filesystem::path& modulePath,
        std::uint32_t classIndex,
        std::uint32_t maximumFrames,
        std::uint32_t channelCount,
        double sampleRate,
        std::string& error
    );

    void close() noexcept;

    // Runs one block through the plugin. Allocation-free: every buffer it
    // needs was sized by open(). Returns false if the plugin refused the
    // block, in which case the destination is left untouched and the
    // caller decides what silence to emit.
    [[nodiscard]] bool process(
        const float* interleavedInput,
        float* interleavedOutput,
        std::uint32_t frameCount
    ) noexcept;

    // The plugin's own opaque state, in the format it defines. This is
    // what a session stores.
    [[nodiscard]] bool saveState(std::vector<std::byte>& state);
    [[nodiscard]] bool loadState(std::span<const std::byte> state);

    [[nodiscard]] bool opened() const noexcept;
    [[nodiscard]] const Vst3HostInfo& info() const noexcept;

    // False when the project was built without IRAMIX_VST3_SDK_PATH, in
    // which case open() always fails and says so.
    [[nodiscard]] static bool available() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iramix::plugin
