#include "iramix/persistence/SessionPersistenceService.hpp"

#include <algorithm>
#include <chrono>
#include <new>
#include <optional>
#include <system_error>
#include <utility>

namespace iramix::persistence {

SessionPersistenceService::SessionPersistenceService(
    std::unique_ptr<SessionSaveCoordinator> coordinator,
    const std::chrono::milliseconds autosaveInterval,
    std::shared_ptr<AutosaveClock> clock
)
    : coordinator_ {std::move(coordinator)},
      autosaveInterval_ {autosaveInterval},
      clock_ {std::move(clock)} {}

SessionPersistenceService::~SessionPersistenceService() {
    stop();
}

std::unique_ptr<SessionPersistenceService>
SessionPersistenceService::create(
    std::filesystem::path target,
    const std::chrono::milliseconds autosaveInterval,
    std::string& error,
    const std::uint64_t initialDurableRevision,
    std::shared_ptr<AutosaveClock> clock
) {
    error.clear();
    if (autosaveInterval <= std::chrono::milliseconds::zero()) {
        error = "autosave interval must be positive";
        return {};
    }
    auto coordinator = SessionSaveCoordinator::create(
        std::move(target),
        error,
        initialDurableRevision
    );
    if (coordinator == nullptr) {
        return {};
    }
    try {
        if (clock == nullptr) {
            clock = makeSteadyAutosaveClock();
        }
        return std::unique_ptr<SessionPersistenceService> {
            new SessionPersistenceService {
                std::move(coordinator),
                autosaveInterval,
                std::move(clock),
            }
        };
    } catch (const std::bad_alloc&) {
        error = "cannot allocate session persistence service";
        return {};
    }
}

bool SessionPersistenceService::start(std::string& error) {
    std::scoped_lock lock {mutex_};
    if (stopped_ || stopRequested_) {
        error = "session persistence service is stopped";
        return false;
    }
    if (started_) {
        error = "session persistence service already started";
        return false;
    }
    if (!coordinator_->start(error)) {
        return false;
    }
    try {
        // A clock that cannot advance on its own must be able to wake the
        // worker, or a virtual deadline would never be observed.
        clock_->setObserver([this] {
            wake_.notify_one();
        });
        worker_ = std::thread {
            [this] {
                run();
            }
        };
        started_ = true;
        return true;
    } catch (const std::system_error& exception) {
        error = "cannot start session persistence worker: ";
        error += exception.what();
        coordinator_->stop();
        stopped_ = true;
        return false;
    }
}

void SessionPersistenceService::stop() noexcept {
    {
        std::scoped_lock lock {mutex_};
        if (stopped_) {
            return;
        }
        if (!started_) {
            coordinator_->stop();
            stopped_ = true;
            return;
        }
        stopRequested_ = true;
    }
    wake_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
    // The observer captures this, and the clock may outlive the service
    // when a caller holds its own handle. Drop it once the worker is gone.
    clock_->setObserver(nullptr);
}

AutosaveDirtyStatus SessionPersistenceService::markDirty(
    const ImmutableSessionSnapshot& snapshot
) noexcept {
    if (snapshot == nullptr || snapshot->revision == 0U) {
        return AutosaveDirtyStatus::invalidRevision;
    }
    std::scoped_lock lock {mutex_};
    if (stopped_ || stopRequested_) {
        return AutosaveDirtyStatus::stopped;
    }
    coordinator_->pump();
    reconcileDurableState();
    if (snapshot->revision <= coordinator_->durableRevision()) {
        return AutosaveDirtyStatus::alreadyDurable;
    }
    if (dirtySnapshot_ != nullptr
        && snapshot->revision <= dirtySnapshot_->revision) {
        return AutosaveDirtyStatus::alreadyTracked;
    }

    const bool replaces = dirtySnapshot_ != nullptr;
    dirtySnapshot_ = snapshot;
    if (replaces) {
        ++dirtyReplacementCount_;
    }
    if (!deadlineArmed_) {
        autosaveDeadline_ = clock_->now() + autosaveInterval_;
        deadlineArmed_ = true;
    }
    wake_.notify_one();
    return replaces
        ? AutosaveDirtyStatus::replaced
        : AutosaveDirtyStatus::tracked;
}

SessionSaveRequestStatus SessionPersistenceService::requestSave(
    const ImmutableSessionSnapshot& snapshot,
    const AtomicSaveFailurePoint failurePoint
) noexcept {
    std::scoped_lock lock {mutex_};
    if (stopped_ || stopRequested_) {
        return SessionSaveRequestStatus::stopped;
    }
    const auto result =
        coordinator_->requestSave(snapshot, failurePoint);
    if (result == SessionSaveRequestStatus::accepted
        || result == SessionSaveRequestStatus::coalesced
        || result == SessionSaveRequestStatus::alreadyRequested) {
        if (dirtySnapshot_ != nullptr) {
            autosaveDeadline_ = clock_->now() + autosaveInterval_;
            deadlineArmed_ = true;
        }
        wake_.notify_one();
    }
    return result;
}

SessionSaveQuery SessionPersistenceService::query(
    const std::uint64_t revision
) noexcept {
    std::scoped_lock lock {mutex_};
    auto result = coordinator_->query(revision);
    reconcileDurableState();
    return result;
}

std::uint64_t
SessionPersistenceService::durableRevision() const noexcept {
    std::scoped_lock lock {mutex_};
    return coordinator_->durableRevision();
}

std::uint64_t
SessionPersistenceService::dirtyRevision() const noexcept {
    std::scoped_lock lock {mutex_};
    return dirtySnapshot_ == nullptr
        ? 0U
        : dirtySnapshot_->revision;
}

std::uint64_t
SessionPersistenceService::autosaveRequestCount() const noexcept {
    std::scoped_lock lock {mutex_};
    return autosaveRequestCount_;
}

std::uint64_t
SessionPersistenceService::dirtyReplacementCount() const noexcept {
    std::scoped_lock lock {mutex_};
    return dirtyReplacementCount_;
}

void SessionPersistenceService::run() noexcept {
    std::unique_lock lock {mutex_};
    while (!stopRequested_) {
        coordinator_->pump();
        reconcileDurableState();
        const auto now = clock_->now();
        if (dirtySnapshot_ != nullptr
            && deadlineArmed_
            && now >= autosaveDeadline_) {
            const auto result =
                coordinator_->requestSave(dirtySnapshot_);
            if (result == SessionSaveRequestStatus::accepted
                || result == SessionSaveRequestStatus::coalesced) {
                ++autosaveRequestCount_;
            }
            if (result != SessionSaveRequestStatus::allocationFailure
                && result != SessionSaveRequestStatus::stopped) {
                autosaveDeadline_ = now + autosaveInterval_;
            }
        }

        // Deadline waiting is in the clock's domain; coordinator polling
        // is not. The save worker performs real disk I/O on another
        // thread, so its completion is still awaited in real time even
        // when virtual time is driving the autosave window.
        std::optional<std::chrono::steady_clock::time_point> nextWake;
        if (deadlineArmed_) {
            nextWake = clock_->wakeTimeFor(autosaveDeadline_);
        }
        if (coordinator_->inFlightRevision() != 0U
            || coordinator_->pendingRevision() != 0U) {
            const auto poll = std::chrono::steady_clock::now()
                + std::chrono::milliseconds {1};
            nextWake = nextWake.has_value()
                ? std::min(*nextWake, poll)
                : poll;
        }
        if (nextWake.has_value()) {
            wake_.wait_until(lock, *nextWake);
        } else {
            wake_.wait(lock);
        }
    }

    coordinator_->pump();
    reconcileDurableState();
    if (dirtySnapshot_ != nullptr
        && dirtySnapshot_->revision
            > coordinator_->durableRevision()) {
        static_cast<void>(
            coordinator_->requestSave(dirtySnapshot_)
        );
    }
    coordinator_->stop();
    reconcileDurableState();
    stopped_ = true;
}

void SessionPersistenceService::reconcileDurableState() noexcept {
    if (dirtySnapshot_ != nullptr
        && coordinator_->durableRevision()
            >= dirtySnapshot_->revision) {
        dirtySnapshot_.reset();
        deadlineArmed_ = false;
    }
}

} // namespace iramix::persistence
