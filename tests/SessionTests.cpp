#include "iramix/persistence/ProjectStore.hpp"
#include "iramix/persistence/ProjectBackupStore.hpp"
#include "iramix/persistence/SessionDocument.hpp"
#include "iramix/persistence/SessionPersistenceService.hpp"
#include "iramix/persistence/SessionSaveCoordinator.hpp"
#include "iramix/session/JournaledSession.hpp"
#include "iramix/session/SessionController.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
        path_ = std::filesystem::temp_directory_path()
            / ("iramix-session-" + std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] double percentileMilliseconds(
    std::vector<double> values,
    const double percentile
) {
    require(!values.empty(), "percentile input is non-empty");
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<std::size_t>(std::ceil(
        percentile * static_cast<double>(values.size())
    )) - 1U;
    return values[rank];
}

[[nodiscard]] iramix::persistence::SessionDocument
makeReferenceSession() {
    constexpr std::uint64_t trackBase = 10'000U;
    constexpr std::uint64_t clipBase = 100'000U;
    constexpr std::uint64_t routeBase = 200'000U;
    constexpr std::uint64_t automationBase = 300'000U;
    constexpr std::uint64_t sourceBase = 1'000'000U;
    constexpr std::size_t trackCount = 200U;
    constexpr std::size_t clipCount = 2'000U;
    constexpr std::size_t automationLaneCount = 40U;
    constexpr std::size_t pointsPerLane = 1'000U;

    iramix::persistence::SessionDocument document;
    document.revision = 1U;
    document.tempo = 128.0;
    document.tracks.reserve(trackCount);
    for (std::size_t index = 0U; index < trackCount; ++index) {
        document.tracks.push_back({
            .stableId = trackBase + index,
            .type = index + 1U == trackCount
                ? iramix::persistence::SessionTrackType::master
                : iramix::persistence::SessionTrackType::audio,
            .gain = 1.0F,
            .color = 0xFF20'3040U
                + static_cast<std::uint32_t>(index),
            .name = "Track " + std::to_string(index),
        });
    }
    document.mediaSources.reserve(clipCount);
    for (std::size_t index = 0U; index < clipCount; ++index) {
        document.mediaSources.push_back({
            .stableId = sourceBase + index,
            .frameCount = 96'000U,
            .sampleRate = 48'000U,
            .channelCount = 2U,
            .path = "media/source-" + std::to_string(index) + ".wav",
            .name = "Source " + std::to_string(index),
        });
    }
    document.clips.reserve(clipCount);
    for (std::size_t index = 0U; index < clipCount; ++index) {
        document.clips.push_back({
            .stableId = clipBase + index,
            .trackId = trackBase + index % (trackCount - 1U),
            .sourceId = sourceBase + index,
            .startFrame = static_cast<std::uint64_t>(index)
                * 12'000U,
            .lengthFrames = 24'000U,
            .sourceOffsetFrames = 0U,
            .gain = 1.0F,
            .muted = false,
            .name = "Clip " + std::to_string(index),
        });
    }
    const auto masterId = trackBase + trackCount - 1U;
    document.routes.reserve(trackCount - 1U);
    for (std::size_t index = 0U; index + 1U < trackCount; ++index) {
        document.routes.push_back({
            .stableId = routeBase + index,
            .sourceTrackId = trackBase + index,
            .destinationTrackId = masterId,
            .gain = 1.0F,
            .enabled = true,
        });
    }
    document.automationLanes.reserve(automationLaneCount);
    for (
        std::size_t laneIndex = 0U;
        laneIndex < automationLaneCount;
        ++laneIndex
    ) {
        iramix::persistence::SessionAutomationLane lane;
        lane.stableId = automationBase + laneIndex;
        lane.targetTrackId = trackBase + laneIndex;
        lane.parameter =
            iramix::persistence::SessionParameterId::gain;
        lane.points.reserve(pointsPerLane);
        for (
            std::size_t pointIndex = 0U;
            pointIndex < pointsPerLane;
            ++pointIndex
        ) {
            lane.points.push_back({
                .samplePosition =
                    static_cast<std::uint64_t>(pointIndex) * 256U,
                .value = static_cast<float>(pointIndex % 101U)
                    / 100.0F,
            });
        }
        document.automationLanes.push_back(std::move(lane));
    }
    return document;
}

