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

[[nodiscard]] RecordingRecoveryResult recoverRecording(
    const std::filesystem::path& path
);

} // namespace iramix::persistence
