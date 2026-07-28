#include "iramix/ipc/Protocol.hpp"
#include "iramix/persistence/SessionSaveCoordinator.hpp"
#include "iramix/platform/Platform.hpp"
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
    auto payload = std::string {status}
        + ";revision=" + std::to_string(completion.revision)
        + ";bytes=" + std::to_string(completion.serializedBytes)
        + ";serialize_ns="
        + std::to_string(completion.serializationNanoseconds)
        + ";save_ns="
        + std::to_string(completion.durableSaveNanoseconds);
    if (completion.detail[0] != '\0') {
        payload += ";detail=";
        payload += completion.detail.data();
    }
    return payload;
}

int runIpcSession(const std::filesystem::path& projectTarget) {
    using iramix::ipc::Message;
    using iramix::ipc::MessageType;

    iramix::session::SessionController session;
    std::unique_ptr<
        iramix::persistence::SessionSaveCoordinator
    > saveCoordinator;
    std::string setupError;
    if (!projectTarget.empty()) {
        saveCoordinator =
            iramix::persistence::SessionSaveCoordinator::create(
            projectTarget,
            setupError
        );
        if (saveCoordinator == nullptr
            || !saveCoordinator->start(setupError)) {
            std::cerr
                << "Session save worker setup failed: "
                << setupError << '\n';
            return 5;
        }
    }

    bool welcomed = false;
    while (true) {
        Message request;
        std::string error;
        if (!iramix::ipc::readMessage(std::cin, request, error)) {
            if (error == "end of stream") {
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
                    + (saveCoordinator != nullptr
                        ? ",save_session,poll_save_completion"
                        : "");
            }
        } else if (request.type == MessageType::ping) {
            response.type = MessageType::acknowledgement;
            response.payload = "pong";
        } else if (request.type == MessageType::sessionState) {
            response.type = MessageType::acknowledgement;
            response.payload = "revision="
                + std::to_string(session.currentRevision())
                + ";tracks=" + std::to_string(session.trackCount());
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
                const auto result = session.setTempo(
                    expectedRevision,
                    tempo
                );
                if (result.applied()) {
                    response.type = MessageType::acknowledgement;
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
        } else if (request.type == MessageType::saveSession) {
            std::uint64_t revision = 0U;
            if (saveCoordinator == nullptr) {
                response.payload = "project_target_required";
            } else if (!parseRevision(request.payload, revision)) {
                response.payload = "invalid_save_revision";
            } else if (revision != session.currentRevision()) {
                response.payload =
                    "revision_conflict;current_revision="
                    + std::to_string(session.currentRevision());
            } else {
                try {
                    const auto result =
                        saveCoordinator->requestSave(
                            session.snapshot()
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
            if (saveCoordinator == nullptr) {
                response.payload = "project_target_required";
            } else if (!parseRevision(request.payload, revision)) {
                response.payload = "invalid_save_revision";
            } else {
                const auto query = saveCoordinator->query(revision);
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
                    response.payload = "unknown_save_revision";
                }
            }
        } else if (request.type == MessageType::shutdown) {
            if (saveCoordinator != nullptr) {
                saveCoordinator->stop();
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
        if (argc == 4
            && std::string_view {argv[2]} == "--project") {
            projectTarget = argv[3];
        } else if (argc != 2) {
            std::cerr
                << "Usage: iramix_engine_probe --ipc-stdio "
                   "[--project <path>]\n";
            return 1;
        }
        if (!configureBinaryStandardStreams()) {
            std::cerr << "Failed to configure binary IPC streams.\n";
            return 4;
        }
        return runIpcSession(projectTarget);
    }

    std::cout
        << "Iramix C++ engine probe\n"
        << "OS: " << iramix::platform::operatingSystemName() << '\n';
    return 0;
}
