#include "iramix/persistence/CommandJournal.hpp"
#include "iramix/persistence/ProjectStore.hpp"
#include "iramix/persistence/RecoverableRecording.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
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

    std::cout
        << "Recoverable recording: forced_exit=77, "
           "flushed_blocks=2, recovered_frames=4, "
           "partial_tails_discarded=1, corrupt_blocks_rejected=1\n";
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
    testCommandJournal(temporary.path());
    testRecoverableRecording(
        std::filesystem::absolute(argv[0]),
        temporary.path()
    );
    std::cout << "All Iramix persistence tests passed.\n";
    return EXIT_SUCCESS;
}
