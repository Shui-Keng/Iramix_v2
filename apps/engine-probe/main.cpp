#include "iramix/ipc/Protocol.hpp"
#include "iramix/persistence/SessionPersistenceService.hpp"
#include "iramix/platform/Platform.hpp"
#include "iramix/session/JournaledSession.hpp"
#include "iramix/session/SessionController.hpp"

#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

bool configureBinaryStandardStreams() {
#if defined(_WIN32)
    return _setmode(_fileno(stdin), _O_BINARY) != -1
        && _setmode(_fileno(stdout), _O_BINARY) != -1;
#else
    return true;
#endif
}

[[nodiscard]] bool parseRevision(
    const std::string_view payload,
    std::uint64_t& revision
) {
    constexpr std::string_view prefix {"revision="};
    if (!payload.starts_with(prefix)) {
        return false;
    }
    const auto value = payload.substr(prefix.size());
    const auto result = std::from_chars(
        value.data(),
        value.data() + value.size(),
        revision
    );
    return result.ec == std::errc {}
        && result.ptr == value.data() + value.size()
        && revision != 0U;
}

[[nodiscard]] bool parseTempoEdit(
    const std::string_view payload,
    std::uint64_t& expectedRevision,
    double& tempo
) {
    constexpr std::string_view revisionPrefix {
        "expected_revision="
    };
    constexpr std::string_view tempoPrefix {"tempo="};
    if (!payload.starts_with(revisionPrefix)) {
        return false;
    }
    const auto separator = payload.find(';');
    if (separator == std::string_view::npos) {
        return false;
    }
    const auto revisionValue = payload.substr(
        revisionPrefix.size(),
        separator - revisionPrefix.size()
    );
    const auto revisionResult = std::from_chars(
        revisionValue.data(),
        revisionValue.data() + revisionValue.size(),
        expectedRevision
    );
    const auto tempoField = payload.substr(separator + 1U);
    if (!tempoField.starts_with(tempoPrefix)) {
        return false;
    }
    const auto tempoValue = tempoField.substr(tempoPrefix.size());
    const auto tempoResult = std::from_chars(
        tempoValue.data(),
        tempoValue.data() + tempoValue.size(),
        tempo
    );
    return revisionResult.ec == std::errc {}
        && revisionResult.ptr
            == revisionValue.data() + revisionValue.size()
        && tempoResult.ec == std::errc {}
        && tempoResult.ptr == tempoValue.data() + tempoValue.size()
        && expectedRevision != 0U;
}

[[nodiscard]] std::string completionPayload(
    const iramix::persistence::SessionSaveCompletion& completion
) {
    const auto status = completion.status
            == iramix::persistence::
                ProjectSaveCompletionStatus::committed
        ? "committed"
        : "failed";
    std::string_view backupStatus;
    using iramix::persistence::SessionBackupCompletionStatus;
    switch (completion.backupStatus) {
    case SessionBackupCompletionStatus::disabled:
        backupStatus = "disabled";
        break;
    case SessionBackupCompletionStatus::committed:
        backupStatus = "committed";
        break;
    case SessionBackupCompletionStatus::committedRetentionWarning:
        backupStatus = "retention_warning";
        break;
    case SessionBackupCompletionStatus::failed:
        backupStatus = "failed";
        break;
    }
    auto payload = std::string {status}
        + ";revision=" + std::to_string(completion.revision)
        + ";bytes=" + std::to_string(completion.serializedBytes)
        + ";serialize_ns="
        + std::to_string(completion.serializationNanoseconds)
        + ";save_ns="
        + std::to_string(completion.durableSaveNanoseconds)
        + ";backup_status=" + std::string {backupStatus}
        + ";backup_ns="
        + std::to_string(completion.backupNanoseconds)
        + ";backup_pruned="
        + std::to_string(completion.backupPrunedCount)
        + ";backup_retained="
        + std::to_string(completion.backupRetainedCount);
    if (completion.detail[0] != '\0') {
        payload += ";detail=";
        payload += completion.detail.data();
    }
    if (completion.backupDetail[0] != '\0') {
        payload += ";backup_detail=";
        payload += completion.backupDetail.data();
    }
    return payload;
}

