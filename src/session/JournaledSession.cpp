#include "iramix/session/JournaledSession.hpp"

#include "iramix/persistence/ProjectStore.hpp"
#include "iramix/persistence/SessionDocument.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <span>
#include <utility>

namespace iramix::session {
namespace {

constexpr std::array<std::byte, 4> kCommandMagic {
    std::byte {'I'},
    std::byte {'S'},
    std::byte {'C'},
    std::byte {'1'},
};
constexpr std::uint32_t kCommandSchemaVersion = 1U;
constexpr std::uint32_t kCheckpointBaselineAction = 4U;
constexpr std::size_t kMaximumCommandNameBytes = 1'024U;

void appendU32(
    std::vector<std::byte>& bytes,
    const std::uint32_t value
) {
    for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
        bytes.push_back(
            static_cast<std::byte>((value >> shift) & 0xFFU)
        );
    }
}

void appendU64(
    std::vector<std::byte>& bytes,
    const std::uint64_t value
) {
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        bytes.push_back(
            static_cast<std::byte>((value >> shift) & 0xFFU)
        );
    }
}

[[nodiscard]] bool readU32(
    const std::span<const std::byte> bytes,
    std::size_t& offset,
    std::uint32_t& value
) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 4U) {
        return false;
    }
    value = 0U;
    for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(
            std::to_integer<unsigned int>(bytes[offset++])
        ) << shift;
    }
    return true;
}

[[nodiscard]] bool readU64(
    const std::span<const std::byte> bytes,
    std::size_t& offset,
    std::uint64_t& value
) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 8U) {
        return false;
    }
    value = 0U;
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        value |= static_cast<std::uint64_t>(
            std::to_integer<unsigned int>(bytes[offset++])
        ) << shift;
    }
    return true;
}

void appendCommand(
    std::vector<std::byte>& bytes,
    const SessionCommand& command
) {
    appendU32(bytes, static_cast<std::uint32_t>(command.type));
    appendU64(bytes, command.entityId);
    appendU32(
        bytes,
        static_cast<std::uint32_t>(command.trackType)
    );
    appendU32(bytes, command.color);
    appendU64(bytes, std::bit_cast<std::uint64_t>(command.tempo));
    appendU32(
        bytes,
        static_cast<std::uint32_t>(command.name.size())
    );
    const auto* const begin = reinterpret_cast<const std::byte*>(
        command.name.data()
    );
    bytes.insert(
        bytes.end(),
        begin,
        begin + static_cast<std::ptrdiff_t>(command.name.size())
    );
}

[[nodiscard]] bool readCommand(
    const std::span<const std::byte> bytes,
    std::size_t& offset,
    SessionCommand& command
) {
    std::uint32_t type = 0U;
    std::uint32_t trackType = 0U;
    std::uint64_t tempoBits = 0U;
    std::uint32_t nameSize = 0U;
    if (!readU32(bytes, offset, type)
        || !readU64(bytes, offset, command.entityId)
        || !readU32(bytes, offset, trackType)
        || !readU32(bytes, offset, command.color)
        || !readU64(bytes, offset, tempoBits)
        || !readU32(bytes, offset, nameSize)
        || nameSize > kMaximumCommandNameBytes
        || offset > bytes.size()
        || bytes.size() - offset < nameSize
        || type < static_cast<std::uint32_t>(
            SessionCommandType::setTempo
        )
        || type > static_cast<std::uint32_t>(
            SessionCommandType::removeTrack
        )
        || trackType < static_cast<std::uint32_t>(
            persistence::SessionTrackType::audio
        )
        || trackType > static_cast<std::uint32_t>(
            persistence::SessionTrackType::master
        )) {
        return false;
    }
    command.type = static_cast<SessionCommandType>(type);
    command.trackType =
        static_cast<persistence::SessionTrackType>(trackType);
    command.tempo = std::bit_cast<double>(tempoBits);
    command.name.assign(
        reinterpret_cast<const char*>(
            bytes.data() + static_cast<std::ptrdiff_t>(offset)
        ),
        nameSize
    );
    offset += nameSize;
    return true;
}

[[nodiscard]] std::vector<std::byte> encodeRecord(
    const std::uint32_t action,
    const SessionCommand& forward,
    const SessionCommand& inverse
) {
    std::vector<std::byte> bytes;
    bytes.reserve(
        72U + forward.name.size() + inverse.name.size()
    );
    bytes.insert(bytes.end(), kCommandMagic.begin(), kCommandMagic.end());
    appendU32(bytes, kCommandSchemaVersion);
    appendU32(bytes, action);
    appendCommand(bytes, forward);
    appendCommand(bytes, inverse);
    return bytes;
}