void testSessionController() {
    iramix::session::SessionController controller;
    require(
        controller.currentRevision() == 1U
            && controller.trackCount() == 1U,
        "default session owns revision one and a master track"
    );

    const auto original = controller.snapshot();
    const auto tempo = controller.setTempo(1U, 132.0);
    require(
        tempo.applied() && tempo.revision == 2U,
        "tempo edit advances the expected revision"
    );
    require(
        original->revision == 1U && original->tempo == 120.0,
        "published session snapshot remains immutable after edits"
    );

    const auto conflict = controller.setTempo(1U, 140.0);
    require(
        conflict.status
                == iramix::session::SessionEditStatus::revisionConflict
            && conflict.revision == 2U,
        "stale edit is rejected with current revision"
    );
    const auto invalid = controller.setTempo(2U, -1.0);
    require(
        invalid.status
                == iramix::session::SessionEditStatus::invalidArgument
            && controller.currentRevision() == 2U,
        "invalid edit does not advance revision"
    );

    const auto added = controller.addTrack(
        2U,
        iramix::persistence::SessionTrackType::instrument,
        "Production instrument",
        0xFF44'6688U
    );
    require(
        added.applied()
            && added.revision == 3U
            && added.entityId == 2U,
        "track edit allocates a stable ID and advances revision"
    );
    const auto revisionThree = controller.snapshot();
    const auto renamed = controller.renameTrack(
        3U,
        added.entityId,
        "Renamed instrument"
    );
    require(
        renamed.applied() && renamed.revision == 4U,
        "rename applies to the production session"
    );
    const auto revisionFour = controller.snapshot();
    require(
        revisionThree->tracks[1].name == "Production instrument"
            && revisionFour->tracks[1].name == "Renamed instrument",
        "old and new immutable snapshots retain their own state"
    );

    std::string error;
    auto reloaded =
        iramix::session::SessionController::fromDocument(
            *revisionFour,
            error
        );
    require(reloaded != nullptr, error.c_str());
    const auto another = reloaded->addTrack(
        4U,
        iramix::persistence::SessionTrackType::audio,
        "Next ID",
        0U
    );
    require(
        another.applied() && another.entityId == 3U,
        "loaded session resumes stable-ID allocation"
    );

    std::cout
        << "Session controller: initial_revision=1, final_revision=4, "
           "revision_conflicts=1, invalid_edits=1, stable_ids=3, "
           "immutable_snapshots=1\n";
}

