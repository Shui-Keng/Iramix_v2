#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace iramix::plugin {

struct PluginBridgeConfig final {
    std::uint32_t maximumFrames {0U};
    std::uint32_t channelCount {0U};
    // Hard upper bound on how long the audio callback will wait for the
    // plugin process. Exceeding it is a normal, counted outcome, never a
    // reason to keep waiting.
    std::chrono::microseconds deadline {0};
    // Capacity reserved for one plugin state blob in the shared region.
    // Zero disables state transfer entirely rather than growing on demand:
    // the region is sized once, before any audio flows.
    std::uint32_t maximumStateBytes {0U};
    // Bound for restore and capture, which run on the control thread and so
    // may wait far longer than a block. Still bounded: a plugin that will
    // not answer must not stall a save or a project load.
    std::chrono::milliseconds stateDeadline {250};
    // Depth of the control-to-plugin parameter queue, rounded up to a power
    // of two. Zero disables parameter transport. Bounded on purpose: a
    // queue that grows is a queue that allocates, and saturation must be a
    // counted outcome rather than an unbounded backlog.
    std::uint32_t parameterQueueCapacity {0U};
    // Depth of the control-to-plugin MIDI queue, rounded up to a power of
    // two. Zero disables MIDI transport. Same bounded-queue reasoning as
    // parameterQueueCapacity; a separate capacity because a note burst and
    // an automation burst saturate independently and neither should be
    // able to starve the other.
    std::uint32_t midiQueueCapacity {0U};
};

// Parameters the stand-in plugin exposes. A real plugin publishes its own
// list; these exist so the transport can be validated against observable
// DSP behaviour.
enum class PluginParameterId : std::uint32_t {
    // Applied to every sample, and carried in the plugin's state blob.
    gain = 1U,
    // Non-zero passes audio through untouched. Deliberately *not* in the
    // state blob: the session schema keeps bypass as a host-side field on
    // SessionPlugin, so the plugin must not claim ownership of it.
    bypass = 2U,
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

enum class PluginStateStatus : std::uint32_t {
    // The plugin accepted the blob, or produced one.
    ok = 1U,
    // The plugin examined the blob and refused it. Its previous state is
    // still in force — a rejected restore never leaves a half-loaded
    // plugin.
    rejectedByPlugin = 2U,
    // The blob does not fit the capacity the region was sized for.
    tooLarge = 3U,
    // The plugin did not answer within stateDeadline.
    timedOut = 4U,
    // The bridge is not started, has been stopped, or was configured
    // without a state region.
    unavailable = 5U,
};

struct PluginBridgeCounters final {
    std::uint64_t processedBlocks {0U};
    std::uint64_t deadlineMisses {0U};
    std::uint64_t exitedBlocks {0U};
    std::uint64_t consecutiveDeadlineMisses {0U};
    std::uint64_t stateRestores {0U};
    std::uint64_t stateCaptures {0U};
    std::uint64_t stateRejections {0U};
    std::uint64_t stateTimeouts {0U};
    std::uint64_t parametersSent {0U};
    // The queue was full. The change was refused at the call site rather
    // than dropped silently somewhere downstream.
    std::uint64_t parameterOverflows {0U};
    // The event was refused because its timestamp went backwards, which
    // would make the rendered result depend on delivery order.
    std::uint64_t parameterOutOfOrder {0U};
    // Reported by the plugin process, not the host.
    std::uint64_t parametersApplied {0U};
    // Applied, but only after the block they were scheduled for had
    // already been rendered.
    std::uint64_t parametersLate {0U};
    std::uint64_t midiEventsSent {0U};
    std::uint64_t midiOverflows {0U};
    std::uint64_t midiOutOfOrder {0U};
    // Reported by the plugin process, not the host.
    std::uint64_t midiEventsApplied {0U};
    std::uint64_t midiEventsLate {0U};
};

enum class PluginParameterStatus : std::uint32_t {
    accepted = 1U,
    // The queue is full. The caller decides what to do; the bridge does
    // not silently drop the change.
    queueFull = 2U,
    // The timestamp is earlier than one already queued.
    outOfOrder = 3U,
    // Not started, stopped, or configured without a parameter queue.
    unavailable = 4U,
};

enum class PluginMidiStatus : std::uint32_t {
    accepted = 1U,
    // The queue is full. The caller decides what to do; the bridge does
    // not silently drop the note.
    queueFull = 2U,
    // The timestamp is earlier than one already queued.
    outOfOrder = 3U,
    // Not started, stopped, or configured without a MIDI queue.
    unavailable = 4U,
};

// Enough of a "vst3" child's real IEditController parameter list for the
// host to address one parameter by its own ID through setParameterById(),
// without a general per-parameter enumeration protocol across the
// process boundary. Both fields are zero for the stand-in, and for a
// real plugin with no IEditController or no parameters at all.
struct PluginParameterMetadata final {
    std::uint32_t count {0U};
    std::uint32_t firstParameterId {0U};
    float firstParameterDefault {0.0F};
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

    // Launches the child in real-plugin mode: the child loads
    // vst3ModulePath itself and every block runs through that plugin's own
    // DSP rather than the stand-in. Failure to open the plugin surfaces as
    // an ordinary exited child — processBlock() degrades to silence exactly
    // as it does for a crash, because from the host's side the two look
    // identical.
    [[nodiscard]] bool startVst3(
        const std::filesystem::path& childExecutable,
        const std::filesystem::path& vst3ModulePath,
        std::uint32_t vst3ClassIndex,
        double sampleRate,
        std::string& error
    );