[[nodiscard]] std::vector<std::byte> encodeCheckpointBaseline(
    const std::uint64_t revision
) {
    std::vector<std::byte> bytes;
    bytes.reserve(20U);
    bytes.insert(bytes.end(), kCommandMagic.begin(), kCommandMagic.end());
    appendU32(bytes, kCommandSchemaVersion);
    appendU32(
        bytes,
        kCheckpointBaselineAction
    );
    appendU64(bytes, revision);
    return bytes;
}

[[nodiscard]] bool decodeRecord(
    const std::span<const std::byte> bytes,
    std::uint32_t& action,
    std::uint64_t& checkpointBaseline,
    SessionCommand& forward,
    SessionCommand& inverse
) {
    if (bytes.size() < kCommandMagic.size()
        || !std::equal(
            kCommandMagic.begin(),
            kCommandMagic.end(),
            bytes.begin()
        )) {
        return false;
    }
    std::size_t offset = kCommandMagic.size();
    std::uint32_t version = 0U;
    checkpointBaseline = 0U;
    if (!readU32(bytes, offset, version)
        || version != kCommandSchemaVersion
        || !readU32(bytes, offset, action)
        || action < 1U
        || action > 4U) {
        return false;
    }
    if (action == kCheckpointBaselineAction) {
        return readU64(bytes, offset, checkpointBaseline)
            && checkpointBaseline != 0U
            && offset == bytes.size();
    }
    return readCommand(bytes, offset, forward)
        && readCommand(bytes, offset, inverse)
        && offset == bytes.size();
}

[[nodiscard]] bool commandEquals(
    const SessionCommand& left,
    const SessionCommand& right
) noexcept {
    return left.type == right.type
        && left.entityId == right.entityId
        && left.trackType == right.trackType
        && left.color == right.color
        && left.tempo == right.tempo
        && left.name == right.name;
}

[[nodiscard]] JournaledEditStatus mapStatus(
    const SessionEditStatus status
) noexcept {
    switch (status) {
    case SessionEditStatus::applied:
        return JournaledEditStatus::applied;
    case SessionEditStatus::revisionConflict:
        return JournaledEditStatus::revisionConflict;
    case SessionEditStatus::invalidArgument:
        return JournaledEditStatus::invalidArgument;
    case SessionEditStatus::entityNotFound:
        return JournaledEditStatus::entityNotFound;
    case SessionEditStatus::allocationFailure:
        return JournaledEditStatus::allocationFailure;
    }
    return JournaledEditStatus::invalidArgument;
}

[[nodiscard]] SessionEditResult applyCommand(
    SessionController& controller,
    const std::uint64_t expectedRevision,
    const SessionCommand& command
) noexcept {
    switch (command.type) {
    case SessionCommandType::setTempo:
        return controller.setTempo(expectedRevision, command.tempo);
    case SessionCommandType::addTrack:
        return controller.addTrackWithStableId(
            expectedRevision,
            command.entityId,
            command.trackType,
            command.name,
            command.color
        );
    case SessionCommandType::renameTrack:
        return controller.renameTrack(
            expectedRevision,
            command.entityId,
            command.name
        );
    case SessionCommandType::removeTrack:
        return controller.removeTrack(
            expectedRevision,
            command.entityId
        );
    }
    return {
        .status = SessionEditStatus::invalidArgument,
        .revision = controller.currentRevision(),
    };
}

[[nodiscard]] const persistence::SessionTrack* findTrack(
    const persistence::SessionDocument& document,
    const std::uint64_t stableId
) noexcept {
    const auto track = std::find_if(
        document.tracks.begin(),
        document.tracks.end(),
        [stableId](const persistence::SessionTrack& candidate) {
            return candidate.stableId == stableId;
        }
    );
    return track == document.tracks.end() ? nullptr : &*track;
}

struct JournalRestoreRequirement final {
    std::uint64_t minimumSnapshotRevision {0U};
    std::uint64_t maximumSnapshotRevision {0U};
    std::string error;
    bool hasCommands {false};
    bool hasExplicitBaseline {false};
    bool ok {false};
};