void testJournaledSession(const std::filesystem::path& root) {
    const auto project = root / "journaled-session.irpx";
    std::string error;
    auto session =
        iramix::session::JournaledSession::open(project, error);
    require(session != nullptr, error.c_str());

    const auto tempo = session->setTempo(1U, 132.0, error);
    require(
        tempo.applied() && tempo.revision == 2U,
        "journaled tempo edit reaches durable revision two"
    );
    const auto stale = session->setTempo(1U, 140.0, error);
    require(
        stale.status
            == iramix::session::
                JournaledEditStatus::revisionConflict,
        "stale journaled edit is rejected before append"
    );
    const auto added = session->addTrack(
        2U,
        iramix::persistence::SessionTrackType::instrument,
        "Production instrument",
        0xFF44'6688U,
        error
    );
    require(
        added.applied()
            && added.revision == 3U
            && added.entityId == 2U,
        "journaled add allocates a stable track ID"
    );

    const auto revisionThree = session->snapshot();
    const auto payload =
        iramix::persistence::serializeSessionDocument(
            *revisionThree,
            error
        );
    require(!payload.empty(), error.c_str());
    require(
        iramix::persistence::saveProjectSnapshot(
            project,
            payload,
            error
        ),
        error.c_str()
    );

    const auto renamed = session->renameTrack(
        3U,
        added.entityId,
        "Renamed instrument",
        error
    );
    require(
        renamed.applied() && renamed.revision == 4U,
        "journaled rename reaches revision four"
    );
    const auto undoRename = session->undo(4U, error);
    require(
        undoRename.applied()
            && undoRename.revision == 5U
            && session->snapshot()->tracks[1].name
                == "Production instrument",
        "undo appends revision five and restores the prior name"
    );

    session.reset();
    session = iramix::session::JournaledSession::open(
        project,
        error
    );
    require(session != nullptr, error.c_str());
    require(
        session->currentRevision() == 5U
            && session->undoDepth() == 2U
            && session->redoDepth() == 1U
            && session->snapshot()->tracks[1].name
                == "Production instrument",
        "open replays commands newer than snapshot and rebuilds history"
    );

    const auto redoRename = session->redo(5U, error);
    require(
        redoRename.applied()
            && redoRename.revision == 6U
            && session->snapshot()->tracks[1].name
                == "Renamed instrument",
        "redo appends a new revision and reapplies rename"
    );
    require(
        session->undo(6U, error).applied(),
        "rename can be undone again"
    );
    require(
        session->setTempo(7U, 140.0, error).applied()
            && session->redoDepth() == 0U,
        "new edit after undo clears the redo branch"
    );
    require(
        session->undo(8U, error).applied()
            && session->snapshot()->tempo == 132.0,
        "tempo undo restores the prior durable value"
    );
    require(
        session->undo(9U, error).applied()
            && session->trackCount() == 1U,
        "undoing add-track removes the unreferenced track"
    );
    const auto redoAdd = session->redo(10U, error);
    require(
        redoAdd.applied()
            && redoAdd.revision == 11U
            && session->trackCount() == 2U
            && session->snapshot()->tracks[1].stableId
                == added.entityId,
        "redoing add-track restores the same stable ID"
    );

    session.reset();
    session = iramix::session::JournaledSession::open(
        project,
        error
    );
    require(session != nullptr, error.c_str());
    const auto recovered = session->snapshot();
    require(
        recovered->revision == 11U
            && recovered->tempo == 132.0
            && recovered->tracks.size() == 2U
            && recovered->tracks[1].stableId == 2U
            && recovered->tracks[1].name
                == "Production instrument"
            && session->undoDepth() == 2U
            && session->redoDepth() == 1U,
        "full reopen deterministically replays undo and redo records"
    );

    const auto journal =
        iramix::session::JournaledSession::journalPathForProject(
            project
        );
    const auto journalRecovery =
        iramix::persistence::recoverCommandJournal(journal);
    require(journalRecovery.ok(), journalRecovery.error.c_str());
    require(
        journalRecovery.commands.size() == 11U
            && journalRecovery.commands.front().sequence == 1U
            && journalRecovery.commands.back().sequence == 11U,
        "baseline, applied edits, and history actions enter the journal"
    );

    std::cout
        << "Journaled session: snapshot_revision=3, "
           "replayed_revision=11, baseline_records=1, "
           "durable_command_records=10, "
           "stale_records=0, undo_depth=2, redo_depth=1, "
           "stable_track_id=2\n";
}

void testJournaledEditLatency(const std::filesystem::path& root) {
    std::string error;
    auto session = iramix::session::JournaledSession::open(
        root / "journal-latency.irpx",
        error
    );
    require(session != nullptr, error.c_str());

    constexpr std::size_t iterations = 100U;
    std::vector<double> milliseconds;
    milliseconds.reserve(iterations);
    for (std::size_t index = 0U; index < iterations; ++index) {
        const auto revision = session->currentRevision();
        const auto started = std::chrono::steady_clock::now();
        const auto result = session->setTempo(
            revision,
            index % 2U == 0U ? 120.0 : 121.0,
            error
        );
        const double elapsed =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started
            ).count();
        require(result.applied(), error.c_str());
        milliseconds.push_back(elapsed);
    }
    const double p50 =
        percentileMilliseconds(milliseconds, 0.50);
    const double p95 =
        percentileMilliseconds(milliseconds, 0.95);
    const double p99 =
        percentileMilliseconds(milliseconds, 0.99);
    const double maximum = *std::max_element(
        milliseconds.begin(),
        milliseconds.end()
    );
    std::cout
        << "Journaled edit durable ACK: iterations=" << iterations
        << ", p50_ms=" << p50
        << ", p95_ms=" << p95
        << ", p99_ms=" << p99
        << ", max_ms=" << maximum << '\n';
}

void saveSnapshot(
    const std::filesystem::path& project,
    const iramix::persistence::ImmutableSessionSnapshot& snapshot
) {
    std::string error;
    const auto payload =
        iramix::persistence::serializeSessionDocument(
            *snapshot,
            error
        );
    require(!payload.empty(), error.c_str());
    require(
        iramix::persistence::saveProjectSnapshot(
            project,
            payload,
            error
        ),
        error.c_str()
    );
}

