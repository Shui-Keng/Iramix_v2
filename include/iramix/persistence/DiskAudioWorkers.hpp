#pragma once

#include "iramix/persistence/RecoverableRecording.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace iramix::persistence {

struct RecordingDiskWorkerConfig final {
    RecordingFormat format;
    std::uint32_t maximumFramesPerBlock {0U};
    std::uint32_t queueBlockCapacity {0U};
    std::uint32_t durableFlushEveryBlocks {1U};
};

// Owns a preallocated SPSC queue. tryEnqueue() is the sole audio-callback
// entry point: it only copies into an available slot and publishes atomics.
class RecordingDiskWorker final {
public:
    ~RecordingDiskWorker();

    RecordingDiskWorker(const RecordingDiskWorker&) = delete;
    RecordingDiskWorker& operator=(const RecordingDiskWorker&) = delete;

    [[nodiscard]] static std::unique_ptr<RecordingDiskWorker> create(
        const std::filesystem::path& path,
        RecordingDiskWorkerConfig config,
        std::string& error
    );

    // start() and stop() are control-thread operations.
    [[nodiscard]] bool start(std::string& error);
    void stop() noexcept;

    [[nodiscard]] bool tryEnqueue(
        std::span<const float> interleavedSamples,
        std::uint32_t frameCount
    ) noexcept;

    [[nodiscard]] std::uint64_t acceptedBlocks() const noexcept;
    [[nodiscard]] std::uint64_t rejectedBlocks() const noexcept;
    [[nodiscard]] std::uint64_t writtenBlocks() const noexcept;
    [[nodiscard]] std::uint64_t bufferedBlocks() const noexcept;
    [[nodiscard]] std::size_t queueStorageBytes() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] std::string lastError() const;

private:
    struct Impl;
    explicit RecordingDiskWorker(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

struct RecordingReadAheadConfig final {
    std::uint32_t maximumFramesPerBlock {0U};
    std::uint32_t queueBlockCapacity {0U};
};

// The worker owns all file I/O. tryDequeue() is callback-safe and emits
// silence on underflow or invalid destination shape.
class RecordingReadAhead final {
public:
    ~RecordingReadAhead();

    RecordingReadAhead(const RecordingReadAhead&) = delete;
    RecordingReadAhead& operator=(const RecordingReadAhead&) = delete;

    [[nodiscard]] static std::unique_ptr<RecordingReadAhead> create(
        const std::filesystem::path& path,
        RecordingReadAheadConfig config,
        std::string& error
    );

    [[nodiscard]] bool start(std::string& error);
    void stop() noexcept;

    [[nodiscard]] bool tryDequeue(
        std::span<float> interleavedDestination,
        std::uint32_t& frameCount
    ) noexcept;

    [[nodiscard]] RecordingFormat format() const noexcept;
    [[nodiscard]] std::uint64_t deliveredBlocks() const noexcept;
    [[nodiscard]] std::uint64_t underflowCount() const noexcept;
    [[nodiscard]] std::uint64_t bufferedBlocks() const noexcept;
    [[nodiscard]] std::size_t queueStorageBytes() const noexcept;
    [[nodiscard]] bool reachedEnd() const noexcept;
    [[nodiscard]] bool discardedInvalidTail() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] std::string lastError() const;

private:
    struct Impl;
    explicit RecordingReadAhead(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace iramix::persistence