[[nodiscard]] JournalRestoreRequirement inspectJournalForRestore(
    const std::filesystem::path& journalPath
) {
    JournalRestoreRequirement result;
    const auto recovered =
        persistence::recoverCommandJournal(journalPath);
    if (!recovered.ok()) {
        result.error = recovered.error;
        return result;
    }
    if (recovered.commands.empty()) {
        result.ok = true;
        return result;
    }
    result.hasCommands = true;
    result.maximumSnapshotRevision =
        recovered.commands.back().sequence;

    std::uint32_t action = 0U;
    std::uint64_t checkpointBaseline = 0U;
    SessionCommand forward;
    SessionCommand inverse;
    if (!decodeRecord(
            recovered.commands.front().payload,
            action,
            checkpointBaseline,
            forward,
            inverse
        )) {
        result.error =
            "cannot inspect first session command for backup restore";
        return result;
    }
    if (action == kCheckpointBaselineAction) {
        if (recovered.commands.front().sequence
                > checkpointBaseline
            || recovered.commands.back().sequence
                < checkpointBaseline) {
            result.error = "invalid journal checkpoint baseline";
            return result;
        }
        result.minimumSnapshotRevision = checkpointBaseline;
        result.hasExplicitBaseline = true;
    } else {
        // A legacy journal may already be a compacted synthetic history.
        // Without an explicit baseline only an equally-new snapshot is safe.
        result.minimumSnapshotRevision =
            recovered.commands.back().sequence;
    }
    result.ok = true;
    return result;
}

} // namespace

JournaledSession::JournaledSession(
    std::unique_ptr<SessionController> controller,
    std::unique_ptr<persistence::CommandJournal> journal,
    std::filesystem::path journalPath,
    const std::uint64_t snapshotRevision
)
    : controller_ {std::move(controller)},
      journal_ {std::move(journal)},
      journalPath_ {std::move(journalPath)},
      snapshotRevision_ {snapshotRevision} {}

std::filesystem::path JournaledSession::journalPathForProject(
    const std::filesystem::path& projectTarget
) {
    auto result = projectTarget;
    result += ".commands.irjc";
    return result;
}

std::unique_ptr<JournaledSession> JournaledSession::open(
    const std::filesystem::path& projectTarget,
    std::string& error
) {
    error.clear();
    if (projectTarget.empty()) {
        error = "journaled session requires a project target";
        return {};
    }

    persistence::SessionDocument document;
    const bool snapshotExists =
        std::filesystem::exists(projectTarget)
        || std::filesystem::exists(
            persistence::projectStagingPath(projectTarget)
        );
    std::string primaryError;
    bool primaryLoaded = false;
    if (snapshotExists) {
        const auto loaded =
            persistence::loadProjectSnapshot(projectTarget);
        if (loaded.ok) {
            auto decoded =
                persistence::deserializeSessionDocument(loaded.payload);
            if (decoded.ok()) {
                document = std::move(decoded.document);
                primaryLoaded = true;
            } else {
                primaryError = std::move(decoded.error);
            }
        } else {
            primaryError = loaded.error;
        }
    }

    if (primaryLoaded) {
        const auto durableSnapshotRevision = document.revision;
        auto session = createFromDocument(
            std::move(document),
            journalPathForProject(projectTarget),
            error
        );
        if (session != nullptr) {
            session->snapshotRevision_ = durableSnapshotRevision;
            return session;
        }
        primaryError = error;
    }

    const auto backupDirectory =
        persistence::defaultProjectBackupDirectory(projectTarget);
    const auto backups =
        persistence::listProjectBackups(backupDirectory);
    if (!backups.ok) {
        error = backups.error;
        return {};
    }
    if (!snapshotExists && backups.entries.empty()) {
        const auto requirement = inspectJournalForRestore(
            journalPathForProject(projectTarget)
        );
        if (!requirement.ok) {
            error = "journal-only recovery rejected: "
                + requirement.error;
            return {};
        }
        if (requirement.hasCommands
            && (!requirement.hasExplicitBaseline
                || requirement.minimumSnapshotRevision > 1U)) {
            error =
                "journal-only recovery requires an explicit default "
                "session baseline";
            return {};
        }
        SessionController defaults;
        document = *defaults.snapshot();
        auto session = createFromDocument(
            std::move(document),
            journalPathForProject(projectTarget),
            error
        );
        if (session != nullptr) {
            session->snapshotRevision_ = 0U;
        }
        return session;
    }

    const auto journalPath = journalPathForProject(projectTarget);
    const auto requirement =
        inspectJournalForRestore(journalPath);
    if (!requirement.ok) {
        error = "backup restore rejected: " + requirement.error;
        return {};
    }

    std::uint64_t skipped = 0U;
    std::string lastBackupError;
    for (const auto& entry : backups.entries) {
        auto loaded = persistence::loadProjectSnapshot(entry.path);
        if (!loaded.ok) {
            ++skipped;
            lastBackupError = std::move(loaded.error);
            continue;
        }
        auto decoded =
            persistence::deserializeSessionDocument(loaded.payload);
        if (!decoded.ok()) {
            ++skipped;
            lastBackupError = std::move(decoded.error);
            continue;
        }
        if (decoded.document.revision != entry.revision) {
            ++skipped;
            lastBackupError =
                "backup filename revision does not match session revision";
            continue;
        }
        if (entry.revision
            < requirement.minimumSnapshotRevision) {
            ++skipped;
            lastBackupError =
                "backup predates the safe journal restore baseline";
            continue;
        }
        if (requirement.maximumSnapshotRevision != 0U
            && entry.revision
                > requirement.maximumSnapshotRevision) {
            ++skipped;
            lastBackupError =
                "backup exceeds the final valid journal sequence";
            continue;
        }

        std::string candidateError;
        auto candidate = createFromDocument(
            std::move(decoded.document),
            journalPath,
            candidateError
        );
        if (candidate == nullptr) {
            ++skipped;
            lastBackupError = std::move(candidateError);
            continue;
        }
        std::string restoreError;
        if (!persistence::saveProjectSnapshot(
                projectTarget,
                loaded.payload,
                restoreError
            )) {
            error = "validated backup could not restore active project: "
                + restoreError;
            return {};
        }
        candidate->snapshotRevision_ = entry.revision;
        candidate->recoveredFromBackup_ = true;
        candidate->recoveredBackupRevision_ = entry.revision;
        candidate->skippedBackupCount_ = skipped;
        return candidate;
    }

    error = snapshotExists
        ? "active project is unreadable: " + primaryError
        : "active project is missing";
    error += "; no safe project backup could be restored";
    if (!lastBackupError.empty()) {
        error += ": " + lastBackupError;
    }
    return {};
}

