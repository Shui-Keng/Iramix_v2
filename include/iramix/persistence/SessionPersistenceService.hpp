#pragma once

#include "iramix/persistence/AutosaveClock.hpp"
#include "iramix/persistence/SessionSaveCoordinator.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace iramix::persistence {

enum class AutosaveDirtyStatus {
    tracked,
    replaced,
    alreadyTracked,
    alreadyDurable,
    invalidRevision,
    stopped,
};

// Owns the save coordinator on a background control worker. The first dirty
// revision starts a fixed autosave window; continuous edits replace the
// snapshot without postponing that window indefinitely.
class SessionPersistenceService final {
public:
    ~SessionPersistenceService();

    SessionPersistenceService(const SessionPersistenceService&) = delete;
    SessionPersistenceService& operator=(
        const SessionPersistenceService&
    ) = delete;

    // A null clock uses the steady clock. Pass a ManualAutosaveClock to
    // drive the autosave window deterministically.
    [[nodiscard]] static std::unique_ptr<SessionPersistenceService>
    create(
        std::filesystem::path target,
        std::chrono::milliseconds autosaveInterval,
        std::string& error,
        std::uint64_t initialDurableRevision = 0U,
        std::shared_ptr<AutosaveClock> clock = {}
    );

    [[nodiscard]] bool start(std::string& error);
    void stop() noexcept;

    [[nodiscard]] AutosaveDirtyStatus markDirty(
        const ImmutableSessionSnapshot& snapshot
    ) noexcept;

    [[nodiscard]] SessionSaveRequestStatus requestSave(
        const ImmutableSessionSnapshot& snapshot,
        AtomicSaveFailurePoint failurePoint =
            AtomicSaveFailurePoint::none
    ) noexcept;

    [[nodiscard]] SessionSaveQuery query(
        std::uint64_t revision
    ) noexcept;

    [[nodiscard]] std::uint64_t durableRevision() const noexcept;
    [[nodiscard]] std::uint64_t dirtyRevision() const noexcept;
    [[nodiscard]] std::uint64_t autosaveRequestCount() const noexcept;
    [[nodiscard]] std::uint64_t dirtyReplacementCount() const noexcept;

private:
    SessionPersistenceService(
        std::unique_ptr<SessionSaveCoordinator> coordinator,
        std::chrono::milliseconds autosaveInterval,
        std::shared_ptr<AutosaveClock> clock
    );

    void run() noexcept;
    void reconcileDurableState() noexcept;

    std::unique_ptr<SessionSaveCoordinator> coordinator_;
    std::chrono::milliseconds autosaveInterval_;
    std::shared_ptr<AutosaveClock> clock_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    ImmutableSessionSnapshot dirtySnapshot_;
    std::chrono::steady_clock::time_point autosaveDeadline_ {};
    std::uint64_t autosaveRequestCount_ {0U};
    std::uint64_t dirtyReplacementCount_ {0U};
    bool deadlineArmed_ {false};
    bool started_ {false};
    bool stopRequested_ {false};
    bool stopped_ {false};
};

} // namespace iramix::persistence
