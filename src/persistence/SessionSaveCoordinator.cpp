#include "iramix/persistence/SessionSaveCoordinator.hpp"

#include <chrono>
#include <new>
#include <thread>
#include <utility>

namespace iramix::persistence {

SessionSaveCoordinator::SessionSaveCoordinator(
    std::unique_ptr<AsyncSessionSaver> saver
)
    : saver_ {std::move(saver)} {}

SessionSaveCoordinator::~SessionSaveCoordinator() {
    stop();
}

std::unique_ptr<SessionSaveCoordinator>
SessionSaveCoordinator::create(
    std::filesystem::path target,
    std::string& error
) {
    auto saver = AsyncSessionSaver::create(
        std::move(target),
        1U,
        error
    );
    if (saver == nullptr) {
        return {};
    }
    try {
        return std::unique_ptr<SessionSaveCoordinator> {
            new SessionSaveCoordinator {std::move(saver)}
        };
    } catch (const std::bad_alloc&) {
        error = "cannot allocate session save coordinator";
        return {};
    }
}

bool SessionSaveCoordinator::start(std::string& error) {
    if (stopped_) {
        error = "session save coordinator is stopped";
        return false;
    }
    if (started_) {
        error = "session save coordinator already started";
        return false;
    }
    if (!saver_->start(error)) {
        return false;
    }
    started_ = true;
    return true;
}

void SessionSaveCoordinator::stop() noexcept {
    if (stopped_) {
        return;
    }
    if (!started_) {
        std::string ignored;
        started_ = saver_->start(ignored);
    }

    while (pending_ != nullptr && inFlightRevision_ != 0U) {
        pump();
        if (pending_ != nullptr && inFlightRevision_ != 0U) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds {1}
            );
        }
    }
    submitPending();
    saver_->stop();

    SessionSaveCompletion completion;
    while (saver_->tryPopCompletion(completion)) {
        applyCompletion(completion);
    }
    stopped_ = true;
}

void SessionSaveCoordinator::pump() noexcept {
    if (stopped_) {
        return;
    }
    SessionSaveCompletion completion;
    while (saver_->tryPopCompletion(completion)) {
        applyCompletion(completion);
    }
    submitPending();
}

SessionSaveRequestStatus SessionSaveCoordinator::requestSave(
    const ImmutableSessionSnapshot& snapshot,
    const AtomicSaveFailurePoint failurePoint
) noexcept {
    if (stopped_) {
        return SessionSaveRequestStatus::stopped;
    }
    if (snapshot == nullptr || snapshot->revision == 0U) {
        return SessionSaveRequestStatus::invalidRevision;
    }

    pump();
    const auto revision = snapshot->revision;
    if (revision < requestedRevision_) {
        return SessionSaveRequestStatus::invalidRevision;
    }
    if (revision == requestedRevision_) {
        return SessionSaveRequestStatus::alreadyRequested;
    }

    try {
        auto replacement = std::make_unique<PendingSave>();
        replacement->snapshot = snapshot;
        replacement->failurePoint = failurePoint;
        const bool replacesPending = pending_ != nullptr;
        const bool waitsForAccepted = inFlightRevision_ != 0U;
        pending_ = std::move(replacement);
        requestedRevision_ = revision;
        if (replacesPending || waitsForAccepted) {
            ++coalescedCount_;
        }
        submitPending();
        return replacesPending || waitsForAccepted
            ? SessionSaveRequestStatus::coalesced
            : SessionSaveRequestStatus::accepted;
    } catch (const std::bad_alloc&) {
        return SessionSaveRequestStatus::allocationFailure;
    }
}

SessionSaveQuery SessionSaveCoordinator::query(
    const std::uint64_t revision
) noexcept {
    pump();
    SessionSaveQuery result;
    result.requestedRevision = requestedRevision_;
    result.durableRevision = durableRevision_;
    if (revision == 0U || revision > requestedRevision_) {
        return result;
    }
    if (durableRevision_ >= revision) {
        result.status = SessionSaveQueryStatus::committed;
        if (latestCompletion_.status
                == ProjectSaveCompletionStatus::committed
            && latestCompletion_.revision == durableRevision_) {
            result.completion = latestCompletion_;
        }
        return result;
    }
    if (failedRevision_ == revision) {
        result.status = SessionSaveQueryStatus::failed;
        result.completion = latestCompletion_;
        return result;
    }
    result.status = SessionSaveQueryStatus::pending;
    return result;
}

std::uint64_t SessionSaveCoordinator::requestedRevision() const noexcept {
    return requestedRevision_;
}

std::uint64_t SessionSaveCoordinator::durableRevision() const noexcept {
    return durableRevision_;
}

std::uint64_t SessionSaveCoordinator::inFlightRevision() const noexcept {
    return inFlightRevision_;
}

std::uint64_t SessionSaveCoordinator::pendingRevision() const noexcept {
    return pending_ == nullptr ? 0U : pending_->snapshot->revision;
}

std::uint64_t SessionSaveCoordinator::submittedCount() const noexcept {
    return submittedCount_;
}

std::uint64_t SessionSaveCoordinator::coalescedCount() const noexcept {
    return coalescedCount_;
}

void SessionSaveCoordinator::submitPending() noexcept {
    if (pending_ == nullptr
        || inFlightRevision_ != 0U
        || stopped_) {
        return;
    }
    const auto revision = pending_->snapshot->revision;
    const auto result = saver_->trySubmit(
        revision,
        pending_->snapshot,
        pending_->failurePoint
    );
    if (result == ProjectSaveSubmitResult::accepted) {
        inFlightRevision_ = revision;
        pending_.reset();
        ++submittedCount_;
    }
}

void SessionSaveCoordinator::applyCompletion(
    const SessionSaveCompletion& completion
) noexcept {
    latestCompletion_ = completion;
    inFlightRevision_ = 0U;
    if (completion.status == ProjectSaveCompletionStatus::committed) {
        durableRevision_ = completion.revision;
        failedRevision_ = 0U;
    } else {
        failedRevision_ = completion.revision;
    }
}

} // namespace iramix::persistence
