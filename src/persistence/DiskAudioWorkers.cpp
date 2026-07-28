#include "iramix/persistence/DiskAudioWorkers.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

namespace iramix::persistence {
namespace {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

class SpscBlockRing final {
public:
    SpscBlockRing(
        const RecordingFormat format,
        const std::uint32_t maximumFrames,
        const std::uint32_t capacity
    )
        : format_ {format},
          maximumFrames_ {maximumFrames},
          capacity_ {capacity},
          slotSamples_ {
              static_cast<std::size_t>(maximumFrames)
              * format.channelCount
          },
          samples_(
              slotSamples_ * static_cast<std::size_t>(capacity),
              0.0F
          ),
          frameCounts_(capacity, 0U) {}

    [[nodiscard]] bool tryPush(
        const std::span<const float> samples,
        const std::uint32_t frames
    ) noexcept {
        const std::size_t expectedSamples =
            static_cast<std::size_t>(frames) * format_.channelCount;
        if (frames == 0U
            || frames > maximumFrames_
            || samples.size() != expectedSamples) {
            return false;
        }
        const auto write = writeIndex_.load(
            std::memory_order_relaxed
        );
        const auto read = readIndex_.load(
            std::memory_order_acquire
        );
        if (write - read >= capacity_) {
            return false;
        }
        const std::size_t slot = static_cast<std::size_t>(
            write % capacity_
        );
        auto* const destination =
            samples_.data() + slot * slotSamples_;
        std::copy(samples.begin(), samples.end(), destination);
        frameCounts_[slot] = frames;
        writeIndex_.store(write + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool tryPop(
        const std::span<float> destination,
        std::uint32_t& frames
    ) noexcept {
        const auto read = readIndex_.load(
            std::memory_order_relaxed
        );
        const auto write = writeIndex_.load(
            std::memory_order_acquire
        );
        if (read == write) {
            return false;
        }
        const std::size_t slot = static_cast<std::size_t>(
            read % capacity_
        );
        const auto availableFrames = frameCounts_[slot];
        const std::size_t sampleCount =
            static_cast<std::size_t>(availableFrames)
            * format_.channelCount;
        if (destination.size() < sampleCount) {
            return false;
        }
        const auto* const source =
            samples_.data() + slot * slotSamples_;
        std::copy_n(source, sampleCount, destination.data());
        frames = availableFrames;
        readIndex_.store(read + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::uint64_t bufferedBlocks() const noexcept {
        const auto write = writeIndex_.load(
            std::memory_order_acquire
        );
        const auto read = readIndex_.load(
            std::memory_order_acquire
        );
        return write - read;
    }

    [[nodiscard]] std::size_t storageBytes() const noexcept {
        return samples_.size() * sizeof(float)
            + frameCounts_.size() * sizeof(std::uint32_t);
    }

private:
    RecordingFormat format_;
    std::uint32_t maximumFrames_ {0U};
    std::uint64_t capacity_ {0U};
    std::size_t slotSamples_ {0U};
    std::vector<float> samples_;
    std::vector<std::uint32_t> frameCounts_;
    std::atomic<std::uint64_t> writeIndex_ {0U};
    std::array<
        std::byte,
        64U - sizeof(std::atomic<std::uint64_t>)
    > producerConsumerSeparation_ {};
    std::atomic<std::uint64_t> readIndex_ {0U};
};

[[nodiscard]] bool validQueueShape(
    const RecordingFormat format,
    const std::uint32_t maximumFrames,
    const std::uint32_t capacity,
    std::string& error
) {
    if (format.sampleRate == 0U
        || format.channelCount == 0U
        || maximumFrames == 0U
        || capacity == 0U) {
        error = "disk audio queue dimensions must be non-zero";
        return false;
    }
    const std::size_t maximumSamples =
        std::numeric_limits<std::size_t>::max() / sizeof(float);
    if (format.channelCount
            > maximumSamples / maximumFrames
        || static_cast<std::size_t>(format.channelCount)
                * maximumFrames
            > maximumSamples / capacity) {
        error = "disk audio queue exceeds addressable memory";
        return false;
    }
    return true;
}

} // namespace

struct RecordingDiskWorker::Impl final {
    Impl(
        RecordingDiskWorkerConfig workerConfig,
        std::unique_ptr<RecoverableRecordingWriter> recordingWriter
    )
        : config {workerConfig},
          ring {
              workerConfig.format,
              workerConfig.maximumFramesPerBlock,
              workerConfig.queueBlockCapacity
          },
          workBuffer(
              static_cast<std::size_t>(
                  workerConfig.maximumFramesPerBlock
              ) * workerConfig.format.channelCount,
              0.0F
          ),
          writer {std::move(recordingWriter)} {}

    void run() noexcept {
        std::uint32_t blocksSinceFlush = 0U;
        while (!stopRequested.load(std::memory_order_acquire)
            || ring.bufferedBlocks() > 0U) {
            std::uint32_t frames = 0U;
            if (!ring.tryPop(workBuffer, frames)) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds {1}
                );
                continue;
            }
            const auto sampleCount =
                static_cast<std::size_t>(frames)
                * config.format.channelCount;
            if (!writer->appendInterleavedBlock(
                    std::span<const float> {workBuffer}.first(
                        sampleCount
                    ),
                    frames,
                    workerError
                )) {
                workerFailed.store(true, std::memory_order_release);
                accepting.store(false, std::memory_order_release);
                return;
            }
            written.fetch_add(1U, std::memory_order_relaxed);
            ++blocksSinceFlush;
            if (blocksSinceFlush
                >= config.durableFlushEveryBlocks) {
                if (!writer->flush(workerError)) {
                    workerFailed.store(
                        true,
                        std::memory_order_release
                    );
                    accepting.store(
                        false,
                        std::memory_order_release
                    );
                    return;
                }
                blocksSinceFlush = 0U;
            }
        }
        if (blocksSinceFlush > 0U
            && !writer->flush(workerError)) {
            workerFailed.store(true, std::memory_order_release);
        }
    }

    RecordingDiskWorkerConfig config;
    SpscBlockRing ring;
    std::vector<float> workBuffer;
    std::unique_ptr<RecoverableRecordingWriter> writer;
    std::thread thread;
    bool started {false};
    std::atomic<bool> accepting {true};
    std::atomic<bool> stopRequested {false};
    std::atomic<bool> workerFailed {false};
    std::atomic<std::uint64_t> accepted {0U};
    std::atomic<std::uint64_t> rejected {0U};
    std::atomic<std::uint64_t> written {0U};
    std::string workerError;
};

RecordingDiskWorker::RecordingDiskWorker(
    std::unique_ptr<Impl> impl
)
    : impl_ {std::move(impl)} {}

RecordingDiskWorker::~RecordingDiskWorker() {
    stop();
}

std::unique_ptr<RecordingDiskWorker>
RecordingDiskWorker::create(
    const std::filesystem::path& path,
    const RecordingDiskWorkerConfig config,
    std::string& error
) {
    error.clear();
    if (!validQueueShape(
        config.format,
        config.maximumFramesPerBlock,
        config.queueBlockCapacity,
        error
    )) {
        return {};
    }
    if (config.durableFlushEveryBlocks == 0U) {
        error = "recording flush cadence must be non-zero";
        return {};
    }
    auto writer = RecoverableRecordingWriter::create(
        path,
        config.format,
        error
    );
    if (writer == nullptr) {
        return {};
    }
    try {
        auto impl = std::make_unique<Impl>(
            config,
            std::move(writer)
        );
        return std::unique_ptr<RecordingDiskWorker> {
            new RecordingDiskWorker {std::move(impl)}
        };
    } catch (const std::bad_alloc&) {
        error = "cannot allocate recording disk queue";
        return {};
    }
}

bool RecordingDiskWorker::start(std::string& error) {
    error.clear();
    if (impl_->started) {
        error = "recording disk worker already started";
        return false;
    }
    if (impl_->writer == nullptr
        || impl_->stopRequested.load(std::memory_order_acquire)) {
        error = "recording disk worker is already finalized";
        return false;
    }
    impl_->started = true;
    try {
        impl_->thread = std::thread {[state = impl_.get()] {
            state->run();
        }};
    } catch (const std::system_error& exception) {
        impl_->started = false;
        error = "cannot start recording disk worker: ";
        error += exception.what();
        return false;
    }
    return true;
}

void RecordingDiskWorker::stop() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    impl_->accepting.store(false, std::memory_order_release);
    impl_->stopRequested.store(true, std::memory_order_release);
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    impl_->writer.reset();
}

bool RecordingDiskWorker::tryEnqueue(
    const std::span<const float> interleavedSamples,
    const std::uint32_t frameCount
) noexcept {
    if (!impl_->accepting.load(std::memory_order_acquire)
        || !impl_->ring.tryPush(interleavedSamples, frameCount)) {
        impl_->rejected.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    impl_->accepted.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

std::uint64_t RecordingDiskWorker::acceptedBlocks() const noexcept {
    return impl_->accepted.load(std::memory_order_relaxed);
}

std::uint64_t RecordingDiskWorker::rejectedBlocks() const noexcept {
    return impl_->rejected.load(std::memory_order_relaxed);
}

std::uint64_t RecordingDiskWorker::writtenBlocks() const noexcept {
    return impl_->written.load(std::memory_order_relaxed);
}

std::uint64_t RecordingDiskWorker::bufferedBlocks() const noexcept {
    return impl_->ring.bufferedBlocks();
}

std::size_t RecordingDiskWorker::queueStorageBytes() const noexcept {
    return impl_->ring.storageBytes();
}

bool RecordingDiskWorker::failed() const noexcept {
    return impl_->workerFailed.load(std::memory_order_acquire);
}

std::string RecordingDiskWorker::lastError() const {
    return impl_->workerError;
}

struct RecordingReadAhead::Impl final {
    Impl(
        const RecordingScanResult recordingScan,
        const RecordingReadAheadConfig workerConfig,
        std::unique_ptr<RecoverableRecordingReader> recordingReader
    )
        : scan {recordingScan},
          config {workerConfig},
          ring {
              recordingScan.format,
              workerConfig.maximumFramesPerBlock,
              workerConfig.queueBlockCapacity
          },
          workBuffer(
              static_cast<std::size_t>(
                  workerConfig.maximumFramesPerBlock
              ) * recordingScan.format.channelCount,
              0.0F
          ),
          reader {std::move(recordingReader)} {}

    void run() noexcept {
        while (!stopRequested.load(std::memory_order_acquire)) {
            if (ring.bufferedBlocks() >= config.queueBlockCapacity) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds {1}
                );
                continue;
            }
            std::uint32_t frames = 0U;
            const auto status = reader->readNextBlock(
                workBuffer,
                frames,
                workerError
            );
            if (status == RecordingBlockReadStatus::block) {
                const auto sampleCount =
                    static_cast<std::size_t>(frames)
                    * scan.format.channelCount;
                if (!ring.tryPush(
                    std::span<const float> {workBuffer}.first(
                        sampleCount
                    ),
                    frames
                )) {
                    workerError =
                        "read-ahead queue publication failed";
                    workerFailed.store(
                        true,
                        std::memory_order_release
                    );
                    return;
                }
                continue;
            }
            if (status == RecordingBlockReadStatus::cleanEnd
                || status
                    == RecordingBlockReadStatus::invalidTail) {
                endReached.store(true, std::memory_order_release);
                return;
            }
            if (workerError.empty()) {
                workerError = "read-ahead recording read failed";
            }
            workerFailed.store(true, std::memory_order_release);
            return;
        }
    }

    RecordingScanResult scan;
    RecordingReadAheadConfig config;
    SpscBlockRing ring;
    std::vector<float> workBuffer;
    std::unique_ptr<RecoverableRecordingReader> reader;
    std::thread thread;
    bool started {false};
    std::atomic<bool> stopRequested {false};
    std::atomic<bool> endReached {false};
    std::atomic<bool> workerFailed {false};
    std::atomic<std::uint64_t> delivered {0U};
    std::atomic<std::uint64_t> underflows {0U};
    std::string workerError;
};

RecordingReadAhead::RecordingReadAhead(
    std::unique_ptr<Impl> impl
)
    : impl_ {std::move(impl)} {}

RecordingReadAhead::~RecordingReadAhead() {
    stop();
}

std::unique_ptr<RecordingReadAhead> RecordingReadAhead::create(
    const std::filesystem::path& path,
    const RecordingReadAheadConfig config,
    std::string& error
) {
    error.clear();
    const auto scan = scanRecording(path);
    if (!scan.ok()) {
        error = scan.error;
        return {};
    }
    if (!validQueueShape(
        scan.format,
        config.maximumFramesPerBlock,
        config.queueBlockCapacity,
        error
    )) {
        return {};
    }
    if (config.maximumFramesPerBlock
        < scan.maximumFramesPerBlock) {
        error = "read-ahead slot is smaller than recording blocks";
        return {};
    }
    auto reader = RecoverableRecordingReader::create(path, error);
    if (reader == nullptr) {
        return {};
    }
    try {
        auto impl = std::make_unique<Impl>(
            scan,
            config,
            std::move(reader)
        );
        return std::unique_ptr<RecordingReadAhead> {
            new RecordingReadAhead {std::move(impl)}
        };
    } catch (const std::bad_alloc&) {
        error = "cannot allocate read-ahead queue";
        return {};
    }
}

bool RecordingReadAhead::start(std::string& error) {
    error.clear();
    if (impl_->started) {
        error = "recording read-ahead already started";
        return false;
    }
    if (impl_->stopRequested.load(std::memory_order_acquire)) {
        error = "recording read-ahead is already stopped";
        return false;
    }
    impl_->started = true;
    try {
        impl_->thread = std::thread {[state = impl_.get()] {
            state->run();
        }};
    } catch (const std::system_error& exception) {
        impl_->started = false;
        error = "cannot start recording read-ahead: ";
        error += exception.what();
        return false;
    }
    return true;
}

void RecordingReadAhead::stop() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    impl_->stopRequested.store(true, std::memory_order_release);
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

bool RecordingReadAhead::tryDequeue(
    const std::span<float> interleavedDestination,
    std::uint32_t& frameCount
) noexcept {
    std::fill(
        interleavedDestination.begin(),
        interleavedDestination.end(),
        0.0F
    );
    frameCount = static_cast<std::uint32_t>(
        std::min<std::size_t>(
            impl_->config.maximumFramesPerBlock,
            interleavedDestination.size()
                / impl_->scan.format.channelCount
        )
    );
    std::uint32_t queuedFrames = 0U;
    if (!impl_->ring.tryPop(
        interleavedDestination,
        queuedFrames
    )) {
        impl_->underflows.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    frameCount = queuedFrames;
    impl_->delivered.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

RecordingFormat RecordingReadAhead::format() const noexcept {
    return impl_->scan.format;
}

std::uint64_t RecordingReadAhead::deliveredBlocks() const noexcept {
    return impl_->delivered.load(std::memory_order_relaxed);
}

std::uint64_t RecordingReadAhead::underflowCount() const noexcept {
    return impl_->underflows.load(std::memory_order_relaxed);
}

std::uint64_t RecordingReadAhead::bufferedBlocks() const noexcept {
    return impl_->ring.bufferedBlocks();
}

std::size_t RecordingReadAhead::queueStorageBytes() const noexcept {
    return impl_->ring.storageBytes();
}

bool RecordingReadAhead::reachedEnd() const noexcept {
    return impl_->endReached.load(std::memory_order_acquire);
}

bool RecordingReadAhead::discardedInvalidTail() const noexcept {
    return impl_->scan.discardedInvalidTail;
}

bool RecordingReadAhead::failed() const noexcept {
    return impl_->workerFailed.load(std::memory_order_acquire);
}

std::string RecordingReadAhead::lastError() const {
    return impl_->workerError;
}

} // namespace iramix::persistence