void testJournalCheckpointCompaction(
    const std::filesystem::path& root
) {
    const auto project = root / "checkpoint-session.irpx";
    std::string error;
    auto session =
        iramix::session::JournaledSession::open(project, error);
    require(session != nullptr, error.c_str());

    require(session->setTempo(1U, 121.0, error).applied(), "edit A");
    require(session->setTempo(2U, 122.0, error).applied(), "edit B");
    require(session->setTempo(3U, 123.0, error).applied(), "edit C");
    require(session->undo(4U, error).applied(), "undo C");
    require(session->redo(5U, error).applied(), "redo C");
    require(session->undo(6U, error).applied(), "undo C again");
    require(session->undo(7U, error).applied(), "undo B");
    require(
        session->setTempo(8U, 130.0, error).applied(),
        "edit D clears the abandoned branch"
    );

    const auto journal =
        iramix::session::JournaledSession::journalPathForProject(
            project
        );
    auto recovered =
        iramix::persistence::recoverCommandJournal(journal);
    require(
        recovered.ok() && recovered.commands.size() == 9U,
        "pre-checkpoint journal retains baseline and history actions"
    );
    require(
        !session->checkpoint(8U, error),
        "checkpoint rejects a snapshot older than live state"
    );
    recovered =
        iramix::persistence::recoverCommandJournal(journal);
    require(
        recovered.ok() && recovered.commands.size() == 9U,
        "rejected checkpoint leaves original journal authoritative"
    );

    std::error_code sizeError;
    const auto originalBytes =
        std::filesystem::file_size(journal, sizeError);
    require(!sizeError, "original journal size is readable");
    saveSnapshot(project, session->snapshot());
    const auto checkpointStarted =
        std::chrono::steady_clock::now();
    require(session->checkpoint(9U, error), error.c_str());
    const double checkpointMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - checkpointStarted
        ).count();
    const auto compactedBytes =
        std::filesystem::file_size(journal, sizeError);
    require(!sizeError, "compacted journal size is readable");
    recovered =
        iramix::persistence::recoverCommandJournal(journal);
    require(
        recovered.ok()
            && recovered.commands.size() == 3U
            && recovered.commands.front().sequence == 7U
            && recovered.commands.back().sequence == 9U,
        "checkpoint records baseline and retains active undo"
    );

    session.reset();
    session =
        iramix::session::JournaledSession::open(project, error);
    require(session != nullptr, error.c_str());
    require(
        session->currentRevision() == 9U
            && session->undoDepth() == 2U
            && session->redoDepth() == 0U
            && session->snapshot()->tempo == 130.0,
        "compacted edit stack reconstructs from durable snapshot"
    );
    require(
        session->undo(9U, error).applied()
            && session->snapshot()->tempo == 121.0,
        "new command appends after compacted journal sequence"
    );
    saveSnapshot(project, session->snapshot());
    require(session->checkpoint(10U, error), error.c_str());
    recovered =
        iramix::persistence::recoverCommandJournal(journal);
    require(
        recovered.ok()
            && recovered.commands.size() == 4U
            && recovered.commands.back().sequence == 10U,
        "checkpoint baseline retains one redo branch"
    );

    session.reset();
    session =
        iramix::session::JournaledSession::open(project, error);
    require(session != nullptr, error.c_str());
    require(
        session->undoDepth() == 1U
            && session->redoDepth() == 1U
            && session->redo(10U, error).applied()
            && session->snapshot()->tempo == 130.0,
        "compacted redo stack reconstructs and reapplies"
    );

    std::cout
        << "Journal checkpoint: original_records=8, "
           "initial_baseline_records=1, "
           "baseline_records=1, active_undo_records=2, "
           "redo_checkpoint_records=3, "
           "dead_branch_records_removed=6, stable_revision=10, "
           "original_bytes=" << originalBytes
        << ", compacted_bytes=" << compactedBytes
        << ", checkpoint_ms=" << checkpointMilliseconds << '\n';
}

void saveBackup(
    const std::filesystem::path& project,
    const std::uint64_t filenameRevision,
    const iramix::persistence::ImmutableSessionSnapshot& snapshot
) {
    std::string error;
    const auto payload =
        iramix::persistence::serializeSessionDocument(
            *snapshot,
            error
        );
    require(!payload.empty(), error.c_str());
    const auto saved = iramix::persistence::saveProjectBackup(
        {
            .directory =
                iramix::persistence::defaultProjectBackupDirectory(
                    project
                ),
            .retainedBackups = 10U,
        },
        filenameRevision,
        payload
    );
    require(saved.committed, saved.error.c_str());
}

