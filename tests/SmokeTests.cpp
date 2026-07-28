#include "iramix/ipc/Protocol.hpp"
#include "iramix/platform/Platform.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    require(!iramix::platform::operatingSystemName().empty(),
            "The operating system must have a name.");

    const iramix::ipc::Message sent {
        .version = iramix::ipc::kProtocolVersion,
        .type = iramix::ipc::MessageType::ping,
        .sequence = 42U,
        .payload = "codec-smoke",
    };
    std::stringstream wire {
        std::ios::in | std::ios::out | std::ios::binary
    };
    std::string error;
    require(
        iramix::ipc::writeMessage(wire, sent, error),
        "The IPC message must serialize."
    );

    iramix::ipc::Message received;
    require(
        iramix::ipc::readMessage(wire, received, error),
        "The IPC message must deserialize."
    );
    require(received.version == sent.version, "Version must round-trip.");
    require(received.type == sent.type, "Type must round-trip.");
    require(received.sequence == sent.sequence, "Sequence must round-trip.");
    require(received.payload == sent.payload, "Payload must round-trip.");

    std::cout << "All Iramix smoke tests passed.\n";
    return EXIT_SUCCESS;
}
