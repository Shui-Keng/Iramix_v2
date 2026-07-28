#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace iramix::persistence {

struct RecordingFormat final {
    std::uint32_t sampleRate {0U};
    std::uint32_t channelCount {0U};
};

struct RecordingRecoveryResult final {
    RecordingFormat format;
    std::vector<float> interleavedSamples;
    std::uint64_t frameCount {0U};
    std::uint64_t blockCount {0U};
    bool discardedInvalidTail {false};
    std::string error;

    [[nodiscard]] bool ok() const noexcept {
        return error.empty();
    }
};

struct RecordingScanResult final {
    RecordingFormat format;
    std::uint64_t frameCount {0U};
    std::uint64_t blockCount {0U};
    std::uint64_t validBytes {0U};
    std::uint64_t fileBytes {0U};
    std::uint32_t maximumFramesPerBlock {0U};
    std::size_t streamingBufferBytes {0U};
    bool discardedInvalidTail {false};
    std::string error;

    [[nodiscard]] bool ok() const noexcept {
        return error.empty();
    }
};

enum class RecordingBlockReadStatus {
    block,
    cleanEnd,
    invalidTail,
    destinationTooSmall,
    ioError,
};

class RecoverableRecordingWriter final {
public:
    ~RecoverableRecordingWriter();

    RecoverableRecordingWriter(
        const RecoverableRecordingWriter&
    ) = delete;
    RecoverableRecordingWriter& operator=(
        const RecoverableRecordingWriter&
    ) = delete;

    [[nodiscard]] static std::unique_ptr<
        RecoverableRecordingWriter
    > create(
        const std::filesystem::path& path,
        RecordingFormat format,
        std::string& error
    );

    [[nodiscard]] bool appendInterleavedBlock(
        std::span<const float> samples,
        std::uint32_t frameCount,
        std::string& error
    );

    // Guarantees every complete block written so far reaches the OS durable
    // flush boundary before returning.
    [[nodiscard]] bool flush(std::string& error);

private:
    RecoverableRecordingWriter(
        std::filesystem::path path,
        RecordingFormat format,
        std::FILE* file
    );

    std::filesystem::path path_;
    RecordingFormat format_;
    std::FILE* file_ {nullptr};
    std::uint64_t nextBlockSequence_ {1U};
};

class RecoverableRecordingReader final {
public:
    ~RecoverableRecordingReader();

    RecoverableRecordingReader(
        const RecoverableRecordingReader&
    ) = delete;
    RecoverableRecordingReader& operator=(
        const RecoverableRecordingReader&
    ) = delete;

    [[nodiscard]] static std::unique_ptr<
        RecoverableRecordingReader
    > create(
        const std::filesystem::path& path,
        std::string& error
    );

    [[nodiscard]] RecordingFormat format() const noexcept {
        return format_;
    }

    [[nodiscard]] RecordingBlockReadStatus readNextBlock(
        std::span<float> destination,
        std::uint32_t& frameCount,
        std::string& error
    );

private:
    RecoverableRecordingReader(
        RecordingFormat format,
        std::FILE* file,
        std::uint64_t fileBytes
    );

    RecordingFormat format_;
    std::FILE* file_ {nullptr};
    std::uint64_t fileBytes_ {0U};
    std::uint64_t offset_ {0U};
    std::uint64_t expectedSequence_ {1U};
};

// Scans and validates the longest complete recording prefix with a fixed-size
// scratch buffer. Sample payloads are never accumulated in memory.
[[nodiscard]] RecordingScanResult scanRecording(
    const std::filesystem::path& path
);

// Truncates only an invalid suffix previously identified by scanRecording.
[[nodiscard]] bool repairRecordingTail(
    const std::filesystem::path& path,
    const RecordingScanResult& scan,
    std::string& error
);

// Compatibility/materialization helper for tests and small imports. Production
// disk streaming should use scanRecording plus RecoverableRecordingReader.
[[nodiscard]] RecordingRecoveryResult recoverRecording(
    const std::filesystem::path& path
);

} // namespace iramix::persistence