void overwriteWithCorruption(
    const std::filesystem::path& path
) {
    std::ofstream output {
        path,
        std::ios::binary | std::ios::trunc
    };
    output << "not an Iramix project envelope";
    require(output.good(), "corruption fixture is written");
}

void testAutomaticBackupRestore(
    const std::filesystem::path& root
) {
    const auto project = root / "automatic-backup-restore.irpx";
    std::string error;
    auto session =
        iramix::session::JournaledSession::open(project, error);
    require(session != nullptr, error.c_str());
    require(
        session->setTempo(1U, 121.0, error).applied(),
        "restore fixture revision two applies"
    );
    const auto revisionTwo = session->snapshot();
    saveSnapshot(project, revisionTwo);
    saveBackup(project, 2U, revisionTwo);
    require(session->checkpoint(2U, error), error.c_str());

    require(
        session->setTempo(2U, 133.0, error).applied(),
        "post-checkpoint journal command applies"
    );
    const auto revisionThree = session->snapshot();
    saveBackup(
        project,
        4U,
        revisionThree
    );
    const auto backupDirectory =
        iramix::persistence::defaultProjectBackupDirectory(project);
    overwriteWithCorruption(
        iramix::persistence::projectBackupPath(
            backupDirectory,
            5U
        )
    );
    overwriteWithCorruption(project);
    session.reset();

    session =
        iramix::session::JournaledSession::open(project, error);
    require(session != nullptr, error.c_str());
    require(
        session->recoveredFromBackup()
            && session->recoveredBackupRevision() == 2U
            && session->skippedBackupCount() == 2U
            && session->snapshotRevision() == 2U
            && session->checkpointRevision() == 2U
            && session->currentRevision() == 3U
            && session->snapshot()->tempo == 133.0,
        "restore skips corrupt and revision-mismatched backups, then "
        "replays commands newer than the checkpoint baseline"
    );
    const auto restoredPrimary =
        iramix::persistence::loadProjectSnapshot(project);
    require(restoredPrimary.ok, restoredPrimary.error.c_str());
    const auto restoredDocument =
        iramix::persistence::deserializeSessionDocument(
            restoredPrimary.payload
        );
    require(
        restoredDocument.ok()
            && restoredDocument.document.revision == 2U,
        "validated backup atomically restores the active project"
    );

    const auto stalePrimaryProject =
        root / "stale-primary-restore.irpx";
    auto stalePrimary =
        iramix::session::JournaledSession::open(
            stalePrimaryProject,
            error
        );
    require(stalePrimary != nullptr, error.c_str());
    const auto revisionOne = stalePrimary->snapshot();
    require(
        stalePrimary->setTempo(1U, 144.0, error).applied(),
        "stale-primary fixture edit applies"
    );
    const auto stalePrimaryRevisionTwo = stalePrimary->snapshot();
    saveSnapshot(stalePrimaryProject, stalePrimaryRevisionTwo);
    saveBackup(
        stalePrimaryProject,
        2U,
        stalePrimaryRevisionTwo
    );
    require(stalePrimary->checkpoint(2U, error), error.c_str());
    saveSnapshot(stalePrimaryProject, revisionOne);
    stalePrimary.reset();
    stalePrimary =
        iramix::session::JournaledSession::open(
            stalePrimaryProject,
            error
        );
    require(stalePrimary != nullptr, error.c_str());
    require(
        stalePrimary->recoveredFromBackup()
            && stalePrimary->recoveredBackupRevision() == 2U
            && stalePrimary->currentRevision() == 2U
            && stalePrimary->snapshot()->tempo == 144.0,
        "schema-valid primary older than checkpoint falls back to backup"
    );

    const auto journalOnlyProject =
        root / "journal-only-restore.irpx";
    auto journalOnly =
        iramix::session::JournaledSession::open(
            journalOnlyProject,
            error
        );
    require(journalOnly != nullptr, error.c_str());
    require(
        journalOnly->setTempo(1U, 155.0, error).applied(),
        "journal-only fixture edit applies"
    );
    journalOnly.reset();
    journalOnly =
        iramix::session::JournaledSession::open(
            journalOnlyProject,
            error
        );
    require(journalOnly != nullptr, error.c_str());
    require(
        !journalOnly->recoveredFromBackup()
            && journalOnly->currentRevision() == 2U
            && journalOnly->snapshot()->tempo == 155.0,
        "explicit default baseline preserves journal-only recovery"
    );

    const auto legacyProject = root / "legacy-backup-restore.irpx";
    auto legacy =
        iramix::session::JournaledSession::open(
            legacyProject,
            error
        );
    require(legacy != nullptr, error.c_str());
    require(
        legacy->setTempo(1U, 140.0, error).applied(),
        "legacy restore fixture revision two applies"
    );
    const auto legacyRevisionTwo = legacy->snapshot();
    saveBackup(legacyProject, 2U, legacyRevisionTwo);
    require(
        legacy->setTempo(2U, 150.0, error).applied(),
        "legacy restore fixture revision three applies"
    );
    const auto legacyRevisionThree = legacy->snapshot();
    const auto legacyJournal =
        iramix::session::JournaledSession::journalPathForProject(
            legacyProject
        );
    const auto withBaseline =
        iramix::persistence::recoverCommandJournal(legacyJournal);
    require(
        withBaseline.ok()
            && withBaseline.commands.size() == 3U,
        "legacy fixture begins with an explicit baseline"
    );
    require(
        iramix::persistence::rewriteCommandJournal(
            legacyJournal,
            std::span<const iramix::persistence::JournalCommand> {
                withBaseline.commands
            }.subspan(1U),
            error
        ),
        error.c_str()
    );
    legacy.reset();

    legacy =
        iramix::session::JournaledSession::open(
            legacyProject,
            error
        );
    require(
        legacy == nullptr
            && error.find("safe journal restore baseline")
                != std::string::npos,
        "legacy journal rejects a backup older than its last sequence"
    );
    saveBackup(legacyProject, 3U, legacyRevisionThree);
    legacy =
        iramix::session::JournaledSession::open(
            legacyProject,
            error
        );
    require(legacy != nullptr, error.c_str());
    require(
        legacy->recoveredFromBackup()
            && legacy->recoveredBackupRevision() == 3U
            && legacy->currentRevision() == 3U
            && legacy->snapshot()->tempo == 150.0,
        "legacy journal accepts an equally-new validated backup"
    );

    std::cout
        << "Automatic backup restore: checkpoint_baseline=2, "
           "backup_revision=2, replayed_revision=3, "
           "corrupt_backups_skipped=1, revision_mismatches_skipped=1, "
           "stale_valid_primaries_rejected=1, journal_only_recoveries=1, "
           "legacy_stale_backups_rejected=1, "
           "active_project_replacements=3\n";
}