[[nodiscard]] std::string journaledEditFailure(
    const iramix::session::JournaledEditResult& result,
    const std::string& detail
) {
    using iramix::session::JournaledEditStatus;
    std::string status;
    switch (result.status) {
    case JournaledEditStatus::revisionConflict:
        status = "revision_conflict";
        break;
    case JournaledEditStatus::invalidArgument:
        status = "invalid_argument";
        break;
    case JournaledEditStatus::entityNotFound:
        status = "entity_not_found";
        break;
    case JournaledEditStatus::allocationFailure:
        status = "allocation_failure";
        break;
    case JournaledEditStatus::journalFailure:
        status = "journal_failure";
        break;
    case JournaledEditStatus::nothingToUndo:
        status = "nothing_to_undo";
        break;
    case JournaledEditStatus::nothingToRedo:
        status = "nothing_to_redo";
        break;
    case JournaledEditStatus::applied:
        status = "applied";
        break;
    }
    auto payload = status + ";current_revision="
        + std::to_string(result.revision);
    if (!detail.empty()) {
        payload += ";detail=" + detail;
    }
    return payload;
}

int runIpcSession(
    const std::filesystem::path& projectTarget,
    const std::chrono::milliseconds autosaveInterval
) {
    using iramix::ipc::Message;
    using iramix::ipc::MessageType;

    iramix::session::SessionController ephemeralSession;
    std::unique_ptr<iramix::session::JournaledSession>
        journaledSession;
    std::unique_ptr<
        iramix::persistence::SessionPersistenceService
    > persistenceService;
    std::string setupError;
    if (!projectTarget.empty()) {
        journaledSession =
            iramix::session::JournaledSession::open(
                projectTarget,
                setupError
            );
        if (journaledSession == nullptr) {
            std::cerr
                << "Journaled session setup failed: "
                << setupError << '\n';
            return 5;
        }
        persistenceService =
            iramix::persistence::SessionPersistenceService::create(
                projectTarget,
                autosaveInterval,
                setupError,
                journaledSession->snapshotRevision()
            );
        if (persistenceService == nullptr
            || !persistenceService->start(setupError)) {
            std::cerr
                << "Session save worker setup failed: "
                << setupError << '\n';
            return 5;
        }
    }
    const auto currentRevision = [&]() noexcept {
        return journaledSession != nullptr
            ? journaledSession->currentRevision()
            : ephemeralSession.currentRevision();
    };
    const auto trackCount = [&]() noexcept {
        return journaledSession != nullptr
            ? journaledSession->trackCount()
            : ephemeralSession.trackCount();
    };
    const auto sessionSnapshot = [&]() {
        return journaledSession != nullptr
            ? journaledSession->snapshot()
            : ephemeralSession.snapshot();
    };
    const auto trackAutosave = [&]() {
        if (persistenceService == nullptr
            || journaledSession == nullptr) {
            return std::string {};
        }
        try {
            const auto status = persistenceService->markDirty(
                journaledSession->snapshot()
            );
            if (status
                    == iramix::persistence::
                        AutosaveDirtyStatus::invalidRevision
                || status
                    == iramix::persistence::
                        AutosaveDirtyStatus::stopped) {
                return std::string {
                    ";autosave_tracking=deferred"
                };
            }
            return std::string {};
        } catch (const std::bad_alloc&) {
            return std::string {
                ";autosave_tracking=allocation_failure"
            };
        }
    };
    const auto checkpointDurable = [&]() {
        if (persistenceService == nullptr
            || journaledSession == nullptr) {
            return;
        }
        const auto durable =
            persistenceService->durableRevision();
        if (durable == journaledSession->currentRevision()
            && durable
                != journaledSession->checkpointRevision()) {
            std::string checkpointError;
            if (!journaledSession->checkpoint(
                    durable,
                    checkpointError
                )) {
                std::cerr
                    << "Session checkpoint deferred: "
                    << checkpointError << '\n';
            }
        }
    };

    bool welcomed = false;
    while (true) {
        Message request;
        std::string error;
        if (!iramix::ipc::readMessage(std::cin, request, error)) {
            if (error == "end of stream") {
                if (persistenceService != nullptr) {
                    persistenceService->stop();
                    checkpointDurable();
                }
                return 0;
            }
            std::cerr << "IPC read failed: " << error << '\n';
            return 2;
        }

        Message response {
            .version = iramix::ipc::kProtocolVersion,
            .type = MessageType::reject,
            .sequence = request.sequence,
            .payload = "invalid_state",
        };

        if (!welcomed) {
            if (request.type != MessageType::hello) {
                response.payload = "hello_required";
            } else if (
                request.version != iramix::ipc::kProtocolVersion
            ) {
                response.payload = "unsupported_version";
            } else {
                welcomed = true;
                response.type = MessageType::welcome;
                response.payload = std::string {"os="}
                    + std::string {
                        iramix::platform::operatingSystemName()
                    }
                    + ";capabilities=ping,shutdown,"
                      "session_state,set_tempo"
                    + (persistenceService != nullptr
                        ? ",save_session,poll_save_completion,"
                          "undo,redo,autosave,checkpoint"
                        : "");
            }
        } else if (request.type == MessageType::ping) {
            response.type = MessageType::acknowledgement;
            response.payload = "pong";
        } else if (request.type == MessageType::sessionState) {
            response.type = MessageType::acknowledgement;
            response.payload = "revision="
                + std::to_string(currentRevision())
                + ";tracks=" + std::to_string(trackCount());
            if (journaledSession != nullptr) {
                response.payload += ";undo_depth="
                    + std::to_string(
                        journaledSession->undoDepth()
                    )
                    + ";redo_depth="
                    + std::to_string(
                        journaledSession->redoDepth()
                    );
                response.payload += ";durable_revision="
                    + std::to_string(
                        persistenceService->durableRevision()
                    )
                    + ";dirty_revision="
                    + std::to_string(
                        persistenceService->dirtyRevision()
                    )
                    + ";autosave_requests="
                    + std::to_string(
                        persistenceService->
                            autosaveRequestCount()
                    )
                    + ";checkpoint_revision="
                    + std::to_string(
                        journaledSession->checkpointRevision()
                    );
            }
        } else if (request.type == MessageType::setTempo) {
            std::uint64_t expectedRevision = 0U;
            double tempo = 0.0;
            if (!parseTempoEdit(
                    request.payload,
                    expectedRevision,
                    tempo
                )) {
                response.payload = "invalid_tempo_edit";
            } else {
                if (journaledSession != nullptr) {
                    std::string editError;
                    const auto result =
                        journaledSession->setTempo(
                            expectedRevision,
                            tempo,
                            editError
                        );
                    if (result.applied()) {
                        response.type =
                            MessageType::acknowledgement;
                        response.payload = "revision="
                            + std::to_string(result.revision)
                            + trackAutosave();
                    } else {
                        response.payload = journaledEditFailure(
                            result,
                            editError
                        );
                    }
                } else {
                    const auto result =
                        ephemeralSession.setTempo(
                            expectedRevision,
                            tempo
                        );
                    if (result.applied()) {
                        response.type =
                            MessageType::acknowledgement;
                        response.payload = "revision="
                            + std::to_string(result.revision);
                    } else if (
                        result.status
                        == iramix::session::
                            SessionEditStatus::revisionConflict
                    ) {
                        response.payload =
                            "revision_conflict;current_revision="
                            + std::to_string(result.revision);
                    } else {
                        response.payload =
                            "invalid_tempo;current_revision="
                            + std::to_string(result.revision);
                    }
                }
            }
        } else if (
            request.type == MessageType::undo
            || request.type == MessageType::redo
        ) {
            std::uint64_t revision = 0U;
            if (journaledSession == nullptr) {
                response.payload = "project_target_required";
            } else if (!parseRevision(request.payload, revision)) {
                response.payload = "invalid_history_revision";
            } else {
                std::string editError;
                const auto result =
                    request.type == MessageType::undo
                    ? journaledSession->undo(revision, editError)
                    : journaledSession->redo(revision, editError);
                if (result.applied()) {
                    response.type = MessageType::acknowledgement;
                    response.payload = "revision="
                        + std::to_string(result.revision)
                        + trackAutosave();
                } else {
                    response.payload = journaledEditFailure(
                        result,
                        editError
                    );
                }
            }
        } else if (request.type == MessageType::saveSession) {
            std::uint64_t revision = 0U;
            if (persistenceService == nullptr) {
                response.payload = "project_target_required";
            } else if (!parseRevision(request.payload, revision)) {
                response.payload = "invalid_save_revision";
            } else if (revision != currentRevision()) {
                response.payload =
                    "revision_conflict;current_revision="
                    + std::to_string(currentRevision());
            } else {
                try {
                    const auto result =
                        persistenceService->requestSave(
                            sessionSnapshot()
                        );
                    if (result
                            == iramix::persistence::
                                SessionSaveRequestStatus::accepted
                        || result
                            == iramix::persistence::
                                SessionSaveRequestStatus::coalesced
                        || result
                            == iramix::persistence::
                                SessionSaveRequestStatus::
                                    alreadyRequested) {
                        response.type =
                            MessageType::acknowledgement;
                        response.payload =
                            (result
                                == iramix::persistence::
                                    SessionSaveRequestStatus::coalesced
                                ? "coalesced;revision="
                                : "accepted;revision=")
                            + std::to_string(revision);
                    } else {
                        response.payload = "save_request_rejected";
                    }
                } catch (const std::bad_alloc&) {
                    response.payload = "snapshot_allocation_failed";
                }
            }
        } else if (
            request.type == MessageType::pollSaveCompletion
        ) {
            std::uint64_t revision = 0U;
            if (persistenceService == nullptr) {
                response.payload = "project_target_required";
            } else if (!parseRevision(request.payload, revision)) {
                response.payload = "invalid_save_revision";
            } else {
                const auto query =
                    persistenceService->query(revision);
                if (query.status
                    == iramix::persistence::
                        SessionSaveQueryStatus::committed) {
                    response.type = MessageType::acknowledgement;
                    response.payload =
                        completionPayload(query.completion);
                } else if (query.status
                    == iramix::persistence::
                        SessionSaveQueryStatus::failed
                ) {
                    response.type = MessageType::acknowledgement;
                    response.payload =
                        completionPayload(query.completion);
                } else if (query.status
                    == iramix::persistence::
                        SessionSaveQueryStatus::pending) {
                    response.type = MessageType::acknowledgement;
                    response.payload = "none";
                } else {
                    response.type = MessageType::acknowledgement;
                    response.payload = "none";
                }
            }
        } else if (request.type == MessageType::shutdown) {
            if (persistenceService != nullptr) {
                persistenceService->stop();
                checkpointDurable();
            }
            response.type = MessageType::acknowledgement;
            response.payload = "shutdown";
        } else {
            response.payload = "unsupported_message";
        }

        if (!iramix::ipc::writeMessage(std::cout, response, error)) {
            std::cerr << "IPC write failed: " << error << '\n';
            return 3;
        }
        checkpointDurable();
        if (
            welcomed
            && request.type == MessageType::shutdown
            && response.type == MessageType::acknowledgement
        ) {
            return 0;
        }
    }
}

} // namespace