std::unique_ptr<JournaledSession>
JournaledSession::createFromDocument(
    persistence::SessionDocument document,
    std::filesystem::path journalPath,
    std::string& error
) {
    error.clear();
    const auto documentRevision = document.revision;
    auto controller = SessionController::fromDocument(
        std::move(document),
        error
    );
    if (controller == nullptr) {
        return {};
    }
    const auto recovered =
        persistence::recoverCommandJournal(journalPath);
    if (!recovered.ok()) {
        error = recovered.error;
        return {};
    }
    if (recovered.commands.empty()) {
        const persistence::JournalCommand baseline {
            .sequence = documentRevision,
            .payload = encodeCheckpointBaseline(documentRevision),
        };
        if (!persistence::rewriteCommandJournal(
                journalPath,
                std::span {&baseline, 1U},
                error
            )) {
            return {};
        }
    }
    try {
        auto session = std::unique_ptr<JournaledSession> {
            new JournaledSession {
                std::move(controller),
                std::make_unique<persistence::CommandJournal>(
                    journalPath
                ),
                journalPath,
                0U,
            }
        };
        if (!session->replay(journalPath, error)) {
            return {};
        }
        return session;
    } catch (const std::bad_alloc&) {
        error = "cannot allocate journaled session";
        return {};
    }
}

std::uint64_t JournaledSession::currentRevision() const noexcept {
    return controller_->currentRevision();
}

std::uint64_t JournaledSession::snapshotRevision() const noexcept {
    return snapshotRevision_;
}

std::size_t JournaledSession::trackCount() const noexcept {
    return controller_->trackCount();
}

std::size_t JournaledSession::undoDepth() const noexcept {
    return undo_.size();
}

std::size_t JournaledSession::redoDepth() const noexcept {
    return redo_.size();
}

bool JournaledSession::requiresReopen() const noexcept {
    return requiresReopen_;
}

std::uint64_t JournaledSession::checkpointRevision() const noexcept {
    return checkpointRevision_;
}

bool JournaledSession::recoveredFromBackup() const noexcept {
    return recoveredFromBackup_;
}