    // Real-plugin mode with fault injection: after three real blocks have
    // gone through the actual plugin's own DSP, the child crashes
    // (hang == false) or hangs (hang == true). Exists to prove the
    // bridge's crash/hang recovery contract when the process that fails
    // was, until the moment it does, genuinely running third-party plugin
    // code rather than only the stand-in.
    [[nodiscard]] bool startVst3Fault(
        const std::filesystem::path& childExecutable,
        const std::filesystem::path& vst3ModulePath,
        std::uint32_t vst3ClassIndex,
        double sampleRate,
        bool hang,
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

    // Queues one parameter change for the plugin, to take effect at
    // `sampleTime`. Lock-free and allocation-free, so it is safe from the
    // control thread while audio is running; timestamps must not go
    // backwards, because a rendered result that depends on delivery order
    // is not reproducible.
    [[nodiscard]] PluginParameterStatus setParameter(
        PluginParameterId parameter,
        float value,
        std::uint64_t sampleTime
    ) noexcept;

    // Same transport as setParameter(), addressed by a plugin's own raw
    // parameter ID rather than the stand-in's PluginParameterId enum.
    // What a real plugin's IEditController parameters are reached
    // through — PluginParameterId::gain has no meaning for a real
    // plugin, but PluginParameterId::bypass is still delivered as bypass
    // regardless of which overload sends it, since the bridge dispatches
    // on the wire ID, not on which call produced it.
    [[nodiscard]] PluginParameterStatus setParameterById(
        std::uint32_t parameterId,
        float value,
        std::uint64_t sampleTime
    ) noexcept;

    // Queues a MIDI note for a real plugin, delivered to the child's
    // Vst3Host on its own event bus at the given sampleTime — never
    // written into the plugin's state, and never applied by the
    // stand-in, which has no MIDI concept. Same bounded, lock-free,
    // ordered-delivery contract as setParameter(): a queue that grows is
    // a queue that allocates, and out-of-order timestamps would make the
    // rendered result depend on delivery order rather than the timeline.
    [[nodiscard]] PluginMidiStatus sendMidiNoteOn(
        std::int16_t pitch,
        float velocity,
        std::uint64_t sampleTime
    ) noexcept;

    [[nodiscard]] PluginMidiStatus sendMidiNoteOff(
        std::int16_t pitch,
        float velocity,
        std::uint64_t sampleTime
    ) noexcept;

    // Sample position of the next block. The bridge advances this by
    // frameCount on every processBlock() call, degraded ones included —
    // transport does not stop because a plugin missed its deadline. A real
    // integration would take this from the transport instead.
    [[nodiscard]] std::uint64_t samplePosition() const noexcept;

    // Hands a session's stored state blob to the live plugin. Control
    // thread only: it allocates nothing but it does wait, so calling it
    // from the audio callback would defeat the whole bridge. The plugin
    // applies the blob between blocks, never during one, so audio is never
    // rendered against half-restored state.
    [[nodiscard]] PluginStateStatus restoreState(
        std::span<const std::byte> state
    );

    // Reads the live plugin's current state back for saving. Control thread
    // only, and bounded by stateDeadline so a stuck plugin delays a save
    // instead of blocking it forever (R-12).
    [[nodiscard]] PluginStateStatus captureState(
        std::vector<std::byte>& state
    );

    [[nodiscard]] PluginBridgeCounters counters() const noexcept;
    [[nodiscard]] bool childRunning() const noexcept;
    [[nodiscard]] std::string sharedMemoryName() const;

    // Populated by a "vst3" child before it signals ready; {0, 0, 0.0}
    // before the child is ready, or for the stand-in.
    [[nodiscard]] PluginParameterMetadata parameterMetadata() const noexcept;

    // Child-process entry point. Attaches to the named shared memory and
    // services blocks until asked to stop. mode == "vst3" loads
    // vst3ModulePath itself (see startVst3()); the other three arguments
    // are otherwise unused.
    [[nodiscard]] static int runChild(
        const std::string& sharedMemoryName,
        const std::string& mode,
        const std::string& vst3ModulePath = std::string {},
        std::uint32_t vst3ClassIndex = 0U,
        double vst3SampleRate = 48'000.0
    );

private:
    struct Impl;
    explicit PluginBridge(std::unique_ptr<Impl> impl);
    [[nodiscard]] bool startWithArguments(
        const std::filesystem::path& childExecutable,
        const std::vector<std::string>& arguments,
        std::string& error
    );
    [[nodiscard]] PluginMidiStatus enqueueMidiEvent(
        std::uint8_t type,
        std::int16_t pitch,
        float velocity,
        std::uint64_t sampleTime
    ) noexcept;
    std::unique_ptr<Impl> impl_;
};

// State format of the stand-in plugin the child hosts. A real plugin's blob
// is opaque to the host and stays that way; this one is documented and
// inspectable purely so a test can prove a restored value actually reached
// the DSP rather than merely being copied across the boundary.
namespace stub {

[[nodiscard]] std::vector<std::byte> encodeState(
    float gain,
    std::span<const std::byte> payload
);

// Returns false when the blob fails its magic or checksum check — the same
// decision the child makes before applying it.
[[nodiscard]] bool decodeStateGain(
    std::span<const std::byte> state,
    float& gain
) noexcept;

} // namespace stub

} // namespace iramix::plugin