int main(const int argc, char* argv[]) {
    const bool handshakeRequested =
        argc == 2 && std::string_view {argv[1]} == "--handshake";
    const bool ipcRequested =
        argc >= 2 && std::string_view {argv[1]} == "--ipc-stdio";

    if (handshakeRequested) {
        std::cout
            << "IRAMIX_ENGINE 1 "
            << iramix::platform::operatingSystemName()
            << '\n';
        return 0;
    }
    if (ipcRequested) {
        std::filesystem::path projectTarget;
        std::chrono::milliseconds autosaveInterval {5'000};
        int argument = 2;
        while (argument < argc) {
            const std::string_view option {argv[argument]};
            if (option == "--project" && argument + 1 < argc) {
                projectTarget = argv[argument + 1];
                argument += 2;
            } else if (
                option == "--autosave-interval-ms"
                && argument + 1 < argc
            ) {
                std::uint64_t milliseconds = 0U;
                const std::string_view value {argv[argument + 1]};
                const auto parsed = std::from_chars(
                    value.data(),
                    value.data() + value.size(),
                    milliseconds
                );
                if (parsed.ec != std::errc {}
                    || parsed.ptr != value.data() + value.size()
                    || milliseconds == 0U
                    || milliseconds > 5'000U) {
                    std::cerr
                        << "Invalid autosave interval.\n";
                    return 1;
                }
                autosaveInterval =
                    std::chrono::milliseconds {milliseconds};
                argument += 2;
            } else {
                std::cerr
                    << "Usage: iramix_engine_probe --ipc-stdio "
                       "[--project <path>] "
                       "[--autosave-interval-ms <1..5000>]\n";
                return 1;
            }
        }
        if (projectTarget.empty() && argc != 2) {
            std::cerr
                << "Usage: iramix_engine_probe --ipc-stdio "
                   "[--project <path>] "
                   "[--autosave-interval-ms <1..5000>]\n";
            return 1;
        }
        if (!configureBinaryStandardStreams()) {
            std::cerr << "Failed to configure binary IPC streams.\n";
            return 4;
        }
        return runIpcSession(projectTarget, autosaveInterval);
    }

    std::cout
        << "Iramix C++ engine probe\n"
        << "OS: " << iramix::platform::operatingSystemName() << '\n';
    return 0;
}