std::uint64_t
JournaledSession::recoveredBackupRevision() const noexcept {
    return recoveredBackupRevision_;
}

std::uint64_t JournaledSession::skippedBackupCount() const noexcept {
    return skippedBackupCount_;
}

persistence::ImmutableSessionSnapshot
JournaledSession::snapshot() const {
    return controller_->snapshot();
}

JournaledEditResult JournaledSession::setTempo(
    const std::uint64_t expectedRevision,
    const double tempo,
    std::string& error
) {
    SessionCommand command;
    command.type = SessionCommandType::setTempo;
    command.tempo = tempo;
    return commitEdit(expectedRevision, std::move(command), error);
}

JournaledEditResult JournaledSession::addTrack(
    const std::uint64_t expectedRevision,
    const persistence::SessionTrackType type,
    const std::string_view name,
    const std::uint32_t color,
    std::string& error
) {
    SessionCommand command;
    command.type = SessionCommandType::addTrack;
    command.trackType = type;
    command.color = color;
    try {
        command.name = name;
    } catch (const std::bad_alloc&) {
        error = "cannot allocate track command";
        return {
            .status = JournaledEditStatus::allocationFailure,
            .revision = currentRevision(),
        };
    }
    return commitEdit(expectedRevision, std::move(command), error);
}

JournaledEditResult JournaledSession::renameTrack(
    const std::uint64_t expectedRevision,
    const std::uint64_t trackId,
    const std::string_view name,
    std::string& error
) {
    SessionCommand command;
    command.type = SessionCommandType::renameTrack;
    command.entityId = trackId;
    try {
        command.name = name;
    } catch (const std::bad_alloc&) {
        error = "cannot allocate rename command";
        return {
            .status = JournaledEditStatus::allocationFailure,
            .revision = currentRevision(),
        };
    }
    return commitEdit(expectedRevision, std::move(command), error);
}

JournaledEditResult JournaledSession::undo(
    const std::uint64_t expectedRevision,
    std::string& error
) {
    return commitHistoryAction(
        expectedRevision,
        HistoryAction::undo,
        error
    );
}

JournaledEditResult JournaledSession::redo(
    const std::uint64_t expectedRevision,
    std::string& error
) {
    return commitHistoryAction(
        expectedRevision,
        HistoryAction::redo,
        error
    );
}

bool JournaledSession::checkpoint(
    const std::uint64_t durableRevision,
    std::string& error
) {
    error.clear();
    if (requiresReopen_) {
        error = "journal state is ambiguous; reopen required";
        return false;
    }
    if (durableRevision == checkpointRevision_) {
        return true;
    }
    if (durableRevision == 0U
        || durableRevision != currentRevision()) {
        error =
            "checkpoint requires the current revision to be durable";
        return false;
    }

    try {
        std::vector<HistoryEntry> linearHistory;
        linearHistory.reserve(undo_.size() + redo_.size());
        linearHistory.insert(
            linearHistory.end(),
            undo_.begin(),
            undo_.end()
        );
        for (auto entry = redo_.rbegin(); entry != redo_.rend(); ++entry) {
            linearHistory.push_back(*entry);
        }

        const auto recordCount =
            linearHistory.size() + redo_.size() + 1U;
        if (recordCount
            > static_cast<std::size_t>(durableRevision)) {
            error = "history cannot fit below checkpoint revision";
            return false;
        }
        std::vector<persistence::JournalCommand> records;
        records.reserve(recordCount);
        std::uint64_t sequence =
            durableRevision - recordCount + 1U;
        records.push_back({
            .sequence = sequence++,
            .payload = encodeCheckpointBaseline(durableRevision),
        });
        for (const auto& entry : linearHistory) {
            records.push_back({
                .sequence = sequence++,
                .payload = encodeRecord(
                    static_cast<std::uint32_t>(
                        HistoryAction::edit
                    ),
                    entry.forward,
                    entry.inverse
                ),
            });
        }
        for (const auto& entry : redo_) {
            records.push_back({
                .sequence = sequence++,
                .payload = encodeRecord(
                    static_cast<std::uint32_t>(
                        HistoryAction::undo
                    ),
                    entry.forward,
                    entry.inverse
                ),
            });
        }
        if (!persistence::rewriteCommandJournal(
                journalPath_,
                records,
                error
            )) {
            return false;
        }
        checkpointRevision_ = durableRevision;
        snapshotRevision_ = durableRevision;
        return true;
    } catch (const std::bad_alloc&) {
        error = "cannot allocate compacted command history";
        return false;
    }
}

