#include "iramix/session/SessionController.hpp"

#include "iramix/persistence/AsyncSessionSaver.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace iramix::session {
namespace {

constexpr std::size_t kMaximumEditNameBytes = 1'024U;

[[nodiscard]] std::uint64_t maximumStableId(
    const persistence::SessionDocument& document
) noexcept {
    std::uint64_t maximum = 0U;
    const auto include = [&maximum](const auto& entities) {
        for (const auto& entity : entities) {
            maximum = std::max(maximum, entity.stableId);
        }
    };
    include(document.tracks);
    include(document.clips);
    include(document.routes);
    include(document.automationLanes);
    include(document.mediaSources);
    include(document.midiSequences);
    include(document.plugins);
    return maximum;
}

} // namespace

SessionController::SessionController() {
    document_.revision = 1U;
    document_.tracks.push_back({
        .stableId = 1U,
        .type = persistence::SessionTrackType::master,
        .gain = 1.0F,
        .color = 0xFF20'3040U,
        .name = "Master",
    });
    nextStableId_ = 2U;
}

SessionController::SessionController(
    persistence::SessionDocument document,
    const std::uint64_t nextStableId
)
    : document_ {std::move(document)},
      nextStableId_ {nextStableId} {}

std::unique_ptr<SessionController> SessionController::fromDocument(
    persistence::SessionDocument document,
    std::string& error
) {
    auto validation = persistence::serializeSessionDocument(
        document,
        error
    );
    if (validation.empty()) {
        return {};
    }
    const auto maximum = maximumStableId(document);
    if (maximum == std::numeric_limits<std::uint64_t>::max()) {
        error = "session stable ID space is exhausted";
        return {};
    }
    try {
        return std::unique_ptr<SessionController> {
            new SessionController {
                std::move(document),
                maximum + 1U,
            }
        };
    } catch (const std::bad_alloc&) {
        error = "cannot allocate editable session";
        return {};
    }
}

std::uint64_t SessionController::currentRevision() const noexcept {
    return document_.revision;
}

std::size_t SessionController::trackCount() const noexcept {
    return document_.tracks.size();
}

persistence::ImmutableSessionSnapshot
SessionController::snapshot() const {
    return std::make_shared<const persistence::SessionDocument>(
        document_
    );
}

SessionEditResult SessionController::conflictOrCurrent(
    const std::uint64_t expectedRevision
) const noexcept {
    return {
        .status = expectedRevision == document_.revision
            ? SessionEditStatus::applied
            : SessionEditStatus::revisionConflict,
        .revision = document_.revision,
        .entityId = 0U,
    };
}

SessionEditResult SessionController::setTempo(
    const std::uint64_t expectedRevision,
    const double tempo
) noexcept {
    const auto revision = conflictOrCurrent(expectedRevision);
    if (!revision.applied()) {
        return revision;
    }
    if (!std::isfinite(tempo) || tempo <= 0.0 || tempo > 1'000.0) {
        return {
            .status = SessionEditStatus::invalidArgument,
            .revision = document_.revision,
        };
    }
    if (document_.revision
        == std::numeric_limits<std::uint64_t>::max()) {
        return {
            .status = SessionEditStatus::invalidArgument,
            .revision = document_.revision,
        };
    }
    document_.tempo = tempo;
    ++document_.revision;
    return {
        .status = SessionEditStatus::applied,
        .revision = document_.revision,
    };
}

SessionEditResult SessionController::addTrack(
    const std::uint64_t expectedRevision,
    const persistence::SessionTrackType type,
    const std::string_view name,
    const std::uint32_t color
) noexcept {
    return addTrackWithStableId(
        expectedRevision,
        nextStableId_,
        type,
        name,
        color
    );
}

