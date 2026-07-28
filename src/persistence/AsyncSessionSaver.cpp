#include "iramix/persistence/AsyncSessionSaver.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <thread>
#include <utility>
#include <vector>

namespace iramix::persistence {
namespace {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

void copyDetail(
    const std::string& source,
    std::array<char, 192>& destination
) noexcept {
    destination.fill('\0');
    const auto count = std::min(
        source.size(),
        destination.size() - 1U
    );
    std::memcpy(destination.data(), source.data(), count);
}

[[nodiscard]] std::uint64_t elapsedNanoseconds(
    const std::chrono::steady_clock::time_point started
) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started
        ).count()
    );
}

} // namespace

struct AsyncSessionSaver::Impl final {
    struct Slot final {
        std::uint64_t revision {0U};
        ImmutableSessionSnapshot snapshot;
        AtomicSaveFailurePoint failurePoint {
            AtomicSaveFailurePoint::none
        };
        SessionSaveCompletion completion;
    };

    Impl(
        std::filesystem::path projectTarget,
        const std::uint32_t pipelineCapacity
    )
        : target {std::move(projectTarget)},
          capacity {pipelineCapacity},
          slots(pipelineCapacity) {}

    void run() noexcept {
        while (!stopRequested.load(std::memory_order_acquire)
            || processIndex.load(std::memory_order_relaxed)
                < writeIndex.load(std::memory_order_acquire)) {
            const auto process = processIndex.load(
                std::memory_order_relaxed
            );
            if (process
                == writeIndex.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds {1}
                );
                continue;
            }

            auto& slot = slots[static_cast<std::size_t>(
                process % capacity
            )];
            auto snapshot = std::move(slot.snapshot);
            auto& completion = slot.completion;
            completion = {};
            completion.revision = slot.revision;

            std::string error;
            const auto serializationStarted =
                std::chrono::steady_clock::now();
            std::vector<std::byte> payload;
            try {
                payload = serializeSessionDocument(*snapshot, error);
            } catch (const std::exception& exception) {
                error = "session serialization exception: ";
                error += exception.what();
            } catch (...) {
                error = "unknown session serialization exception";
            }
            completion.serializationNanoseconds =
                elapsedNanoseconds(serializationStarted);
            completion.serializedBytes =
                static_cast<std::uint64_t>(payload.size());

            bool committed = false;
            if (!payload.empty()) {
                const auto saveStarted =
                    std::chrono::steady_clock::now();
                try {
                    committed = saveProjectSnapshot(
                        target,
                        payload,
                        error,
                        slot.failurePoint
                    );
                } catch (const std::exception& exception) {
                    error = "session save exception: ";
                    error += exception.what();
                } catch (...) {
                    error = "unknown session save exception";
                }
                completion.durableSaveNanoseconds =
                    elapsedNanoseconds(saveStarted);
            }

            completion.status = committed
                ? ProjectSaveCompletionStatus::committed
                : ProjectSaveCompletionStatus::failed;
            copyDetail(error, completion.detail);
            processIndex.store(process + 1U, std::memory_order_release);
        }
    }

    std::filesystem::path target;
    std::uint64_t capacity {0U};
    std::vector<Slot> slots;
    std::thread thread;
    bool started {false};
    bool finalized {false};
    std::uint64_t lastSubmittedRevision {0U};
    std::atomic<bool> accepting {true};
    std::atomic<bool> stopRequested {false};
    std::atomic<std::uint64_t> writeIndex {0U};
    std::atomic<std::uint64_t> processIndex {0U};
    std::atomic<std::uint64_t> readIndex {0U};
    std::atomic<std::uint64_t> accepted {0U};
    std::atomic<std::uint64_t> rejected {0U};
};

AsyncSessionSaver::AsyncSessionSaver(
    std::unique_ptr<Impl> impl
)
    : impl_ {std::move(impl)} {}

AsyncSessionSaver::~AsyncSessionSaver() {
    stop();
}

std::unique_ptr<AsyncSessionSaver> AsyncSessionSaver::create(
    std::filesystem::path target,
    const std::uint32_t pipelineCapacity,
    std::string& error
) {
    error.clear();
    if (target.empty()) {
        error = "session save target must not be empty";
        return {};
    }
    if (pipelineCapacity == 0U) {
        error = "session save pipeline capacity must be non-zero";
        return {};
    }
    try {
        return std::unique_ptr<AsyncSessionSaver> {
            new AsyncSessionSaver {
                std::make_unique<Impl>(
                    std::move(target),
                    pipelineCapacity
                )
            }
        };
    } catch (const std::bad_alloc&) {
        error = "cannot allocate session save pipeline";
        return {};
    }
}

