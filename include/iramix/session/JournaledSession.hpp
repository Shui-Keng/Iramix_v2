#pragma once

#include "iramix/persistence/AsyncSessionSaver.hpp"
#include "iramix/persistence/CommandJournal.hpp"
#include "iramix/session/SessionController.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace iramix::session {

enum class JournaledEditStatus {
    applied,
    revisionConflict,
    invalidArgument,
    entityNotFound,
    allocationFailure,
    journalFailure,
    nothingToUndo,
    nothingToRedo,
};

struct JournaledEditResult final {
    JournaledEditStatus status {
        JournaledEditStatus::invalidArgument
    };
    std::uint64_t revision {0U};
    std::uint64_t entityId {0U};

    [[nodiscard]] bool applied() const noexcept {
        return status == JournaledEditStatus::applied;
    }
};

// Command schema is independent from project, journal-envelope, and IPC
// schemas. Values are deliberately owned so history survives UI lifetimes.
enum class SessionCommandType : std::uint32_t {
    setTempo = 1U,
    addTrack = 2U,
    renameTrack = 3U,
    removeTrack = 4U,
};

struct SessionCommand final {
    SessionCommandType type {SessionCommandType::setTempo};
    std::uint64_t entityId {0U};
    persistence::SessionTrackType trackType {
        persistence::SessionTrackType::audio
    };
    std::uint32_t color {0U};
    double tempo {0.0};
    std::string name;
};

class JournaledSession final {
public:
    [[nodiscard]] static std::unique_ptr<JournaledSession> open(
        const std::filesystem::path& projectTarget,
        std::string& error
    );

    [[nodiscard]] static std::unique_ptr<JournaledSession>
    createFromDocument(
        persistence::SessionDocument document,
        std::filesystem::path journalPath,
        std::string& error
    );

    [[nodiscard]] static std::filesystem::path journalPathForProject(
        const std::filesystem::path& projectTarget
    );

    JournaledSession(const JournaledSession&) = delete;
    JournaledSession& operator=(const JournaledSession&) = delete;

    [[nodiscard]] std::uint64_t currentRevision() const noexcept;
    [[nodiscard]] std::uint64_t snapshotRevision() const noexcept;
    [[nodiscard]] std::size_t trackCount() const noexcept;
    [[nodiscard]] std::size_t undoDepth() const noexcept;
    [[nodiscard]] std::size_t redoDepth() const noexcept;
    [[nodiscard]] bool requiresReopen() const noexcept;
    [[nodiscard]] std::uint64_t checkpointRevision() const noexcept;

    [[nodiscard]] persistence::ImmutableSessionSnapshot snapshot() const;

    [[nodiscard]] JournaledEditResult setTempo(
        std::uint64_t expectedRevision,
        double tempo,
        std::string& error
    );

    [[nodiscard]] JournaledEditResult addTrack(
        std::uint64_t expectedRevision,
        persistence::SessionTrackType type,
        std::string_view name,
        std::uint32_t color,
        std::string& error
    );

    [[nodiscard]] JournaledEditResult renameTrack(
        std::uint64_t expectedRevision,
        std::uint64_t trackId,
        std::string_view name,
        std::string& error
    );

    [[nodiscard]] JournaledEditResult undo(
        std::uint64_t expectedRevision,
        std::string& error
    );

    [[nodiscard]] JournaledEditResult redo(
        std::uint64_t expectedRevision,
        std::string& error
    );

    // Safe only after a project snapshot at exactly durableRevision has
    // committed. Rewrites history atomically without changing live state.
    [[nodiscard]] bool checkpoint(
        std::uint64_t durableRevision,
        std::string& error
    );

private:
    enum class HistoryAction : std::uint32_t {
        edit = 1U,
        undo = 2U,
        redo = 3U,
    };

    struct HistoryEntry final {
        SessionCommand forward;
        SessionCommand inverse;
    };

    JournaledSession(
        std::unique_ptr<SessionController> controller,
        std::unique_ptr<persistence::CommandJournal> journal,
        std::filesystem::path journalPath,
        std::uint64_t snapshotRevision
    );

    [[nodiscard]] bool replay(
        const std::filesystem::path& journalPath,
        std::string& error
    );

    [[nodiscard]] JournaledEditResult commitEdit(
        std::uint64_t expectedRevision,
        SessionCommand command,
        std::string& error
    );

    [[nodiscard]] JournaledEditResult commitHistoryAction(
        std::uint64_t expectedRevision,
        HistoryAction action,
        std::string& error
    );

    std::unique_ptr<SessionController> controller_;
    std::unique_ptr<persistence::CommandJournal> journal_;
    std::filesystem::path journalPath_;
    std::uint64_t snapshotRevision_ {0U};
    std::vector<HistoryEntry> undo_;
    std::vector<HistoryEntry> redo_;
    std::uint64_t checkpointRevision_ {0U};
    bool requiresReopen_ {false};
};

} // namespace iramix::session
