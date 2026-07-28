#include "iramix/persistence/AsyncProjectSaver.hpp"

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

} // namespace

struct AsyncProjectSaver::Impl final {
    struct Slot final {
        std::uint64_t revision {0U};
        ImmutableProjectPayload payload;
        AtomicSaveFailurePoint failurePoint {
            AtomicSaveFailurePoint::none
        };
        ProjectSaveCompletion completion;
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
            auto payload = std::move(slot.payload);
            std::string saveError;
            bool committed = false;
            try {
                committed = saveProjectSnapshot(
                    target,
                    *payload,
                    saveError,
                    slot.failurePoint
                );
            } catch (const std::exception& exception) {
                saveError = "project save exception: ";
                saveError += exception.what();
            } catch (...) {
                saveError = "unknown project save exception";
            }

            slot.completion.revision = slot.revision;
            slot.completion.status = committed
                ? ProjectSaveCompletionStatus::committed
                : ProjectSaveCompletionStatus::failed;
            copyDetail(saveError, slot.completion.detail);
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

AsyncProjectSaver::AsyncProjectSaver(
    std::unique_ptr<Impl> impl
)
    : impl_ {std::move(impl)} {}

AsyncProjectSaver::~AsyncProjectSaver() {
    stop();
}

std::unique_ptr<AsyncProjectSaver> AsyncProjectSaver::create(
    std::filesystem::path target,
    const std::uint32_t pipelineCapacity,
    std::string& error
) {
    error.clear();
    if (target.empty()) {
        error = "project save target must not be empty";
        return {};
    }
    if (pipelineCapacity == 0U) {
        error = "project save pipeline capacity must be non-zero";
        return {};
    }
    try {
        return std::unique_ptr<AsyncProjectSaver> {
            new AsyncProjectSaver {
                std::make_unique<Impl>(
                    std::move(target),
                    pipelineCapacity
                )
            }
        };
    } catch (const std::bad_alloc&) {
        error = "cannot allocate project save pipeline";
        return {};
    }
}

bool AsyncProjectSaver::start(std::string& error) {
    error.clear();
    if (impl_->started) {
        error = "project save worker already started";
        return false;
    }
    if (impl_->finalized) {
        error = "project save worker is already finalized";
        return false;
    }
    impl_->started = true;
    try {
        impl_->thread = std::thread {[state = impl_.get()] {
            state->run();
        }};
    } catch (const std::system_error& exception) {
        impl_->started = false;
        error = "cannot start project save worker: ";
        error += exception.what();
        return false;
    }
    return true;
}

void AsyncProjectSaver::stop() noexcept {
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
        // Accepted setup-time requests still receive completions even if
        // teardown happens before start(). stop() is a control-thread call.
        impl_->run();
    }
    impl_->finalized = true;
}

ProjectSaveSubmitResult AsyncProjectSaver::trySubmit(
    const std::uint64_t revision,
    const ImmutableProjectPayload& payload,
    const AtomicSaveFailurePoint failurePoint
) noexcept {
    if (!impl_->accepting.load(std::memory_order_acquire)) {
        impl_->rejected.fetch_add(1U, std::memory_order_relaxed);
        return ProjectSaveSubmitResult::stopped;
    }
    if (revision == 0U
        || revision <= impl_->lastSubmittedRevision
        || payload == nullptr) {
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
    slot.payload = payload;
    slot.failurePoint = failurePoint;
    impl_->lastSubmittedRevision = revision;
    impl_->writeIndex.store(write + 1U, std::memory_order_release);
    impl_->accepted.fetch_add(1U, std::memory_order_relaxed);
    return ProjectSaveSubmitResult::accepted;
}

bool AsyncProjectSaver::tryPopCompletion(
    ProjectSaveCompletion& completion
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

std::uint64_t AsyncProjectSaver::outstandingCount() const noexcept {
    return impl_->writeIndex.load(std::memory_order_acquire)
        - impl_->readIndex.load(std::memory_order_acquire);
}

std::uint64_t AsyncProjectSaver::pendingSaveCount() const noexcept {
    return impl_->writeIndex.load(std::memory_order_acquire)
        - impl_->processIndex.load(std::memory_order_acquire);
}

std::uint64_t AsyncProjectSaver::completionCount() const noexcept {
    return impl_->processIndex.load(std::memory_order_acquire)
        - impl_->readIndex.load(std::memory_order_acquire);
}

std::uint64_t AsyncProjectSaver::acceptedCount() const noexcept {
    return impl_->accepted.load(std::memory_order_relaxed);
}

std::uint64_t AsyncProjectSaver::rejectedCount() const noexcept {
    return impl_->rejected.load(std::memory_order_relaxed);
}

} // namespace iramix::persistence