bool AsyncSessionSaver::start(std::string& error) {
    error.clear();
    if (impl_->started) {
        error = "session save worker already started";
        return false;
    }
    if (impl_->finalized) {
        error = "session save worker is already finalized";
        return false;
    }
    impl_->started = true;
    try {
        impl_->thread = std::thread {[state = impl_.get()] {
            state->run();
        }};
    } catch (const std::system_error& exception) {
        impl_->started = false;
        error = "cannot start session save worker: ";
        error += exception.what();
        return false;
    }
    return true;
}

void AsyncSessionSaver::stop() noexcept {
    if (impl_ == nullptr || impl_->finalized) {
        return;
    }
    impl_->accepting.store(false, std::memory_order_release);
    impl_->stopRequested.store(true, std::memory_order_release);
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    } else if (
        impl_->processIndex.load(std::memory_order_acquire)
        < impl_->writeIndex.load(std::memory_order_acquire)
    ) {
        impl_->run();
    }
    impl_->finalized = true;
}

ProjectSaveSubmitResult AsyncSessionSaver::trySubmit(
    const std::uint64_t revision,
    const ImmutableSessionSnapshot& snapshot,
    const AtomicSaveFailurePoint failurePoint
) noexcept {
    if (!impl_->accepting.load(std::memory_order_acquire)) {
        impl_->rejected.fetch_add(1U, std::memory_order_relaxed);
        return ProjectSaveSubmitResult::stopped;
    }
    if (revision == 0U
        || revision <= impl_->lastSubmittedRevision
        || snapshot == nullptr
        || snapshot->revision != revision) {
        impl_->rejected.fetch_add(1U, std::memory_order_relaxed);
        return ProjectSaveSubmitResult::invalidRevision;
    }

    const auto write = impl_->writeIndex.load(
        std::memory_order_relaxed
    );
    const auto read = impl_->readIndex.load(
        std::memory_order_acquire
    );
    if (write - read >= impl_->capacity) {
        impl_->rejected.fetch_add(1U, std::memory_order_relaxed);
        return ProjectSaveSubmitResult::full;
    }

    auto& slot = impl_->slots[static_cast<std::size_t>(
        write % impl_->capacity
    )];
    slot.revision = revision;
    slot.snapshot = snapshot;
    slot.failurePoint = failurePoint;
    impl_->lastSubmittedRevision = revision;
    impl_->writeIndex.store(write + 1U, std::memory_order_release);
    impl_->accepted.fetch_add(1U, std::memory_order_relaxed);
    return ProjectSaveSubmitResult::accepted;
}

bool AsyncSessionSaver::tryPopCompletion(
    SessionSaveCompletion& completion
) noexcept {
    const auto read = impl_->readIndex.load(
        std::memory_order_relaxed
    );
    const auto process = impl_->processIndex.load(
        std::memory_order_acquire
    );
    if (read == process) {
        return false;
    }
    auto& slot = impl_->slots[static_cast<std::size_t>(
        read % impl_->capacity
    )];
    completion = slot.completion;
    impl_->readIndex.store(read + 1U, std::memory_order_release);
    return true;
}

std::uint64_t AsyncSessionSaver::outstandingCount() const noexcept {
    return impl_->writeIndex.load(std::memory_order_acquire)
        - impl_->readIndex.load(std::memory_order_acquire);
}

std::uint64_t AsyncSessionSaver::pendingSaveCount() const noexcept {
    return impl_->writeIndex.load(std::memory_order_acquire)
        - impl_->processIndex.load(std::memory_order_acquire);
}

std::uint64_t AsyncSessionSaver::completionCount() const noexcept {
    return impl_->processIndex.load(std::memory_order_acquire)
        - impl_->readIndex.load(std::memory_order_acquire);
}

std::uint64_t AsyncSessionSaver::acceptedCount() const noexcept {
    return impl_->accepted.load(std::memory_order_relaxed);
}

std::uint64_t AsyncSessionSaver::rejectedCount() const noexcept {
    return impl_->rejected.load(std::memory_order_relaxed);
}

} // namespace iramix::persistence