JournaledEditResult JournaledSession::commitEdit(
    const std::uint64_t expectedRevision,
    SessionCommand command,
    std::string& error
) {
    error.clear();
    if (requiresReopen_) {
        error = "journal state is ambiguous; reopen required";
        return {
            .status = JournaledEditStatus::journalFailure,
            .revision = currentRevision(),
        };
    }
    if (expectedRevision != currentRevision()) {
        return {
            .status = JournaledEditStatus::revisionConflict,
            .revision = currentRevision(),
        };
    }

    try {
        const auto before = controller_->snapshot();
        auto candidate = SessionController::fromDocument(*before, error);
        if (candidate == nullptr) {
            return {
                .status = JournaledEditStatus::allocationFailure,
                .revision = currentRevision(),
            };
        }

        SessionEditResult applied;
        if (command.type == SessionCommandType::addTrack
            && command.entityId == 0U) {
            applied = candidate->addTrack(
                expectedRevision,
                command.trackType,
                command.name,
                command.color
            );
            command.entityId = applied.entityId;
        } else {
            applied = applyCommand(
                *candidate,
                expectedRevision,
                command
            );
        }
        if (!applied.applied()) {
            return {
                .status = mapStatus(applied.status),
                .revision = applied.revision,
                .entityId = applied.entityId,
            };
        }

        SessionCommand inverse;
        if (command.type == SessionCommandType::setTempo) {
            inverse.type = SessionCommandType::setTempo;
            inverse.tempo = before->tempo;
        } else if (command.type == SessionCommandType::addTrack) {
            inverse.type = SessionCommandType::removeTrack;
            inverse.entityId = command.entityId;
        } else if (command.type
            == SessionCommandType::renameTrack) {
            const auto* const track = findTrack(
                *before,
                command.entityId
            );
            if (track == nullptr) {
                return {
                    .status = JournaledEditStatus::entityNotFound,
                    .revision = currentRevision(),
                };
            }
            inverse.type = SessionCommandType::renameTrack;
            inverse.entityId = track->stableId;
            inverse.name = track->name;
        } else {
            error = "remove-track is an internal history command";
            return {
                .status = JournaledEditStatus::invalidArgument,
                .revision = currentRevision(),
            };
        }

        undo_.reserve(undo_.size() + 1U);
        auto payload = encodeRecord(
            static_cast<std::uint32_t>(HistoryAction::edit),
            command,
            inverse
        );
        if (!journal_->append(applied.revision, payload, error)) {
            requiresReopen_ = true;
            return {
                .status = JournaledEditStatus::journalFailure,
                .revision = currentRevision(),
            };
        }

        controller_.swap(candidate);
        redo_.clear();
        undo_.push_back({
            .forward = std::move(command),
            .inverse = std::move(inverse),
        });
        return {
            .status = JournaledEditStatus::applied,
            .revision = currentRevision(),
            .entityId = applied.entityId,
        };
    } catch (const std::bad_alloc&) {
        error = "cannot allocate journaled edit";
        return {
            .status = JournaledEditStatus::allocationFailure,
            .revision = currentRevision(),
        };
    }
}

JournaledEditResult JournaledSession::commitHistoryAction(
    const std::uint64_t expectedRevision,
    const HistoryAction action,
    std::string& error
) {
    error.clear();
    if (requiresReopen_) {
        error = "journal state is ambiguous; reopen required";
        return {
            .status = JournaledEditStatus::journalFailure,
            .revision = currentRevision(),
        };
    }
    if (expectedRevision != currentRevision()) {
        return {
            .status = JournaledEditStatus::revisionConflict,
            .revision = currentRevision(),
        };
    }
    const bool isUndo = action == HistoryAction::undo;
    auto& source = isUndo ? undo_ : redo_;
    auto& destination = isUndo ? redo_ : undo_;
    if (source.empty()) {
        return {
            .status = isUndo
                ? JournaledEditStatus::nothingToUndo
                : JournaledEditStatus::nothingToRedo,
            .revision = currentRevision(),
        };
    }

    try {
        const auto before = controller_->snapshot();
        auto candidate = SessionController::fromDocument(*before, error);
        if (candidate == nullptr) {
            return {
                .status = JournaledEditStatus::allocationFailure,
                .revision = currentRevision(),
            };
        }
        destination.reserve(destination.size() + 1U);
        const auto& entry = source.back();
        const auto& appliedCommand = isUndo
            ? entry.inverse
            : entry.forward;
        const auto applied = applyCommand(
            *candidate,
            expectedRevision,
            appliedCommand
        );
        if (!applied.applied()) {
            error = "history command no longer applies";
            return {
                .status = mapStatus(applied.status),
                .revision = applied.revision,
                .entityId = applied.entityId,
            };
        }
        auto payload = encodeRecord(
            static_cast<std::uint32_t>(action),
            entry.forward,
            entry.inverse
        );
        if (!journal_->append(applied.revision, payload, error)) {
            requiresReopen_ = true;
            return {
                .status = JournaledEditStatus::journalFailure,
                .revision = currentRevision(),
            };
        }

        controller_.swap(candidate);
        destination.push_back(std::move(source.back()));
        source.pop_back();
        return {
            .status = JournaledEditStatus::applied,
            .revision = currentRevision(),
            .entityId = applied.entityId,
        };
    } catch (const std::bad_alloc&) {
        error = "cannot allocate history command";
        return {
            .status = JournaledEditStatus::allocationFailure,
            .revision = currentRevision(),
        };
    }
}