void testAutosaveScheduler(const std::filesystem::path& root) {
    const auto project = root / "autosave-session.irpx";
    std::string error;
    auto session =
        iramix::session::JournaledSession::open(project, error);
    require(session != nullptr, error.c_str());
    auto service =
        iramix::persistence::SessionPersistenceService::create(
            project,
            std::chrono::milliseconds {30},
            error
        );
    require(service != nullptr, error.c_str());
    require(service->start(error), error.c_str());

    const auto started = std::chrono::steady_clock::now();
    require(
        session->setTempo(1U, 121.0, error).applied(),
        "first autosave edit applies"
    );
    require(
        service->markDirty(session->snapshot())
            == iramix::persistence::AutosaveDirtyStatus::tracked,
        "first dirty revision starts a fixed autosave window"
    );
    std::this_thread::sleep_for(std::chrono::milliseconds {5});
    require(
        session->setTempo(2U, 122.0, error).applied(),
        "second autosave edit applies"
    );
    require(
        service->markDirty(session->snapshot())
            == iramix::persistence::AutosaveDirtyStatus::replaced,
        "newer edit replaces the tracked autosave snapshot"
    );
    std::this_thread::sleep_for(std::chrono::milliseconds {5});
    require(
        session->setTempo(3U, 123.0, error).applied(),
        "third autosave edit applies"
    );
    require(
        service->markDirty(session->snapshot())
            == iramix::persistence::AutosaveDirtyStatus::replaced,
        "continuous edits coalesce to the latest revision"
    );

    const auto timeout = std::chrono::steady_clock::now()
        + std::chrono::seconds {5};
    auto query = service->query(4U);
    while (
        query.status
            != iramix::persistence::SessionSaveQueryStatus::committed
        && std::chrono::steady_clock::now() < timeout
    ) {
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
        query = service->query(4U);
    }
    const auto elapsed =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started
        ).count();
    const auto dirtyRevision = service->dirtyRevision();
    const auto autosaveRequests =
        service->autosaveRequestCount();
    const auto dirtyReplacements =
        service->dirtyReplacementCount();
    require(
        query.status
                == iramix::persistence::
                    SessionSaveQueryStatus::committed
            && query.durableRevision == 4U
            && dirtyRevision == 0U
            && autosaveRequests >= 1U
            && autosaveRequests <= 3U
            && dirtyReplacements == 2U,
        "autosave commits the newest continuous-edit revision"
    );
    require(elapsed < 1'000.0, "autosave fixed window does not starve");
    service->stop();

    const auto loaded =
        iramix::persistence::loadProjectSnapshot(project);
    require(loaded.ok, loaded.error.c_str());
    const auto decoded =
        iramix::persistence::deserializeSessionDocument(
            loaded.payload
        );
    require(
        decoded.ok()
            && decoded.document.revision == 4U
            && decoded.document.tempo == 123.0,
        "autosaved project reopens at the latest dirty revision"
    );

    const auto shutdownProject =
        root / "shutdown-flush-session.irpx";
    auto shutdownSession =
        iramix::session::JournaledSession::open(
            shutdownProject,
            error
        );
    require(shutdownSession != nullptr, error.c_str());
    auto shutdownService =
        iramix::persistence::SessionPersistenceService::create(
            shutdownProject,
            std::chrono::seconds {10},
            error
        );
    require(shutdownService != nullptr, error.c_str());
    require(shutdownService->start(error), error.c_str());
    require(
        shutdownSession->setTempo(1U, 144.0, error).applied(),
        "shutdown fixture edit applies"
    );
    require(
        shutdownService->markDirty(shutdownSession->snapshot())
            == iramix::persistence::AutosaveDirtyStatus::tracked,
        "shutdown fixture tracks dirty state"
    );
    shutdownService->stop();
    require(
        shutdownService->durableRevision() == 2U,
        "shutdown flushes the latest dirty revision"
    );

    std::cout
        << "Autosave scheduler: interval_ms=30, edits=3, "
           "autosave_requests=" << autosaveRequests
        << ", dirty_replacements=" << dirtyReplacements << ", "
           "durable_revision=4, elapsed_ms=" << elapsed
        << ", shutdown_flush_revision=2\n";
}

