#include "iramix/ipc/Protocol.hpp"
#include "iramix/persistence/AsyncSessionSaver.hpp"
#include "iramix/platform/Platform.hpp"

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

    std::unique_ptr<iramix::persistence::AsyncSessionSaver> saver;
    std::string setupError;
    if (!projectTarget.empty()) {
        saver = iramix::persistence::AsyncSessionSaver::create(
            projectTarget,
            8U,
            setupError
        );
        if (saver == nullptr || !saver->start(setupError)) {
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
                    + ";capabilities=ping,shutdown"
                    + (saver != nullptr
                        ? ",save_session,poll_save_completion"
                        : "");
            }
        } else if (request.type == MessageType::ping) {
            response.type = MessageType::acknowledgement;
            response.payload = "pong";
        } else if (request.type == MessageType::saveSession) {
            std::uint64_t revision = 0U;
            if (saver == nullptr) {
                response.payload = "project_target_required";
            } else if (!parseRevision(request.payload, revision)) {
                response.payload = "invalid_save_revision";
            } else {
                iramix::persistence::SessionDocument document;
                document.revision = revision;
                document.tracks.push_back({
                    .stableId = 1U,
                    .type = iramix::persistence::
                        SessionTrackType::master,
                    .gain = 1.0F,
                    .color = 0xFF20'3040U,
                    .name = "Master",
                });
                const auto snapshot = std::make_shared<
                    const iramix::persistence::SessionDocument
                >(std::move(document));
                const auto result =
                    saver->trySubmit(revision, snapshot);
                if (result
                    == iramix::persistence::
                        ProjectSaveSubmitResult::accepted) {
                    response.type = MessageType::acknowledgement;
                    response.payload = "accepted;revision="
                        + std::to_string(revision);
                } else if (
                    result
                    == iramix::persistence::
                        ProjectSaveSubmitResult::full
                ) {
                    response.payload = "save_pipeline_full";
                } else {
                    response.payload = "invalid_save_revision";
                }
            }
        } else if (
            request.type == MessageType::pollSaveCompletion
        ) {
            if (saver == nullptr) {
                response.payload = "project_target_required";
            } else {
                iramix::persistence::SessionSaveCompletion completion;
                response.type = MessageType::acknowledgement;
                response.payload =
                    saver->tryPopCompletion(completion)
                    ? completionPayload(completion)
                    : "none";
            }
        } else if (request.type == MessageType::shutdown) {
            if (saver != nullptr) {
                saver->stop();
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
