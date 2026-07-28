#include "iramix/persistence/AsyncProjectSaver.hpp"
#include "iramix/persistence/AsyncSessionSaver.hpp"
#include "iramix/persistence/CommandJournal.hpp"
#include "iramix/persistence/DiskAudioWorkers.hpp"
#include "iramix/persistence/ProjectBackupStore.hpp"
#include "iramix/persistence/ProjectStore.hpp"
#include "iramix/persistence/RecoverableRecording.hpp"
#include "iramix/persistence/SessionDocument.hpp"
#include "iramix/realtime/Audit.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] std::vector<std::byte> bytesOf(
    const std::string_view text
) {
    const auto* const begin = reinterpret_cast<const std::byte*>(
        text.data()
    );
    return {begin, begin + text.size()};
}

[[nodiscard]] std::string textOf(
    const std::span<const std::byte> bytes
) {
    return {
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size(),
    };
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
        path_ = std::filesystem::temp_directory_path()
            / ("iramix-persistence-" + std::to_string(suffix));
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

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds {5};
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
    }
    return predicate();
}

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

void testAtomicProjectStore(const std::filesystem::path& root) {
    const auto project = root / "project" / "session.irpx";
    const auto first = bytesOf("session-revision-1");
    const auto second = bytesOf("session-revision-2");
    const auto recoveredPayload = bytesOf("recovered-staging");
    std::string error;

    require(
        iramix::persistence::saveProjectSnapshot(
            project,
            first,
            error
        ),
        error.c_str()
    );
    auto loaded = iramix::persistence::loadProjectSnapshot(project);
    require(loaded.ok, loaded.error.c_str());
    require(
        textOf(loaded.payload) == "session-revision-1",
        "initial project round trip"
    );

    require(
        !iramix::persistence::saveProjectSnapshot(
            project,
            second,
            error,
            iramix::persistence::AtomicSaveFailurePoint::
                afterStagingFlush
        ),
        "injected save failure"
    );
    loaded = iramix::persistence::loadProjectSnapshot(project);
    require(loaded.ok, loaded.error.c_str());
    require(
        textOf(loaded.payload) == "session-revision-1",
        "failed replacement preserves committed snapshot"
    );

    require(
        iramix::persistence::saveProjectSnapshot(
            project,
            second,
            error
        ),
        error.c_str()
    );
    loaded = iramix::persistence::loadProjectSnapshot(project);
    require(
        loaded.ok && textOf(loaded.payload) == "session-revision-2",
        "successful replacement commits new snapshot"
    );

    std::filesystem::remove(project);
    require(
        !iramix::persistence::saveProjectSnapshot(
            project,
            recoveredPayload,
            error,
            iramix::persistence::AtomicSaveFailurePoint::
                afterStagingFlush
        ),
        "staging-only crash simulation"
    );
    loaded = iramix::persistence::loadProjectSnapshot(project);
    require(loaded.ok, loaded.error.c_str());
    require(
        loaded.recoveredFromStaging
            && textOf(loaded.payload) == "recovered-staging",
        "complete staging snapshot is promoted"
    );

    std::cout
        << "Atomic project store: committed_revisions=2, "
           "injected_failures=2, staging_recoveries=1\n";
}

void testProjectBackupRotation(
    const std::filesystem::path& root
) {
    const auto directory = root / "revisioned-backups";
    const iramix::persistence::ProjectBackupPolicy policy {
        .directory = directory,
        .retainedBackups = 3U,
    };

    for (std::uint64_t revision = 1U; revision <= 5U; ++revision) {
        const auto payload =
            bytesOf("backup-revision-" + std::to_string(revision));
        const auto saved = iramix::persistence::saveProjectBackup(
            policy,
            revision,
            payload
        );
        require(saved.committed, saved.error.c_str());
        require(
            saved.retentionApplied,
            "successful backup applies retention"
        );
    }

    const auto unknown = directory / "do-not-prune.user-data";
    {
        std::ofstream output {unknown, std::ios::binary};
        output << "owned by user";
    }
    const auto sixthPayload = bytesOf("backup-revision-6");
    const auto sixth = iramix::persistence::saveProjectBackup(
        policy,
        6U,
        sixthPayload
    );
    require(
        sixth.committed
            && sixth.retentionApplied
            && sixth.prunedCount == 1U
            && sixth.retainedCount == 3U,
        sixth.error.c_str()
    );

    const auto listing =
        iramix::persistence::listProjectBackups(directory);
    require(listing.ok, listing.error.c_str());
    require(
        listing.entries.size() == 3U
            && listing.entries[0].revision == 6U
            && listing.entries[1].revision == 5U
            && listing.entries[2].revision == 4U,
        "retention keeps the newest three revisioned backups"
    );
    require(
        std::filesystem::exists(unknown),
        "retention never removes an unrecognized user file"
    );

    const auto corruptNewest =
        iramix::persistence::projectBackupPath(directory, 7U);
    {
        std::ofstream output {corruptNewest, std::ios::binary};
        output << "not an Iramix project envelope";
    }
    const auto recovered =
        iramix::persistence::recoverNewestProjectBackup(directory);
    require(recovered.recovered, recovered.error.c_str());
    require(
        recovered.revision == 6U
            && recovered.skippedInvalidCount == 1U
            && textOf(recovered.payload) == "backup-revision-6",
        "recovery skips corrupt newest backup and selects prior valid"
    );

    const auto project = root / "backup-failure-isolation.irpx";
    const auto invalidBackupDirectory =
        root / "backup-path-is-a-file";
    {
        std::ofstream output {
            invalidBackupDirectory,
            std::ios::binary
        };
        output << "prevents directory creation";
    }
    std::string error;
    auto saver = iramix::persistence::AsyncSessionSaver::create(
        project,
        1U,
        error,
        {
            .directory = invalidBackupDirectory,
            .retainedBackups = 3U,
        }
    );
    require(saver != nullptr, error.c_str());
    iramix::persistence::SessionDocument document;
    document.revision = 1U;
    document.tracks.push_back({
        .stableId = 1U,
        .type = iramix::persistence::SessionTrackType::audio,
        .gain = 1.0F,
        .color = 0U,
        .name = "Primary remains durable",
    });
    const auto snapshot =
        std::make_shared<const iramix::persistence::SessionDocument>(
            std::move(document)
        );
    require(saver->start(error), error.c_str());
    require(
        saver->trySubmit(1U, snapshot)
            == iramix::persistence::ProjectSaveSubmitResult::accepted,
        "backup failure isolation save is accepted"
    );
    saver->stop();
    iramix::persistence::SessionSaveCompletion completion;
    require(
        saver->tryPopCompletion(completion)
            && completion.status
                == iramix::persistence::
                    ProjectSaveCompletionStatus::committed
            && completion.backupStatus
                == iramix::persistence::
                    SessionBackupCompletionStatus::failed
            && completion.backupDetail[0] != '\0',
        "backup failure is observable without downgrading primary ACK"
    );
    const auto primary =
        iramix::persistence::loadProjectSnapshot(project);
    require(primary.ok, primary.error.c_str());

    std::cout
        << "Project backups: committed=6, retention=3, pruned=3, "
           "corrupt_skipped=1, unknown_files_preserved=1, "
           "primary_ack_isolated_from_backup_failure=1\n";
}