void testSaveCoalescing(const std::filesystem::path& root) {
    iramix::session::SessionController controller;
    const auto revisionOne = controller.snapshot();
    require(
        controller.setTempo(1U, 121.0).applied(),
        "revision two applies"
    );
    const auto revisionTwo = controller.snapshot();
    require(
        controller.setTempo(2U, 122.0).applied(),
        "revision three applies"
    );
    const auto revisionThree = controller.snapshot();
    require(
        controller.setTempo(3U, 123.0).applied(),
        "revision four applies"
    );
    const auto revisionFour = controller.snapshot();

    std::string error;
    const auto project = root / "coalesced-session.irpx";
    auto coordinator =
        iramix::persistence::SessionSaveCoordinator::create(
            project,
            error
        );
    require(coordinator != nullptr, error.c_str());

    require(
        coordinator->requestSave(revisionTwo)
            == iramix::persistence::
                SessionSaveRequestStatus::accepted,
        "first session save is accepted"
    );
    require(
        coordinator->requestSave(revisionThree)
            == iramix::persistence::
                SessionSaveRequestStatus::coalesced,
        "save burst waits behind accepted revision"
    );
    require(
        coordinator->requestSave(revisionFour)
            == iramix::persistence::
                SessionSaveRequestStatus::coalesced,
        "newest save replaces unaccepted pending revision"
    );
    require(
        coordinator->requestSave(revisionFour)
            == iramix::persistence::
                SessionSaveRequestStatus::alreadyRequested,
        "duplicate latest revision is idempotent"
    );
    require(
        coordinator->requestSave(revisionOne)
            == iramix::persistence::
                SessionSaveRequestStatus::invalidRevision,
        "older save revision is rejected"
    );
    require(
        coordinator->inFlightRevision() == 2U
            && coordinator->pendingRevision() == 4U
            && coordinator->submittedCount() == 1U
            && coordinator->coalescedCount() == 2U,
        "coordinator retains one accepted and one latest snapshot"
    );

    require(coordinator->start(error), error.c_str());
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds {5};
    while (
        coordinator->query(4U).status
            != iramix::persistence::
                SessionSaveQueryStatus::committed
        && std::chrono::steady_clock::now() < deadline
    ) {
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }
    const auto latest = coordinator->query(4U);
    require(
        latest.status
                == iramix::persistence::
                    SessionSaveQueryStatus::committed
            && latest.durableRevision == 4U
            && latest.completion.serializedBytes != 0U,
        "latest coalesced revision becomes durable"
    );
    require(
        coordinator->query(2U).status
            == iramix::persistence::
                SessionSaveQueryStatus::committed,
        "later durable revision covers earlier accepted edits"
    );
    coordinator->stop();
    require(
        coordinator->submittedCount() == 2U
            && coordinator->durableRevision() == 4U,
        "only first and latest revisions reach the worker"
    );

    const auto loaded =
        iramix::persistence::loadProjectSnapshot(project);
    require(loaded.ok, loaded.error.c_str());
    const auto decoded =
        iramix::persistence::deserializeSessionDocument(
            loaded.payload
        );
    require(
        decoded.ok()
            && decoded.document.revision == 4U
            && decoded.document.tempo == 123.0,
        "coalesced project reopens at latest production revision"
    );

    std::cout
        << "Session save coordinator: requested_revision=4, "
           "durable_revision=4, submitted=2, coalesced=2, "
           "duplicate_requests=1, stale_rejections=1\n";
}

