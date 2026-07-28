#include "iramix/persistence/CommandJournal.hpp"
#include "iramix/persistence/DiskAudioWorkers.hpp"
#include "iramix/persistence/ProjectStore.hpp"
#include "iramix/persistence/RecoverableRecording.hpp"
#include "iramix/realtime/Audit.hpp"

#include <algorithm>
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