SessionEditResult SessionController::addTrackWithStableId(
    const std::uint64_t expectedRevision,
    const std::uint64_t stableId,
    const persistence::SessionTrackType type,
    const std::string_view name,
    const std::uint32_t color
) noexcept {
    const auto revision = conflictOrCurrent(expectedRevision);
    if (!revision.applied()) {
        return revision;
    }
    if (name.empty()
        || name.size() > kMaximumEditNameBytes
        || type < persistence::SessionTrackType::audio
        || type > persistence::SessionTrackType::master
        || stableId == 0U
        || stableId == std::numeric_limits<std::uint64_t>::max()
        || document_.revision
            == std::numeric_limits<std::uint64_t>::max()) {
        return {
            .status = SessionEditStatus::invalidArgument,
            .revision = document_.revision,
        };
    }
    const auto duplicate = std::find_if(
        document_.tracks.begin(),
        document_.tracks.end(),
        [stableId](const persistence::SessionTrack& candidate) {
            return candidate.stableId == stableId;
        }
    );
    if (duplicate != document_.tracks.end()) {
        return {
            .status = SessionEditStatus::invalidArgument,
            .revision = document_.revision,
        };
    }
    try {
        std::string ownedName {name};
        document_.tracks.push_back({
            .stableId = stableId,
            .type = type,
            .gain = 1.0F,
            .color = color,
            .name = std::move(ownedName),
        });
        nextStableId_ = std::max(nextStableId_, stableId + 1U);
        ++document_.revision;
        return {
            .status = SessionEditStatus::applied,
            .revision = document_.revision,
            .entityId = stableId,
        };
    } catch (const std::bad_alloc&) {
        return {
            .status = SessionEditStatus::allocationFailure,
            .revision = document_.revision,
        };
    }
}

SessionEditResult SessionController::renameTrack(
    const std::uint64_t expectedRevision,
    const std::uint64_t trackId,
    const std::string_view name
) noexcept {
    const auto revision = conflictOrCurrent(expectedRevision);
    if (!revision.applied()) {
        return revision;
    }
    if (name.empty()
        || name.size() > kMaximumEditNameBytes
        || document_.revision
            == std::numeric_limits<std::uint64_t>::max()) {
        return {
            .status = SessionEditStatus::invalidArgument,
            .revision = document_.revision,
        };
    }
    const auto track = std::find_if(
        document_.tracks.begin(),
        document_.tracks.end(),
        [trackId](const persistence::SessionTrack& candidate) {
            return candidate.stableId == trackId;
        }
    );
    if (track == document_.tracks.end()) {
        return {
            .status = SessionEditStatus::entityNotFound,
            .revision = document_.revision,
        };
    }
    try {
        std::string ownedName {name};
        track->name = std::move(ownedName);
        ++document_.revision;
        return {
            .status = SessionEditStatus::applied,
            .revision = document_.revision,
            .entityId = trackId,
        };
    } catch (const std::bad_alloc&) {
        return {
            .status = SessionEditStatus::allocationFailure,
            .revision = document_.revision,
        };
    }
}

SessionEditResult SessionController::removeTrack(
    const std::uint64_t expectedRevision,
    const std::uint64_t trackId
) noexcept {
    const auto revision = conflictOrCurrent(expectedRevision);
    if (!revision.applied()) {
        return revision;
    }
    if (document_.revision
        == std::numeric_limits<std::uint64_t>::max()) {
        return {
            .status = SessionEditStatus::invalidArgument,
            .revision = document_.revision,
        };
    }
    const auto track = std::find_if(
        document_.tracks.begin(),
        document_.tracks.end(),
        [trackId](const persistence::SessionTrack& candidate) {
            return candidate.stableId == trackId;
        }
    );
    if (track == document_.tracks.end()) {
        return {
            .status = SessionEditStatus::entityNotFound,
            .revision = document_.revision,
        };
    }
    const auto clipReferencesTrack = std::any_of(
        document_.clips.begin(),
        document_.clips.end(),
        [trackId](const persistence::SessionClip& clip) {
            return clip.trackId == trackId;
        }
    );
    const auto routeReferencesTrack = std::any_of(
        document_.routes.begin(),
        document_.routes.end(),
        [trackId](const persistence::SessionRoute& route) {
            return route.sourceTrackId == trackId
                || route.destinationTrackId == trackId;
        }
    );
    const auto automationReferencesTrack = std::any_of(
        document_.automationLanes.begin(),
        document_.automationLanes.end(),
        [trackId](
            const persistence::SessionAutomationLane& lane
        ) {
            return lane.targetTrackId == trackId;
        }
    );
    const auto pluginReferencesTrack = std::any_of(
        document_.plugins.begin(),
        document_.plugins.end(),
        [trackId](const persistence::SessionPlugin& plugin) {
            return plugin.targetTrackId == trackId;
        }
    );
    if (clipReferencesTrack
        || routeReferencesTrack
        || automationReferencesTrack
        || pluginReferencesTrack) {
        return {
            .status = SessionEditStatus::invalidArgument,
            .revision = document_.revision,
        };
    }
    document_.tracks.erase(track);
    ++document_.revision;
    return {
        .status = SessionEditStatus::applied,
        .revision = document_.revision,
        .entityId = trackId,
    };
}

} // namespace iramix::session