void testSessionDocumentRoundTrip(
    const std::filesystem::path& root
) {
    iramix::persistence::SessionDocument source;
    source.revision = 42U;
    source.sampleRate = 96'000U;
    source.tempo = 137.5;
    source.tracks = {
        {
            .stableId = 101U,
            .type = iramix::persistence::SessionTrackType::audio,
            .gain = 0.75F,
            .color = 0xFF33'6699U,
            .name = "Field recording",
        },
        {
            .stableId = 205U,
            .type =
                iramix::persistence::SessionTrackType::instrument,
            .gain = 1.0F,
            .color = 0xFFAA'5500U,
            .name = "Orchestra",
        },
        {
            .stableId = 999U,
            .type = iramix::persistence::SessionTrackType::master,
            .gain = 0.9F,
            .color = 0xFF22'2222U,
            .name = "Master",
        },
    };
    source.clips = {
        {
            .stableId = 2'001U,
            .trackId = 101U,
            .sourceId = 50'001U,
            .startFrame = 0U,
            .lengthFrames = 48'000U,
            .sourceOffsetFrames = 1'024U,
            .gain = 0.8F,
            .muted = false,
            .name = "Texture",
        },
        {
            .stableId = 2'002U,
            .trackId = 205U,
            .sourceId = 50'002U,
            .startFrame = 96'000U,
            .lengthFrames = 24'000U,
            .sourceOffsetFrames = 0U,
            .gain = 1.0F,
            .muted = true,
            .name = "Phrase",
        },
    };
    source.routes = {
        {
            .stableId = 3'001U,
            .sourceTrackId = 101U,
            .destinationTrackId = 999U,
            .gain = 0.9F,
            .enabled = true,
        },
    };
    source.automationLanes = {
        {
            .stableId = 4'001U,
            .targetTrackId = 205U,
            .parameter =
                iramix::persistence::SessionParameterId::gain,
            .points = {
                {.samplePosition = 0U, .value = 0.25F},
                {.samplePosition = 48'000U, .value = 0.75F},
                {.samplePosition = 96'000U, .value = 1.0F},
            },
        },
    };

    std::string error;
    const auto currentBytes =
        iramix::persistence::serializeSessionDocument(
            source,
            error
        );
    require(!currentBytes.empty(), error.c_str());
    auto decoded =
        iramix::persistence::deserializeSessionDocument(
            currentBytes
        );
    require(decoded.ok(), decoded.error.c_str());
    require(
        decoded.sourceSchemaVersion
                == iramix::persistence::currentSessionSchemaVersion
            && !decoded.migrated
            && decoded.document.revision == source.revision
            && decoded.document.sampleRate == source.sampleRate
            && decoded.document.tempo == source.tempo
            && decoded.document.tracks.size()
                == source.tracks.size()
            && decoded.document.tracks[0].stableId == 101U
            && decoded.document.tracks[0].name
                == "Field recording"
            && decoded.document.tracks[1].color
                == 0xFFAA'5500U
            && decoded.document.tracks[2].type
                == iramix::persistence::SessionTrackType::master
            && decoded.document.clips.size() == 2U
            && decoded.document.clips[1].muted
            && decoded.document.routes.size() == 1U
            && decoded.document.routes[0].destinationTrackId
                == 999U
            && decoded.document.automationLanes.size() == 1U
            && decoded.document.automationLanes[0].points.size()
                == 3U,
        "current session schema round trips deterministically"
    );

    auto legacySource = source;
    legacySource.clips.clear();
    legacySource.routes.clear();
    legacySource.automationLanes.clear();
    const auto legacyBytes =
        iramix::persistence::serializeSessionDocument(
            legacySource,
            error,
            1U
        );
    require(!legacyBytes.empty(), error.c_str());
    decoded =
        iramix::persistence::deserializeSessionDocument(
            legacyBytes
        );
    require(decoded.ok(), decoded.error.c_str());
    require(
        decoded.sourceSchemaVersion == 1U
            && decoded.migrated
            && decoded.document.sampleRate == 48'000U
            && std::all_of(
                decoded.document.tracks.begin(),
                decoded.document.tracks.end(),
                [](const auto& track) {
                    return track.color == 0U;
                }
            ),
        "schema v1 migrates sample rate and track color defaults"
    );
    const auto schemaV2Bytes =
        iramix::persistence::serializeSessionDocument(
            legacySource,
            error,
            2U
        );
    require(!schemaV2Bytes.empty(), error.c_str());
    decoded =
        iramix::persistence::deserializeSessionDocument(
            schemaV2Bytes
        );
    require(
        decoded.ok()
            && decoded.sourceSchemaVersion == 2U
            && decoded.migrated
            && decoded.document.sampleRate == source.sampleRate
            && decoded.document.clips.empty()
            && decoded.document.routes.empty()
            && decoded.document.automationLanes.empty(),
        "schema v2 migrates empty v3 entity collections"
    );
    require(
        iramix::persistence::serializeSessionDocument(
            source,
            error,
            2U
        ).empty(),
        "lossy export to schema v2 is rejected"
    );

    const auto project = root / "session-round-trip.irpx";
    require(
        iramix::persistence::saveProjectSnapshot(
            project,
            currentBytes,
            error
        ),
        error.c_str()
    );
    const auto loaded =
        iramix::persistence::loadProjectSnapshot(project);
    require(loaded.ok, loaded.error.c_str());
    decoded =
        iramix::persistence::deserializeSessionDocument(
            loaded.payload
        );
    require(
        decoded.ok()
            && decoded.document.revision == 42U
            && decoded.document.tracks.size() == 3U
            && decoded.document.clips.size() == 2U
            && decoded.document.routes.size() == 1U
            && decoded.document.automationLanes.size() == 1U,
        "session document round trips through project envelope"
    );

    auto unsupported = currentBytes;
    unsupported[4] = std::byte {99U};
    decoded =
        iramix::persistence::deserializeSessionDocument(
            unsupported
        );
    require(
        !decoded.ok(),
        "unknown future session schema is rejected"
    );

    auto duplicateIds = source;
    duplicateIds.tracks[1].stableId =
        duplicateIds.tracks[0].stableId;
    require(
        iramix::persistence::serializeSessionDocument(
            duplicateIds,
            error
        ).empty(),
        "duplicate stable session IDs are rejected"
    );
    auto brokenReference = source;
    brokenReference.clips[0].trackId = 987'654U;
    require(
        iramix::persistence::serializeSessionDocument(
            brokenReference,
            error
        ).empty(),
        "dangling session references are rejected"
    );

    std::cout
        << "Session document: current_schema=3, tracks=3, "
           "clips=2, routes=1, automation_lanes=1, "
           "v1_migrations=1, v2_migrations=1, "
           "unknown_schemas_rejected=1, project_round_trips=1\n";
}

void testAsyncProjectSaver(
    const std::filesystem::path& root
) {
    const auto project = root / "async-project.irpx";
    std::string error;
    require(
        iramix::persistence::saveProjectSnapshot(
            project,
            bytesOf("committed-base"),
            error
        ),
        error.c_str()
    );

    auto saver =
        iramix::persistence::AsyncProjectSaver::create(
            project,
            2U,
            error
        );
    require(saver != nullptr, error.c_str());
    const auto failedPayload =
        std::make_shared<const std::vector<std::byte>>(
            bytesOf("must-not-commit")
        );
    const auto committedPayload =
        std::make_shared<const std::vector<std::byte>>(
            bytesOf("committed-async")
        );
    require(
        saver->trySubmit(
            1U,
            failedPayload,
            iramix::persistence::AtomicSaveFailurePoint::
                afterStagingFlush
        ) == iramix::persistence::ProjectSaveSubmitResult::accepted,
        "first asynchronous save is accepted"
    );
    require(
        saver->trySubmit(2U, committedPayload)
            == iramix::persistence::ProjectSaveSubmitResult::accepted,
        "second asynchronous save is accepted"
    );
    require(
        saver->trySubmit(3U, committedPayload)
            == iramix::persistence::ProjectSaveSubmitResult::full,
        "asynchronous save pipeline reports saturation"
    );
    require(
        saver->trySubmit(2U, committedPayload)
            == iramix::persistence::ProjectSaveSubmitResult::
                invalidRevision,
        "asynchronous save rejects duplicate revisions"
    );
    iramix::persistence::ProjectSaveCompletion completion;
    require(
        !saver->tryPopCompletion(completion),
        "save completion is absent before durable worker processing"
    );
    auto loaded = iramix::persistence::loadProjectSnapshot(project);
    require(
        loaded.ok && textOf(loaded.payload) == "committed-base",
        "unprocessed requests cannot change the committed project"
    );

    require(saver->start(error), error.c_str());
    saver->stop();
    require(
        saver->completionCount() == 2U
            && saver->pendingSaveCount() == 0U,
        "stop drains accepted asynchronous saves"
    );
    require(
        saver->tryPopCompletion(completion)
            && completion.revision == 1U
            && completion.status
                == iramix::persistence::
                    ProjectSaveCompletionStatus::failed
            && completion.detail[0] != '\0',
        "injected save failure produces an explicit rejection"
    );
    require(
        saver->tryPopCompletion(completion)
            && completion.revision == 2U
            && completion.status
                == iramix::persistence::
                    ProjectSaveCompletionStatus::committed
            && completion.detail[0] == '\0',
        "durable replacement produces the matching ACK"
    );
    require(
        saver->outstandingCount() == 0U
            && saver->acceptedCount() == 2U
            && saver->rejectedCount() == 2U,
        "request and completion accounting closes exactly"
    );
    loaded = iramix::persistence::loadProjectSnapshot(project);
    require(
        loaded.ok
            && textOf(loaded.payload) == "committed-async",
        "only the successful asynchronous revision commits"
    );

    const auto loadProject = root / "async-load.irpx";
    auto loadSaver =
        iramix::persistence::AsyncProjectSaver::create(
            loadProject,
            8U,
            error
        );
    require(loadSaver != nullptr, error.c_str());
    require(loadSaver->start(error), error.c_str());

    constexpr std::uint64_t revisionCount = 200U;
    std::vector<double> submitMilliseconds;
    submitMilliseconds.reserve(revisionCount);
    std::uint64_t committedCompletions = 0U;
    for (
        std::uint64_t revision = 1U;
        revision <= revisionCount;
        ++revision
    ) {
        iramix::persistence::SessionDocument document;
        document.revision = revision;
        document.tracks = {
            {
                .stableId = 1U,
                .type =
                    iramix::persistence::SessionTrackType::audio,
                .gain = 1.0F,
                .color = 0xFF44'6688U,
                .name = "Latency fixture",
            },
        };
        auto serialized =
            iramix::persistence::serializeSessionDocument(
                document,
                error
            );
        require(!serialized.empty(), error.c_str());
        const auto immutable =
            std::make_shared<const std::vector<std::byte>>(
                std::move(serialized)
            );

        for (;;) {
            const auto started =
                std::chrono::steady_clock::now();
            const auto result = loadSaver->trySubmit(
                revision,
                immutable
            );
            const double elapsed =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started
                ).count();
            if (result
                == iramix::persistence::
                    ProjectSaveSubmitResult::accepted) {
                submitMilliseconds.push_back(elapsed);
                break;
            }
            require(
                result
                    == iramix::persistence::
                        ProjectSaveSubmitResult::full,
                "load save submission only backpressures when full"
            );
            while (loadSaver->tryPopCompletion(completion)) {
                require(
                    completion.status
                        == iramix::persistence::
                            ProjectSaveCompletionStatus::committed,
                    completion.detail.data()
                );
                ++committedCompletions;
            }
            std::this_thread::yield();
        }
        while (loadSaver->tryPopCompletion(completion)) {
            require(
                completion.status
                    == iramix::persistence::
                        ProjectSaveCompletionStatus::committed,
                completion.detail.data()
            );
            ++committedCompletions;
        }
    }
    require(
        waitUntil([&loadSaver, &completion, &committedCompletions] {
            while (loadSaver->tryPopCompletion(completion)) {
                require(
                    completion.status
                        == iramix::persistence::
                            ProjectSaveCompletionStatus::committed,
                    completion.detail.data()
                );
                ++committedCompletions;
            }
            return committedCompletions == revisionCount;
        }),
        "all load-test save completions arrive"
    );
    loadSaver->stop();

    const double submitP50 = percentileMilliseconds(
        submitMilliseconds,
        0.50
    );
    const double submitP95 = percentileMilliseconds(
        submitMilliseconds,
        0.95
    );
    const double submitP99 = percentileMilliseconds(
        submitMilliseconds,
        0.99
    );
    const double submitMaximum = *std::max_element(
        submitMilliseconds.begin(),
        submitMilliseconds.end()
    );
    require(
        submitP99 < 16.0,
        "asynchronous save submit p99 stays below UI stall budget"
    );
    loaded = iramix::persistence::loadProjectSnapshot(loadProject);
    require(loaded.ok, loaded.error.c_str());
    const auto finalSession =
        iramix::persistence::deserializeSessionDocument(
            loaded.payload
        );
    require(
        finalSession.ok()
            && finalSession.document.revision == revisionCount,
        "load-test final durable revision opens successfully"
    );

    std::cout
        << "Async project saver: revisions=" << revisionCount
        << ", capacity=8, committed_completions="
        << committedCompletions
        << ", submit_p50_ms=" << submitP50
        << ", submit_p95_ms=" << submitP95
        << ", submit_p99_ms=" << submitP99
        << ", submit_max_ms=" << submitMaximum
        << ", injected_failures=1, explicit_full_rejections=1\n";
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
    document.revision = 10'000U;
    document.sampleRate = 48'000U;
    document.tempo = 128.0;
    document.tracks.reserve(trackCount);
    for (std::size_t index = 0U; index < trackCount; ++index) {
        document.tracks.push_back({
            .stableId = trackBase + index,
            .type = index + 1U == trackCount
                ? iramix::persistence::SessionTrackType::master
                : (index % 2U == 0U
                    ? iramix::persistence::SessionTrackType::audio
                    : iramix::persistence::
                        SessionTrackType::instrument),
            .gain = 0.9F,
            .color = 0xFF20'3040U
                + static_cast<std::uint32_t>(index),
            .name = "Reference track " + std::to_string(index),
        });
    }

    document.clips.reserve(clipCount);
    for (std::size_t index = 0U; index < clipCount; ++index) {
        const auto trackIndex = index % (trackCount - 1U);
        document.clips.push_back({
            .stableId = clipBase + index,
            .trackId = trackBase + trackIndex,
            .sourceId = sourceBase + index,
            .startFrame = static_cast<std::uint64_t>(index)
                * 12'000U,
            .lengthFrames = 24'000U,
            .sourceOffsetFrames =
                static_cast<std::uint64_t>(index % 8U) * 512U,
            .gain = 1.0F,
            .muted = index % 97U == 0U,
            .name = "Reference clip " + std::to_string(index),
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

void testReferenceProjectBenchmark(
    const std::filesystem::path& root
) {
    constexpr std::size_t benchmarkIterations = 20U;
    const auto reference = makeReferenceSession();
    std::string error;
    std::vector<double> serializationMilliseconds;
    serializationMilliseconds.reserve(benchmarkIterations);
    std::vector<std::byte> serialized;
    for (
        std::size_t iteration = 0U;
        iteration < benchmarkIterations;
        ++iteration
    ) {
        const auto started = std::chrono::steady_clock::now();
        auto candidate =
            iramix::persistence::serializeSessionDocument(
                reference,
                error
            );
        const double elapsed =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started
            ).count();
        require(!candidate.empty(), error.c_str());
        serializationMilliseconds.push_back(elapsed);
        serialized = std::move(candidate);
    }

    const auto project = root / "reference-large.irpx";
    require(
        iramix::persistence::saveProjectSnapshot(
            project,
            serialized,
            error
        ),
        error.c_str()
    );

    std::vector<double> openMilliseconds;
    openMilliseconds.reserve(benchmarkIterations);
    for (
        std::size_t iteration = 0U;
        iteration < benchmarkIterations;
        ++iteration
    ) {
        const auto started = std::chrono::steady_clock::now();
        const auto loaded =
            iramix::persistence::loadProjectSnapshot(project);
        require(loaded.ok, loaded.error.c_str());
        const auto decoded =
            iramix::persistence::deserializeSessionDocument(
                loaded.payload
            );
        const double elapsed =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started
            ).count();
        require(
            decoded.ok()
                && decoded.document.tracks.size() == 200U
                && decoded.document.clips.size() == 2'000U
                && decoded.document.routes.size() == 199U
                && decoded.document.automationLanes.size() == 40U
                && decoded.document.automationLanes[0].points.size()
                    == 1'000U,
            decoded.error.c_str()
        );
        openMilliseconds.push_back(elapsed);
    }

    const double serializeP50 = percentileMilliseconds(
        serializationMilliseconds,
        0.50
    );
    const double serializeP95 = percentileMilliseconds(
        serializationMilliseconds,
        0.95
    );
    const double serializeP99 = percentileMilliseconds(
        serializationMilliseconds,
        0.99
    );
    const double openP50 = percentileMilliseconds(
        openMilliseconds,
        0.50
    );
    const double openP95 = percentileMilliseconds(
        openMilliseconds,
        0.95
    );
    const double openP99 = percentileMilliseconds(
        openMilliseconds,
        0.99
    );
    const double serializeMaximum = *std::max_element(
        serializationMilliseconds.begin(),
        serializationMilliseconds.end()
    );
    const double openMaximum = *std::max_element(
        openMilliseconds.begin(),
        openMilliseconds.end()
    );
    require(
        openP99 < 5'000.0,
        "reference project open p99 stays below five seconds"
    );

    auto truncated = serialized;
    truncated.pop_back();
    require(
        !iramix::persistence::deserializeSessionDocument(
            truncated
        ).ok(),
        "truncated schema v3 project is rejected"
    );

    std::error_code fileSizeError;
    const auto projectBytes =
        std::filesystem::file_size(project, fileSizeError);
    require(!fileSizeError, fileSizeError.message().c_str());
    std::cout
        << "Reference project: tracks=200, clips=2000, routes=199, "
           "automation_lanes=40, automation_points=40000, "
           "project_bytes=" << projectBytes
        << ", iterations=" << benchmarkIterations
        << ", serialize_p50_ms=" << serializeP50
        << ", serialize_p95_ms=" << serializeP95
        << ", serialize_p99_ms=" << serializeP99
        << ", serialize_max_ms=" << serializeMaximum
        << ", open_p50_ms=" << openP50
        << ", open_p95_ms=" << openP95
        << ", open_p99_ms=" << openP99
        << ", open_max_ms=" << openMaximum
        << ", truncated_projects_rejected=1\n";
}

void testAsyncSessionSaver(
    const std::filesystem::path& root
) {
    const auto project = root / "async-session.irpx";
    std::string error;
    auto saver =
        iramix::persistence::AsyncSessionSaver::create(
            project,
            3U,
            error
        );
    require(saver != nullptr, error.c_str());

    iramix::persistence::SessionDocument invalid;
    invalid.revision = 1U;
    invalid.tracks.push_back({
        .stableId = 0U,
        .type = iramix::persistence::SessionTrackType::audio,
        .gain = 1.0F,
        .color = 0U,
        .name = "Invalid zero ID",
    });
    auto invalidSnapshot =
        std::make_shared<const iramix::persistence::SessionDocument>(
            std::move(invalid)
        );

    iramix::persistence::SessionDocument failed;
    failed.revision = 2U;
    failed.tracks.push_back({
        .stableId = 1U,
        .type = iramix::persistence::SessionTrackType::audio,
        .gain = 1.0F,
        .color = 0U,
        .name = "Injected failure",
    });
    auto failedSnapshot =
        std::make_shared<const iramix::persistence::SessionDocument>(
            std::move(failed)
        );

    iramix::persistence::SessionDocument committed;
    committed.revision = 3U;
    committed.tracks.push_back({
        .stableId = 1U,
        .type = iramix::persistence::SessionTrackType::audio,
        .gain = 0.75F,
        .color = 0xFF44'6688U,
        .name = "Committed session",
    });
    auto committedSnapshot =
        std::make_shared<const iramix::persistence::SessionDocument>(
            std::move(committed)
        );

    require(
        saver->trySubmit(1U, invalidSnapshot)
            == iramix::persistence::ProjectSaveSubmitResult::accepted,
        "invalid session is accepted for background validation"
    );
    require(
        saver->trySubmit(
            2U,
            failedSnapshot,
            iramix::persistence::AtomicSaveFailurePoint::
                afterStagingFlush
        ) == iramix::persistence::ProjectSaveSubmitResult::accepted,
        "failure-injected session is accepted"
    );
    require(
        saver->trySubmit(3U, committedSnapshot)
            == iramix::persistence::ProjectSaveSubmitResult::accepted,
        "valid session is accepted"
    );
    require(
        saver->trySubmit(4U, committedSnapshot)
            == iramix::persistence::ProjectSaveSubmitResult::
                invalidRevision,
        "submission revision must match immutable snapshot revision"
    );

    auto fourth = std::make_shared<iramix::persistence::SessionDocument>(
        *committedSnapshot
    );
    fourth->revision = 4U;
    require(
        saver->trySubmit(4U, fourth)
            == iramix::persistence::ProjectSaveSubmitResult::full,
        "session pipeline reports explicit saturation"
    );
    require(saver->start(error), error.c_str());
    saver->stop();

    iramix::persistence::SessionSaveCompletion completion;
    require(
        saver->tryPopCompletion(completion)
            && completion.revision == 1U
            && completion.status
                == iramix::persistence::
                    ProjectSaveCompletionStatus::failed
            && completion.serializedBytes == 0U
            && completion.detail[0] != '\0',
        "background serialization failure produces revisioned reject"
    );
    require(
        saver->tryPopCompletion(completion)
            && completion.revision == 2U
            && completion.status
                == iramix::persistence::
                    ProjectSaveCompletionStatus::failed
            && completion.serializedBytes != 0U
            && completion.durableSaveNanoseconds != 0U
            && completion.detail[0] != '\0',
        "durable write failure produces revisioned reject"
    );
    require(
        saver->tryPopCompletion(completion)
            && completion.revision == 3U
            && completion.status
                == iramix::persistence::
                    ProjectSaveCompletionStatus::committed
            && completion.serializedBytes != 0U
            && completion.serializationNanoseconds != 0U
            && completion.durableSaveNanoseconds != 0U
            && completion.detail[0] == '\0',
        "successful background serialization and save produce ACK"
    );
    require(
        saver->outstandingCount() == 0U
            && saver->acceptedCount() == 3U
            && saver->rejectedCount() == 2U,
        "session save accounting closes exactly"
    );

    auto loaded = iramix::persistence::loadProjectSnapshot(project);
    require(loaded.ok, loaded.error.c_str());
    const auto decoded =
        iramix::persistence::deserializeSessionDocument(
            loaded.payload
        );
    require(
        decoded.ok()
            && decoded.document.revision == 3U
            && decoded.document.tracks[0].name
                == "Committed session",
        "latest successful session revision is durable"
    );

    constexpr std::uint64_t revisionCount = 20U;
    const auto loadProject = root / "async-session-load.irpx";
    auto loadSaver =
        iramix::persistence::AsyncSessionSaver::create(
            loadProject,
            4U,
            error
        );
    require(loadSaver != nullptr, error.c_str());
    require(loadSaver->start(error), error.c_str());

    const auto reference = makeReferenceSession();
    std::vector<double> submitMilliseconds;
    std::vector<double> serializationMilliseconds;
    std::vector<double> durableSaveMilliseconds;
    submitMilliseconds.reserve(revisionCount);
    serializationMilliseconds.reserve(revisionCount);
    durableSaveMilliseconds.reserve(revisionCount);
    std::uint64_t committedCompletions = 0U;
    std::uint64_t nextCompletionRevision = 1U;

    const auto consumeCompletions = [&] {
        while (loadSaver->tryPopCompletion(completion)) {
            require(
                completion.status
                    == iramix::persistence::
                        ProjectSaveCompletionStatus::committed,
                completion.detail.data()
            );
            require(
                completion.revision == nextCompletionRevision,
                "session save completions preserve submission order"
            );
            require(
                completion.serializedBytes > 600'000U,
                "reference session serialization reports payload bytes"
            );
            serializationMilliseconds.push_back(
                static_cast<double>(
                    completion.serializationNanoseconds
                ) / 1'000'000.0
            );
            durableSaveMilliseconds.push_back(
                static_cast<double>(
                    completion.durableSaveNanoseconds
                ) / 1'000'000.0
            );
            ++nextCompletionRevision;
            ++committedCompletions;
        }
    };

    for (
        std::uint64_t revision = 1U;
        revision <= revisionCount;
        ++revision
    ) {
        auto mutableSnapshot =
            std::make_shared<iramix::persistence::SessionDocument>(
                reference
            );
        mutableSnapshot->revision = revision;
        const iramix::persistence::ImmutableSessionSnapshot snapshot =
            std::move(mutableSnapshot);
        for (;;) {
            const auto started =
                std::chrono::steady_clock::now();
            const auto result =
                loadSaver->trySubmit(revision, snapshot);
            const double elapsed =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started
                ).count();
            if (result
                == iramix::persistence::
                    ProjectSaveSubmitResult::accepted) {
                submitMilliseconds.push_back(elapsed);
                break;
            }
            require(
                result
                    == iramix::persistence::
                        ProjectSaveSubmitResult::full,
                "reference session submission only backpressures"
            );
            consumeCompletions();
            std::this_thread::yield();
        }
        consumeCompletions();
    }
    require(
        waitUntil([&] {
            consumeCompletions();
            return committedCompletions == revisionCount;
        }),
        "all background session save completions arrive"
    );
    loadSaver->stop();

    const double submitP50 = percentileMilliseconds(
        submitMilliseconds,
        0.50
    );
    const double submitP95 = percentileMilliseconds(
        submitMilliseconds,
        0.95
    );
    const double submitP99 = percentileMilliseconds(
        submitMilliseconds,
        0.99
    );
    const double serializeP50 = percentileMilliseconds(
        serializationMilliseconds,
        0.50
    );
    const double serializeP95 = percentileMilliseconds(
        serializationMilliseconds,
        0.95
    );
    const double serializeP99 = percentileMilliseconds(
        serializationMilliseconds,
        0.99
    );
    const double saveP50 = percentileMilliseconds(
        durableSaveMilliseconds,
        0.50
    );
    const double saveP95 = percentileMilliseconds(
        durableSaveMilliseconds,
        0.95
    );
    const double saveP99 = percentileMilliseconds(
        durableSaveMilliseconds,
        0.99
    );
    require(
        submitP99 < 16.0,
        "immutable snapshot handoff p99 stays below UI stall budget"
    );

    loaded = iramix::persistence::loadProjectSnapshot(loadProject);
    require(loaded.ok, loaded.error.c_str());
    const auto finalDecoded =
        iramix::persistence::deserializeSessionDocument(
            loaded.payload
        );
    require(
        finalDecoded.ok()
            && finalDecoded.document.revision == revisionCount,
        "background worker durably commits the latest revision"
    );

    std::cout
        << "Async session saver: revisions=" << revisionCount
        << ", reference_tracks=200, reference_clips=2000, "
           "reference_automation_points=40000"
        << ", submit_p50_ms=" << submitP50
        << ", submit_p95_ms=" << submitP95
        << ", submit_p99_ms=" << submitP99
        << ", serialize_worker_p50_ms=" << serializeP50
        << ", serialize_worker_p95_ms=" << serializeP95
        << ", serialize_worker_p99_ms=" << serializeP99
        << ", durable_save_worker_p50_ms=" << saveP50
        << ", durable_save_worker_p95_ms=" << saveP95
        << ", durable_save_worker_p99_ms=" << saveP99
        << ", ordered_completions=" << committedCompletions
        << ", serialization_failures=1, injected_save_failures=1\n";
}

void testCommandJournal(const std::filesystem::path& root) {
    const auto path = root / "commands.irjc";
    iramix::persistence::CommandJournal journal {path};
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    require(
        journal.append(1U, bytesOf("add-track"), error),
        error.c_str()
    );
    require(
        journal.append(2U, bytesOf("move-clip"), error),
        error.c_str()
    );
    const auto durableMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start
        ).count();
    require(
        durableMilliseconds < 5'000,
        "journal durable ACK window"
    );
    require(
        !journal.append(2U, bytesOf("duplicate"), error),
        "duplicate journal sequence is rejected"
    );

    {
        std::ofstream tail {
            path,
            std::ios::binary | std::ios::app,
        };
        const std::array<char, 7> partial {
            'I', 'R', 'J', 'C', '\x01', '\0', '\0',
        };
        tail.write(partial.data(), partial.size());
    }
    auto recovered =
        iramix::persistence::recoverCommandJournal(path);
    require(recovered.ok(), recovered.error.c_str());
    require(
        recovered.commands.size() == 2U
            && recovered.discardedInvalidTail,
        "journal discards partial tail"
    );

    iramix::persistence::CommandJournal reopened {path};
    require(
        reopened.append(3U, bytesOf("set-gain"), error),
        error.c_str()
    );
    recovered = iramix::persistence::recoverCommandJournal(path);
    require(recovered.ok(), recovered.error.c_str());
    require(
        recovered.commands.size() == 3U
            && !recovered.discardedInvalidTail
            && recovered.commands.back().sequence == 3U,
        "journal truncates invalid tail before next append"
    );

    std::cout
        << "Command journal: commands=3, repaired_tails=1, "
        << "two_command_durable_ms=" << durableMilliseconds
        << '\n';
}

[[nodiscard]] int runRecordingCrashChild(
    const std::filesystem::path& path
) {
    std::string error;
    auto writer =
        iramix::persistence::RecoverableRecordingWriter::create(
            path,
            {.sampleRate = 48'000U, .channelCount = 2U},
            error
        );
    if (writer == nullptr) {
        return 10;
    }
    const std::array<float, 4> first {
        0.1F, -0.1F, 0.2F, -0.2F,
    };
    const std::array<float, 4> second {
        0.3F, -0.3F, 0.4F, -0.4F,
    };
    if (!writer->appendInterleavedBlock(first, 2U, error)
        || !writer->flush(error)
        || !writer->appendInterleavedBlock(second, 2U, error)
        || !writer->flush(error)) {
        return 11;
    }
    writer.reset();

    std::ofstream tail {
        path,
        std::ios::binary | std::ios::app,
    };
    const std::array<char, 7> partial {
        'B', 'L', 'K', '1', '\x03', '\0', '\0',
    };
    tail.write(partial.data(), partial.size());
    tail.flush();
    std::_Exit(77);
}

[[nodiscard]] int launchRecordingCrashChild(
    const std::filesystem::path& executable,
    const std::filesystem::path& recording
) {
#if defined(_WIN32)
    std::wstring commandLine =
        L"\"" + executable.wstring()
        + L"\" --recording-crash-child \""
        + recording.wstring() + L"\"";
    std::vector<wchar_t> mutableCommand {
        commandLine.begin(),
        commandLine.end(),
    };
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    if (!CreateProcessW(
            executable.c_str(),
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process
        )) {
        return -1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 0U;
    const bool readExitCode =
        GetExitCodeProcess(process.hProcess, &exitCode) != FALSE;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return readExitCode ? static_cast<int>(exitCode) : -1;
#else
    const pid_t child = ::fork();
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        ::execl(
            executable.c_str(),
            executable.c_str(),
            "--recording-crash-child",
            recording.c_str(),
            static_cast<char*>(nullptr)
        );
        std::_Exit(126);
    }
    int status = 0;
    if (::waitpid(child, &status, 0) != child) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

void testRecoverableRecording(
    const std::filesystem::path& executable,
    const std::filesystem::path& root
) {
    const auto recording = root / "take-001.irrc";
    const int childStatus = launchRecordingCrashChild(
        executable,
        recording
    );
    require(
        childStatus == 77,
        "recording child exits at the injected crash point"
    );

    auto recovered =
        iramix::persistence::recoverRecording(recording);
    require(recovered.ok(), recovered.error.c_str());
    require(
        recovered.format.sampleRate == 48'000U
            && recovered.format.channelCount == 2U,
        "recording format recovers"
    );
    require(
        recovered.blockCount == 2U
            && recovered.frameCount == 4U
            && recovered.interleavedSamples.size() == 8U
            && recovered.discardedInvalidTail,
        "recording keeps flushed blocks and discards crash tail"
    );
    const auto scan =
        iramix::persistence::scanRecording(recording);
    require(scan.ok(), scan.error.c_str());
    require(
        scan.blockCount == 2U
            && scan.frameCount == 4U
            && scan.validBytes == 96U
            && scan.fileBytes == 103U
            && scan.streamingBufferBytes == 64U * 1024U
            && scan.discardedInvalidTail,
        "streaming scan finds the longest valid recording prefix"
    );

    const auto corrupt = root / "take-corrupt.irrc";
    std::filesystem::copy_file(recording, corrupt);
    {
        std::fstream file {
            corrupt,
            std::ios::binary | std::ios::in | std::ios::out,
        };
        constexpr std::streamoff secondPayloadLastByte = 95;
        file.seekg(secondPayloadLastByte);
        char value = 0;
        file.read(&value, 1);
        value ^= 0x01;
        file.seekp(secondPayloadLastByte);
        file.write(&value, 1);
    }
    recovered = iramix::persistence::recoverRecording(corrupt);
    require(recovered.ok(), recovered.error.c_str());
    require(
        recovered.blockCount == 1U
            && recovered.frameCount == 2U
            && recovered.discardedInvalidTail,
        "recording checksum rejects corrupt block and suffix"
    );
    std::string error;
    require(
        iramix::persistence::repairRecordingTail(
            recording,
            scan,
            error
        ),
        error.c_str()
    );
    const auto repairedScan =
        iramix::persistence::scanRecording(recording);
    require(
        repairedScan.ok()
            && repairedScan.fileBytes == 96U
            && repairedScan.validBytes == 96U
            && !repairedScan.discardedInvalidTail,
        "recording repair truncates only the invalid suffix"
    );

    std::cout
        << "Recoverable recording: forced_exit=77, "
           "flushed_blocks=2, recovered_frames=4, "
           "partial_tails_discarded=1, corrupt_blocks_rejected=1, "
           "stream_scan_buffer_bytes="
        << scan.streamingBufferBytes
        << ", repaired_bytes=7\n";
}

void testDiskAudioWorkers(const std::filesystem::path& root) {
    const auto recording = root / "worker-take.irrc";
    std::string error;
    auto recorder =
        iramix::persistence::RecordingDiskWorker::create(
            recording,
            {
                .format = {
                    .sampleRate = 48'000U,
                    .channelCount = 2U,
                },
                .maximumFramesPerBlock = 2U,
                .queueBlockCapacity = 2U,
                .durableFlushEveryBlocks = 2U,
            },
            error
        );
    require(recorder != nullptr, error.c_str());
    const std::array<float, 4> first {
        0.1F, -0.1F, 0.2F, -0.2F,
    };
    const std::array<float, 4> second {
        0.3F, -0.3F, 0.4F, -0.4F,
    };

    iramix::realtime::resetAuditCounters();
    bool firstAccepted = false;
    bool secondAccepted = false;
    bool saturationRejected = false;
    {
        iramix::realtime::CallbackScope callback;
        firstAccepted = recorder->tryEnqueue(first, 2U);
        secondAccepted = recorder->tryEnqueue(second, 2U);
        saturationRejected = !recorder->tryEnqueue(first, 2U);
    }
    const auto recordingAudit =
        iramix::realtime::auditSnapshot();
    require(
        firstAccepted && secondAccepted && saturationRejected,
        "recording queue accepts capacity and reports saturation"
    );
    require(
        recordingAudit.allocations == 0U
            && recordingAudit.deallocations == 0U
            && recordingAudit.blockingLocks == 0U,
        "recording callback path has zero allocation and blocking locks"
    );
    require(recorder->start(error), error.c_str());
    recorder->stop();
    require(!recorder->failed(), recorder->lastError().c_str());
    require(
        recorder->acceptedBlocks() == 2U
            && recorder->rejectedBlocks() == 1U
            && recorder->writtenBlocks() == 2U
            && recorder->bufferedBlocks() == 0U
            && recorder->queueStorageBytes() == 40U,
        "recording worker drains a fixed-capacity queue"
    );

    const auto scan =
        iramix::persistence::scanRecording(recording);
    std::cout
        << "Recording worker scan: ok=" << scan.ok()
        << ", blocks=" << scan.blockCount
        << ", frames=" << scan.frameCount
        << ", valid_bytes=" << scan.validBytes
        << ", file_bytes=" << scan.fileBytes
        << ", invalid_tail=" << scan.discardedInvalidTail
        << ", error=" << scan.error << '\n';
    require(
        scan.ok()
            && scan.blockCount == 2U
            && scan.frameCount == 4U
            && !scan.discardedInvalidTail,
        "recording worker produces a valid recoverable stream"
    );

    auto readAhead =
        iramix::persistence::RecordingReadAhead::create(
            recording,
            {
                .maximumFramesPerBlock = 2U,
                .queueBlockCapacity = 2U,
            },
            error
        );
    require(readAhead != nullptr, error.c_str());
    std::array<float, 4> output {
        1.0F, 1.0F, 1.0F, 1.0F,
    };
    std::uint32_t outputFrames = 0U;

    iramix::realtime::resetAuditCounters();
    bool initialUnderflow = false;
    {
        iramix::realtime::CallbackScope callback;
        initialUnderflow = !readAhead->tryDequeue(
            output,
            outputFrames
        );
    }
    auto playbackAudit = iramix::realtime::auditSnapshot();
    require(
        initialUnderflow
            && outputFrames == 2U
            && std::all_of(
                output.begin(),
                output.end(),
                [](const float value) { return value == 0.0F; }
            ),
        "read-ahead underflow emits deterministic silence"
    );
    require(
        playbackAudit.allocations == 0U
            && playbackAudit.deallocations == 0U
            && playbackAudit.blockingLocks == 0U,
        "read-ahead underflow path is callback-safe"
    );

    require(readAhead->start(error), error.c_str());
    require(
        waitUntil([&readAhead] {
            return readAhead->bufferedBlocks() == 2U;
        }),
        "read-ahead worker pre-fills its bounded queue"
    );

    std::array<float, 4> firstOutput {};
    std::array<float, 4> secondOutput {};
    std::array<float, 4> underflowOutput {
        1.0F, 1.0F, 1.0F, 1.0F,
    };
    std::uint32_t firstFrames = 0U;
    std::uint32_t secondFrames = 0U;
    std::uint32_t underflowFrames = 0U;
    bool firstDelivered = false;
    bool secondDelivered = false;
    bool finalUnderflow = false;
    iramix::realtime::resetAuditCounters();
    {
        iramix::realtime::CallbackScope callback;
        firstDelivered = readAhead->tryDequeue(
            firstOutput,
            firstFrames
        );
        secondDelivered = readAhead->tryDequeue(
            secondOutput,
            secondFrames
        );
        finalUnderflow = !readAhead->tryDequeue(
            underflowOutput,
            underflowFrames
        );
    }
    playbackAudit = iramix::realtime::auditSnapshot();
    require(
        firstDelivered
            && secondDelivered
            && finalUnderflow
            && firstFrames == 2U
            && secondFrames == 2U
            && firstOutput == first
            && secondOutput == second,
        "read-ahead callback receives blocks in order"
    );
    require(
        playbackAudit.allocations == 0U
            && playbackAudit.deallocations == 0U
            && playbackAudit.blockingLocks == 0U,
        "read-ahead delivery path is callback-safe"
    );
    require(
        waitUntil([&readAhead] {
            return readAhead->reachedEnd();
        }),
        "read-ahead worker reaches the validated prefix end"
    );
    readAhead->stop();
    require(!readAhead->failed(), readAhead->lastError().c_str());
    require(
        readAhead->deliveredBlocks() == 2U
            && readAhead->underflowCount() == 2U
            && readAhead->queueStorageBytes() == 40U,
        "read-ahead counters expose delivery and pressure"
    );

    std::cout
        << "Disk audio workers: recording_queue_blocks=2, "
           "recording_rejected=1, recording_written=2, "
           "read_ahead_blocks=2, playback_underflows=2, "
           "queue_bytes_each=40, callback_allocations="
        << recordingAudit.allocations + playbackAudit.allocations
        << ", callback_blocking_locks="
        << recordingAudit.blockingLocks
            + playbackAudit.blockingLocks
        << '\n';
}

void testBoundedStreamingScan(
    const std::filesystem::path& root
) {
    constexpr std::uint32_t blockCount = 2'048U;
    constexpr std::uint32_t framesPerBlock = 256U;
    constexpr std::uint32_t channelCount = 2U;
    const auto recording = root / "streaming-scan.irrc";
    std::string error;
    auto writer =
        iramix::persistence::RecoverableRecordingWriter::create(
            recording,
            {
                .sampleRate = 48'000U,
                .channelCount = channelCount,
            },
            error
        );
    require(writer != nullptr, error.c_str());
    std::array<
        float,
        static_cast<std::size_t>(framesPerBlock) * channelCount
    > block {};
    for (std::size_t index = 0U; index < block.size(); ++index) {
        block[index] = static_cast<float>(index % 31U) / 31.0F;
    }
    for (std::uint32_t index = 0U; index < blockCount; ++index) {
        require(
            writer->appendInterleavedBlock(
                block,
                framesPerBlock,
                error
            ),
            error.c_str()
        );
    }
    require(writer->flush(error), error.c_str());
    writer.reset();

    const auto scan =
        iramix::persistence::scanRecording(recording);
    require(scan.ok(), scan.error.c_str());
    require(
        scan.blockCount == blockCount
            && scan.frameCount
                == static_cast<std::uint64_t>(blockCount)
                    * framesPerBlock
            && scan.maximumFramesPerBlock == framesPerBlock
            && scan.validBytes == scan.fileBytes
            && scan.fileBytes
                > scan.streamingBufferBytes * 64U
            && !scan.discardedInvalidTail,
        "streaming scan memory stays fixed as the recording grows"
    );

    std::cout
        << "Bounded streaming scan: file_bytes=" << scan.fileBytes
        << ", blocks=" << scan.blockCount
        << ", frames=" << scan.frameCount
        << ", scratch_bytes=" << scan.streamingBufferBytes
        << ", materialized_samples=0\n";
}

} // namespace

int main(const int argc, char* argv[]) {
    if (argc == 3
        && std::string_view {argv[1]}
            == "--recording-crash-child") {
        return runRecordingCrashChild(argv[2]);
    }

    TemporaryDirectory temporary;
    testAtomicProjectStore(temporary.path());
    testProjectBackupRotation(temporary.path());
    testSessionDocumentRoundTrip(temporary.path());
    testAsyncProjectSaver(temporary.path());
    testReferenceProjectBenchmark(temporary.path());
    testAsyncSessionSaver(temporary.path());
    testCommandJournal(temporary.path());
    testRecoverableRecording(
        std::filesystem::absolute(argv[0]),
        temporary.path()
    );
    testDiskAudioWorkers(temporary.path());
    testBoundedStreamingScan(temporary.path());
    std::cout << "All Iramix persistence tests passed.\n";
    return EXIT_SUCCESS;
}
