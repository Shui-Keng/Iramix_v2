#pragma once

#include "iramix/persistence/AsyncSessionSaver.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace iramix::persistence {

enum class SessionSaveRequestStatus {
    accepted,
    coalesced,
    alreadyRequested,
    invalidRevision,
    allocationFailure,
    stopped,
};

enum class SessionSaveQueryStatus {
    unknown,
    pending,
    committed,
    failed,
};

struct SessionSaveQuery final {
    SessionSaveQueryStatus status {SessionSaveQueryStatus::unknown};
    std::uint64_t requestedRevision {0U};
    std::uint64_t durableRevision {0U};
    SessionSaveCompletion completion;
};

// Control-thread coordinator for one accepted worker save plus one replaceable
// latest autosave snapshot. Replaced snapshots were never accepted by the
// worker; their edits are covered when the later revision commits.
class SessionSaveCoordinator final {
public:
    ~SessionSaveCoordinator();

    SessionSaveCoordinator(const SessionSaveCoordinator&) = delete;
    SessionSaveCoordinator& operator=(
        const SessionSaveCoordinator&
    ) = delete;

    [[nodiscard]] static std::unique_ptr<SessionSaveCoordinator> create(
        std::filesystem::path target,
        std::string& error,
        std::uint64_t initialDurableRevision = 0U
    );

    [[nodiscard]] bool start(std::string& error);
    void stop() noexcept;
    void pump() noexcept;

    [[nodiscard]] SessionSaveRequestStatus requestSave(
        const ImmutableSessionSnapshot& snapshot,
        AtomicSaveFailurePoint failurePoint =
            AtomicSaveFailurePoint::none
    ) noexcept;

    [[nodiscard]] SessionSaveQuery query(
        std::uint64_t revision
    ) noexcept;

    [[nodiscard]] std::uint64_t requestedRevision() const noexcept;
    [[nodiscard]] std::uint64_t durableRevision() const noexcept;
    [[nodiscard]] std::uint64_t inFlightRevision() const noexcept;
    [[nodiscard]] std::uint64_t pendingRevision() const noexcept;
    [[nodiscard]] std::uint64_t submittedCount() const noexcept;
    [[nodiscard]] std::uint64_t coalescedCount() const noexcept;

private:
    struct PendingSave final {
        ImmutableSessionSnapshot snapshot;
        AtomicSaveFailurePoint failurePoint {
            AtomicSaveFailurePoint::none
        };
    };

    explicit SessionSaveCoordinator(
        std::unique_ptr<AsyncSessionSaver> saver,
        std::uint64_t initialDurableRevision
    );

    void submitPending() noexcept;
    void applyCompletion(
        const SessionSaveCompletion& completion
    ) noexcept;

    std::unique_ptr<AsyncSessionSaver> saver_;
    std::unique_ptr<PendingSave> pending_;
    SessionSaveCompletion latestCompletion_;
    std::uint64_t requestedRevision_ {0U};
    std::uint64_t durableRevision_ {0U};
    std::uint64_t failedRevision_ {0U};
    std::uint64_t inFlightRevision_ {0U};
    std::uint64_t submittedCount_ {0U};
    std::uint64_t coalescedCount_ {0U};
    bool started_ {false};
    bool stopped_ {false};
};

} // namespace iramix::persistence
