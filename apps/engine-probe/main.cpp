#include "iramix/ipc/Protocol.hpp"
#include "iramix/platform/Platform.hpp"

#include <iostream>
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

int runIpcSession() {
    using iramix::ipc::Message;
    using iramix::ipc::MessageType;

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
                    + ";capabilities=ping,shutdown";
            }
        } else if (request.type == MessageType::ping) {
            response.type = MessageType::acknowledgement;
            response.payload = "pong";
        } else if (request.type == MessageType::shutdown) {
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
        argc == 2 && std::string_view {argv[1]} == "--ipc-stdio";

    if (handshakeRequested) {
        std::cout
            << "IRAMIX_ENGINE 1 "
            << iramix::platform::operatingSystemName()
            << '\n';
        return 0;
    }
    if (ipcRequested) {
        if (!configureBinaryStandardStreams()) {
            std::cerr << "Failed to configure binary IPC streams.\n";
            return 4;
        }
        return runIpcSession();
    }

    std::cout
        << "Iramix C++ engine probe\n"
        << "OS: " << iramix::platform::operatingSystemName() << '\n';
    return 0;
}