bool JournaledSession::replay(
    const std::filesystem::path& journalPath,
    std::string& error
) {
    const auto recovered =
        persistence::recoverCommandJournal(journalPath);
    if (!recovered.ok()) {
        error = recovered.error;
        return false;
    }
    try {
        undo_.reserve(recovered.commands.size());
        redo_.reserve(recovered.commands.size());
        const auto baseRevision = currentRevision();
        for (const auto& record : recovered.commands) {
            std::uint32_t rawAction = 0U;
            std::uint64_t checkpointBaseline = 0U;
            HistoryEntry entry;
            if (!decodeRecord(
                    record.payload,
                    rawAction,
                    checkpointBaseline,
                    entry.forward,
                    entry.inverse
                )) {
                error = "unsupported or corrupt session command payload";
                return false;
            }
            const auto action =
                static_cast<HistoryAction>(rawAction);
            if (action == HistoryAction::checkpointBaseline) {
                if (&record != &recovered.commands.front()
                    || checkpointBaseline == 0U
                    || record.sequence > checkpointBaseline
                    || recovered.commands.back().sequence
                        < checkpointBaseline
                    || currentRevision() < checkpointBaseline) {
                    error =
                        "session snapshot predates or conflicts with "
                        "journal checkpoint baseline";
                    return false;
                }
                checkpointRevision_ = checkpointBaseline;
                continue;
            }
            if (record.sequence > currentRevision()) {
                if (record.sequence != currentRevision() + 1U) {
                    error = "session command revision gap";
                    return false;
                }
                const auto& command =
                    action == HistoryAction::undo
                    ? entry.inverse
                    : entry.forward;
                const auto applied = applyCommand(
                    *controller_,
                    currentRevision(),
                    command
                );
                if (!applied.applied()
                    || applied.revision != record.sequence) {
                    error = "session command replay failed";
                    return false;
                }
            }

            if (action == HistoryAction::edit) {
                undo_.push_back(std::move(entry));
                redo_.clear();
            } else if (action == HistoryAction::undo) {
                if (undo_.empty()
                    || !commandEquals(
                        undo_.back().forward,
                        entry.forward
                    )
                    || !commandEquals(
                        undo_.back().inverse,
                        entry.inverse
                    )) {
                    error = "undo history replay mismatch";
                    return false;
                }
                redo_.push_back(std::move(undo_.back()));
                undo_.pop_back();
            } else {
                if (redo_.empty()
                    || !commandEquals(
                        redo_.back().forward,
                        entry.forward
                    )
                    || !commandEquals(
                        redo_.back().inverse,
                        entry.inverse
                    )) {
                    error = "redo history replay mismatch";
                    return false;
                }
                undo_.push_back(std::move(redo_.back()));
                redo_.pop_back();
            }
        }
        if (currentRevision() < baseRevision) {
            error = "session replay moved revision backwards";
            return false;
        }
        if (!recovered.commands.empty()
            && baseRevision
                > recovered.commands.back().sequence) {
            error =
                "session snapshot exceeds the final journal sequence";
            return false;
        }
        return true;
    } catch (const std::bad_alloc&) {
        error = "cannot allocate recovered command history";
        return false;
    }
}

} // namespace iramix::session