void testReferenceSnapshotBenchmark() {
    std::string error;
    auto controller =
        iramix::session::SessionController::fromDocument(
            makeReferenceSession(),
            error
        );
    require(controller != nullptr, error.c_str());

    constexpr std::size_t iterations = 20U;
    std::vector<double> snapshotMilliseconds;
    snapshotMilliseconds.reserve(iterations);
    std::size_t observedPoints = 0U;
    for (std::size_t index = 0U; index < iterations; ++index) {
        const auto started = std::chrono::steady_clock::now();
        const auto snapshot = controller->snapshot();
        const double elapsed =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started
            ).count();
        snapshotMilliseconds.push_back(elapsed);
        observedPoints += snapshot->automationLanes[0].points.size();
    }
    require(
        observedPoints == iterations * 1'000U,
        "reference snapshots retain automation data"
    );

    const double p50 = percentileMilliseconds(
        snapshotMilliseconds,
        0.50
    );
    const double p95 = percentileMilliseconds(
        snapshotMilliseconds,
        0.95
    );
    const double p99 = percentileMilliseconds(
        snapshotMilliseconds,
        0.99
    );
    const double maximum = *std::max_element(
        snapshotMilliseconds.begin(),
        snapshotMilliseconds.end()
    );
    std::cout
        << "Reference session snapshot: tracks=200, clips=2000, "
           "automation_points=40000, iterations=" << iterations
        << ", snapshot_p50_ms=" << p50
        << ", snapshot_p95_ms=" << p95
        << ", snapshot_p99_ms=" << p99
        << ", snapshot_max_ms=" << maximum << '\n';
}

void testRunningCoordinator(const std::filesystem::path& root) {
    iramix::session::SessionController controller;
    require(
        controller.setTempo(1U, 132.0).applied(),
        "running coordinator fixture edit applies"
    );
    std::string error;
    auto coordinator =
        iramix::persistence::SessionSaveCoordinator::create(
            root / "running-coordinator.irpx",
            error
        );
    require(coordinator != nullptr, error.c_str());
    require(coordinator->start(error), error.c_str());
    require(
        coordinator->requestSave(controller.snapshot())
            == iramix::persistence::
                SessionSaveRequestStatus::accepted,
        "running coordinator accepts snapshot"
    );
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds {5};
    auto query = coordinator->query(2U);
    while (
        query.status
            == iramix::persistence::SessionSaveQueryStatus::pending
        && std::chrono::steady_clock::now() < deadline
    ) {
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
        query = coordinator->query(2U);
    }
    require(
        query.status
            == iramix::persistence::SessionSaveQueryStatus::committed,
        "running coordinator publishes durable completion"
    );
    coordinator->stop();
}

} // namespace

int main() {
    TemporaryDirectory temporary;
    testSessionController();
    testJournaledSession(temporary.path());
    testJournaledEditLatency(temporary.path());
    testJournalCheckpointCompaction(temporary.path());
    testAutomaticBackupRestore(temporary.path());
    testAutosaveScheduler(temporary.path());
    testSaveCoalescing(temporary.path());
    testRunningCoordinator(temporary.path());
    testReferenceSnapshotBenchmark();
    std::cout << "All Iramix session tests passed.\n";
    return EXIT_SUCCESS;
}
