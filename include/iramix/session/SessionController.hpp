#pragma once

#include "iramix/persistence/AsyncSessionSaver.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace iramix::session {

enum class SessionEditStatus {
    applied,
    revisionConflict,
    invalidArgument,
    entityNotFound,
    allocationFailure,
};

struct SessionEditResult final {
    SessionEditStatus status {SessionEditStatus::invalidArgument};
    std::uint64_t revision {0U};
    std::uint64_t entityId {0U};

    [[nodiscard]] bool applied() const noexcept {
        return status == SessionEditStatus::applied;
    }
};

// Control-thread-owned editable session. Runtime audio graphs are compiled
// separately; persistence receives explicit immutable DTO snapshots.
class SessionController final {
public:
    SessionController();

    SessionController(const SessionController&) = delete;
    SessionController& operator=(const SessionController&) = delete;

    [[nodiscard]] static std::unique_ptr<SessionController> fromDocument(
        persistence::SessionDocument document,
        std::string& error
    );

    [[nodiscard]] std::uint64_t currentRevision() const noexcept;
    [[nodiscard]] std::size_t trackCount() const noexcept;

    [[nodiscard]] persistence::ImmutableSessionSnapshot snapshot() const;

    [[nodiscard]] SessionEditResult setTempo(
        std::uint64_t expectedRevision,
        double tempo
    ) noexcept;

    [[nodiscard]] SessionEditResult addTrack(
        std::uint64_t expectedRevision,
        persistence::SessionTrackType type,
        std::string_view name,
        std::uint32_t color
    ) noexcept;

    [[nodiscard]] SessionEditResult addTrackWithStableId(
        std::uint64_t expectedRevision,
        std::uint64_t stableId,
        persistence::SessionTrackType type,
        std::string_view name,
        std::uint32_t color
    ) noexcept;

    [[nodiscard]] SessionEditResult renameTrack(
        std::uint64_t expectedRevision,
        std::uint64_t trackId,
        std::string_view name
    ) noexcept;

    [[nodiscard]] SessionEditResult removeTrack(
        std::uint64_t expectedRevision,
        std::uint64_t trackId
    ) noexcept;

private:
    explicit SessionController(
        persistence::SessionDocument document,
        std::uint64_t nextStableId
    );

    [[nodiscard]] SessionEditResult conflictOrCurrent(
        std::uint64_t expectedRevision
    ) const noexcept;

    persistence::SessionDocument document_;
    std::uint64_t nextStableId_ {1U};
};

} // namespace iramix::session
