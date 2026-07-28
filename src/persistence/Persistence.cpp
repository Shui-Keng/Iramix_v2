#include "iramix/persistence/CommandJournal.hpp"
#include "iramix/persistence/ProjectStore.hpp"
#include "iramix/persistence/RecoverableRecording.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace iramix::persistence {
namespace {

static_assert(std::endian::native == std::endian::little);

[[nodiscard]] constexpr std::byte asByte(const char value) noexcept {
    return static_cast<std::byte>(
        static_cast<unsigned char>(value)
    );
}

constexpr std::array<std::byte, 4> kProjectMagic {
    asByte('I'), asByte('R'), asByte('P'), asByte('X'),
};
constexpr std::array<std::byte, 4> kJournalMagic {
    asByte('I'), asByte('R'), asByte('J'), asByte('C'),
};
constexpr std::array<std::byte, 4> kRecordingMagic {
    asByte('I'), asByte('R'), asByte('R'), asByte('C'),
};
constexpr std::array<std::byte, 4> kRecordingBlockMagic {
    asByte('B'), asByte('L'), asByte('K'), asByte('1'),
};
constexpr std::uint32_t kFormatVersion = 1U;
constexpr std::size_t kProjectHeaderSize = 20U;
constexpr std::size_t kJournalHeaderSize = 24U;
constexpr std::size_t kRecordingHeaderSize = 16U;
constexpr std::size_t kRecordingBlockHeaderSize = 24U;

[[nodiscard]] std::string systemError(const int code) {
    return std::system_category().message(code);
}

[[nodiscard]] bool ensureParentDirectory(
    const std::filesystem::path& path,
    std::string& error
) {
    const auto parent = path.parent_path();
    if (parent.empty()) {
        return true;
    }
    std::error_code directoryError;
    std::filesystem::create_directories(parent, directoryError);
    if (directoryError) {
        error = "cannot create parent directory: "
            + directoryError.message();
        return false;
    }
    return true;
}

[[nodiscard]] std::FILE* openFile(
    const std::filesystem::path& path,
    const wchar_t* const windowsMode,
    const char* const posixMode
) noexcept {
#if defined(_WIN32)
    static_cast<void>(posixMode);
    std::FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), windowsMode) != 0) {
        return nullptr;
    }
    return file;
#else
    static_cast<void>(windowsMode);
    return std::fopen(path.c_str(), posixMode);
#endif
}

[[nodiscard]] bool writeAll(
    std::FILE* const file,
    const void* const data,
    const std::size_t bytes,
    std::string& error
) {
    if (bytes == 0U) {
        return true;
    }
    if (std::fwrite(data, 1U, bytes, file) != bytes) {
        error = "file write failed: " + systemError(errno);
        return false;
    }
    return true;
}

[[nodiscard]] bool durableFlush(
    std::FILE* const file,
    std::string& error
) {
    if (std::fflush(file) != 0) {
        error = "file flush failed: " + systemError(errno);
        return false;
    }
#if defined(_WIN32)
    if (_commit(_fileno(file)) != 0) {
        error = "file commit failed: " + systemError(errno);
        return false;
    }
#else
    if (::fsync(fileno(file)) != 0) {
        error = "file fsync failed: " + systemError(errno);
        return false;
    }
#endif
    return true;
}

[[nodiscard]] bool readFile(
    const std::filesystem::path& path,
    std::vector<std::byte>& bytes,
    std::string& error
) {
    auto* const file = openFile(path, L"rb", "rb");
    if (file == nullptr) {
        error = "cannot open file: " + systemError(errno);
        return false;
    }
    if (std::fseek(file, 0L, SEEK_END) != 0) {
        error = "cannot seek file";
        std::fclose(file);
        return false;
    }
    const long size = std::ftell(file);
    if (size < 0 || std::fseek(file, 0L, SEEK_SET) != 0) {
        error = "cannot determine file size";
        std::fclose(file);
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    const bool read = bytes.empty()
        || std::fread(bytes.data(), 1U, bytes.size(), file)
            == bytes.size();
    const int closeResult = std::fclose(file);
    if (!read || closeResult != 0) {
        error = "file read failed";
        return false;
    }
    return true;
}

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

[[nodiscard]] bool readMagic(
    const std::span<const std::byte> bytes,
    std::size_t& offset,
    const std::array<std::byte, 4>& magic
) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < magic.size()) {
        return false;
    }
    const bool matches = std::equal(
        magic.begin(),
        magic.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(offset)
    );
    offset += magic.size();
    return matches;
}

[[nodiscard]] std::uint32_t crc32(
    const std::span<const std::byte> bytes
) noexcept {
    std::uint32_t crc = 0xFFFF'FFFFU;
    for (const auto byte : bytes) {
        crc ^= std::to_integer<std::uint32_t>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask =
                0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB8'8320U & mask);
        }
    }
    return ~crc;
}

[[nodiscard]] bool atomicReplace(
    const std::filesystem::path& staging,
    const std::filesystem::path& target,
    std::string& error
) {
#if defined(_WIN32)
    if (!MoveFileExW(
            staging.c_str(),
            target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
        )) {
        error = "atomic replace failed: "
            + systemError(static_cast<int>(GetLastError()));
        return false;
    }
    return true;
#else
    if (::rename(staging.c_str(), target.c_str()) != 0) {
        error = "atomic replace failed: " + systemError(errno);
        return false;
    }
    const auto directory = target.parent_path().empty()
        ? std::filesystem::path {"."}
        : target.parent_path();
    const int descriptor = ::open(
        directory.c_str(),
        O_RDONLY | O_DIRECTORY
    );
    if (descriptor < 0) {
        error = "cannot open project directory for fsync: "
            + systemError(errno);
        return false;
    }
    const int syncResult = ::fsync(descriptor);
    const int closeResult = ::close(descriptor);
    if (syncResult != 0 || closeResult != 0) {
        error = "project directory fsync failed: "
            + systemError(errno);
        return false;
    }
    return true;
#endif
}

[[nodiscard]] std::vector<std::byte> projectEnvelope(
    const std::span<const std::byte> payload
) {
    std::vector<std::byte> bytes;
    bytes.reserve(kProjectHeaderSize + payload.size());
    bytes.insert(bytes.end(), kProjectMagic.begin(), kProjectMagic.end());
    appendU32(bytes, kFormatVersion);
    appendU64(bytes, static_cast<std::uint64_t>(payload.size()));
    appendU32(bytes, crc32(payload));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

[[nodiscard]] ProjectLoadResult decodeProject(
    const std::filesystem::path& path
) {
    ProjectLoadResult result;
    std::vector<std::byte> bytes;
    if (!readFile(path, bytes, result.error)) {
        return result;
    }
    std::size_t offset = 0U;
    std::uint32_t version = 0U;
    std::uint64_t payloadSize = 0U;
    std::uint32_t expectedCrc = 0U;
    if (!readMagic(bytes, offset, kProjectMagic)
        || !readU32(bytes, offset, version)
        || version != kFormatVersion
        || !readU64(bytes, offset, payloadSize)
        || !readU32(bytes, offset, expectedCrc)
        || payloadSize > bytes.size() - offset
        || payloadSize != bytes.size() - offset) {
        result.error = "invalid project snapshot envelope";
        return result;
    }
    const auto payload = std::span<const std::byte> {
        bytes.data() + static_cast<std::ptrdiff_t>(offset),
        static_cast<std::size_t>(payloadSize),
    };
    if (crc32(payload) != expectedCrc) {
        result.error = "project snapshot checksum mismatch";
        return result;
    }
    result.payload.assign(payload.begin(), payload.end());
    result.ok = true;
    result.error.clear();
    return result;
}

} // namespace

std::filesystem::path projectStagingPath(
    const std::filesystem::path& target
) {
    auto result = target;
    result += ".saving";
    return result;
}

bool saveProjectSnapshot(
    const std::filesystem::path& target,
    const std::span<const std::byte> payload,
    std::string& error,
    const AtomicSaveFailurePoint failurePoint
) {
    error.clear();
    if (!ensureParentDirectory(target, error)) {
        return false;
    }

    const auto staging = projectStagingPath(target);
    auto* const file = openFile(staging, L"wb", "wb");
    if (file == nullptr) {
        error = "cannot open project staging file: "
            + systemError(errno);
        return false;
    }
    const auto envelope = projectEnvelope(payload);
    const bool written = writeAll(
        file,
        envelope.data(),
        envelope.size(),
        error
    );
    const bool flushed = written && durableFlush(file, error);
    const int closeResult = std::fclose(file);
    if (!flushed || closeResult != 0) {
        if (error.empty()) {
            error = "cannot close project staging file";
        }
        return false;
    }
    if (failurePoint == AtomicSaveFailurePoint::afterStagingFlush) {
        error = "simulated failure after staging flush";
        return false;
    }
    return atomicReplace(staging, target, error);
}

ProjectLoadResult loadProjectSnapshot(
    const std::filesystem::path& target
) {
    auto result = decodeProject(target);
    const auto staging = projectStagingPath(target);
    if (result.ok) {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        return result;
    }

    auto staged = decodeProject(staging);
    if (!staged.ok) {
        result.error += "; staging recovery failed: " + staged.error;
        return result;
    }
    std::string replaceError;
    if (!atomicReplace(staging, target, replaceError)) {
        staged.error = replaceError;
        staged.ok = false;
        return staged;
    }
    staged.recoveredFromStaging = true;
    return staged;
}

JournalRecoveryResult recoverCommandJournal(
    const std::filesystem::path& path
) {
    JournalRecoveryResult result;
    if (!std::filesystem::exists(path)) {
        return result;
    }
    std::vector<std::byte> bytes;
    if (!readFile(path, bytes, result.error)) {
        return result;
    }

    std::size_t offset = 0U;
    std::uint64_t previousSequence = 0U;
    while (offset < bytes.size()) {
        const std::size_t recordStart = offset;
        std::uint32_t version = 0U;
        std::uint64_t sequence = 0U;
        std::uint32_t payloadSize = 0U;
        std::uint32_t expectedCrc = 0U;
        if (!readMagic(bytes, offset, kJournalMagic)
            || !readU32(bytes, offset, version)
            || version != kFormatVersion
            || !readU64(bytes, offset, sequence)
            || !readU32(bytes, offset, payloadSize)
            || !readU32(bytes, offset, expectedCrc)
            || sequence <= previousSequence
            || payloadSize > bytes.size() - offset) {
            offset = recordStart;
            result.discardedInvalidTail = true;
            break;
        }
        const auto payload = std::span<const std::byte> {
            bytes.data() + static_cast<std::ptrdiff_t>(offset),
            payloadSize,
        };
        if (crc32(payload) != expectedCrc) {
            offset = recordStart;
            result.discardedInvalidTail = true;
            break;
        }
        JournalCommand command;
        command.sequence = sequence;
        command.payload.assign(payload.begin(), payload.end());
        result.commands.push_back(std::move(command));
        previousSequence = sequence;
        offset += payloadSize;
    }
    result.validBytes = static_cast<std::uint64_t>(offset);
    return result;
}

CommandJournal::CommandJournal(std::filesystem::path path)
    : path_ {std::move(path)} {}

bool CommandJournal::initialize(std::string& error) {
    const auto recovered = recoverCommandJournal(path_);
    if (!recovered.ok()) {
        error = recovered.error;
        return false;
    }
    if (recovered.discardedInvalidTail) {
        std::error_code resizeError;
        std::filesystem::resize_file(
            path_,
            recovered.validBytes,
            resizeError
        );
        if (resizeError) {
            error = "cannot truncate invalid journal tail: "
                + resizeError.message();
            return false;
        }
    }
    lastSequence_ = recovered.commands.empty()
        ? 0U
        : recovered.commands.back().sequence;
    initialized_ = true;
    return true;
}

bool CommandJournal::append(
    const std::uint64_t sequence,
    const std::span<const std::byte> payload,
    std::string& error
) {
    error.clear();
    if (!initialized_ && !initialize(error)) {
        return false;
    }
    if (sequence == 0U || sequence <= lastSequence_) {
        error = "journal sequence must be strictly increasing";
        return false;
    }
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "journal payload exceeds format limit";
        return false;
    }
    if (!ensureParentDirectory(path_, error)) {
        return false;
    }

    std::vector<std::byte> record;
    record.reserve(kJournalHeaderSize + payload.size());
    record.insert(record.end(), kJournalMagic.begin(), kJournalMagic.end());
    appendU32(record, kFormatVersion);
    appendU64(record, sequence);
    appendU32(record, static_cast<std::uint32_t>(payload.size()));
    appendU32(record, crc32(payload));
    record.insert(record.end(), payload.begin(), payload.end());

    auto* const file = openFile(path_, L"ab", "ab");
    if (file == nullptr) {
        error = "cannot open command journal: " + systemError(errno);
        return false;
    }
    const bool written = writeAll(
        file,
        record.data(),
        record.size(),
        error
    );
    const bool flushed = written && durableFlush(file, error);
    const int closeResult = std::fclose(file);
    if (!flushed || closeResult != 0) {
        if (error.empty()) {
            error = "cannot close command journal";
        }
        return false;
    }
    lastSequence_ = sequence;
    return true;
}

RecoverableRecordingWriter::RecoverableRecordingWriter(
    std::filesystem::path path,
    const RecordingFormat format,
    std::FILE* const file
)
    : path_ {std::move(path)},
      format_ {format},
      file_ {file} {}

RecoverableRecordingWriter::~RecoverableRecordingWriter() {
    if (file_ != nullptr) {
        std::fclose(file_);
    }
}

std::unique_ptr<RecoverableRecordingWriter>
RecoverableRecordingWriter::create(
    const std::filesystem::path& path,
    const RecordingFormat format,
    std::string& error
) {
    error.clear();
    if (format.sampleRate == 0U || format.channelCount == 0U) {
        error = "recording format must be non-zero";
        return {};
    }
    if (!ensureParentDirectory(path, error)) {
        return {};
    }
    auto* const file = openFile(path, L"wb", "wb");
    if (file == nullptr) {
        error = "cannot open recoverable recording: "
            + systemError(errno);
        return {};
    }
    std::vector<std::byte> header;
    header.reserve(kRecordingHeaderSize);
    header.insert(
        header.end(),
        kRecordingMagic.begin(),
        kRecordingMagic.end()
    );
    appendU32(header, kFormatVersion);
    appendU32(header, format.sampleRate);
    appendU32(header, format.channelCount);
    if (!writeAll(file, header.data(), header.size(), error)
        || !durableFlush(file, error)) {
        std::fclose(file);
        return {};
    }
    return std::unique_ptr<RecoverableRecordingWriter> {
        new RecoverableRecordingWriter {path, format, file}
    };
}

bool RecoverableRecordingWriter::appendInterleavedBlock(
    const std::span<const float> samples,
    const std::uint32_t frameCount,
    std::string& error
) {
    error.clear();
    const std::uint64_t expectedSamples =
        static_cast<std::uint64_t>(frameCount)
        * format_.channelCount;
    if (frameCount == 0U
        || expectedSamples != samples.size()
        || samples.size()
            > std::numeric_limits<std::uint32_t>::max()
                / sizeof(float)) {
        error = "recording block shape is invalid";
        return false;
    }
    const auto payload = std::as_bytes(samples);
    std::vector<std::byte> header;
    header.reserve(kRecordingBlockHeaderSize);
    header.insert(
        header.end(),
        kRecordingBlockMagic.begin(),
        kRecordingBlockMagic.end()
    );
    appendU64(header, nextBlockSequence_);
    appendU32(header, frameCount);
    appendU32(header, static_cast<std::uint32_t>(payload.size()));
    appendU32(header, crc32(payload));
    if (!writeAll(file_, header.data(), header.size(), error)
        || !writeAll(file_, payload.data(), payload.size(), error)) {
        return false;
    }
    ++nextBlockSequence_;
    return true;
}

bool RecoverableRecordingWriter::flush(std::string& error) {
    error.clear();
    return durableFlush(file_, error);
}

RecordingRecoveryResult recoverRecording(
    const std::filesystem::path& path
) {
    RecordingRecoveryResult result;
    std::vector<std::byte> bytes;
    if (!readFile(path, bytes, result.error)) {
        return result;
    }
    std::size_t offset = 0U;
    std::uint32_t version = 0U;
    if (!readMagic(bytes, offset, kRecordingMagic)
        || !readU32(bytes, offset, version)
        || version != kFormatVersion
        || !readU32(bytes, offset, result.format.sampleRate)
        || !readU32(bytes, offset, result.format.channelCount)
        || result.format.sampleRate == 0U
        || result.format.channelCount == 0U) {
        result.error = "invalid recoverable recording header";
        return result;
    }

    std::uint64_t expectedSequence = 1U;
    while (offset < bytes.size()) {
        const std::size_t blockStart = offset;
        std::uint64_t sequence = 0U;
        std::uint32_t frameCount = 0U;
        std::uint32_t payloadBytes = 0U;
        std::uint32_t expectedCrc = 0U;
        if (!readMagic(bytes, offset, kRecordingBlockMagic)
            || !readU64(bytes, offset, sequence)
            || !readU32(bytes, offset, frameCount)
            || !readU32(bytes, offset, payloadBytes)
            || !readU32(bytes, offset, expectedCrc)
            || sequence != expectedSequence
            || frameCount == 0U
            || payloadBytes > bytes.size() - offset) {
            offset = blockStart;
            result.discardedInvalidTail = true;
            break;
        }
        const std::uint64_t expectedBytes =
            static_cast<std::uint64_t>(frameCount)
            * result.format.channelCount;
        if (
            expectedBytes
            > std::numeric_limits<std::uint64_t>::max()
                / sizeof(float)
        ) {
            offset = blockStart;
            result.discardedInvalidTail = true;
            break;
        }
        const std::uint64_t expectedPayloadBytes =
            expectedBytes * sizeof(float);
        if (payloadBytes != expectedPayloadBytes) {
            offset = blockStart;
            result.discardedInvalidTail = true;
            break;
        }
        const auto payload = std::span<const std::byte> {
            bytes.data() + static_cast<std::ptrdiff_t>(offset),
            payloadBytes,
        };
        if (crc32(payload) != expectedCrc) {
            offset = blockStart;
            result.discardedInvalidTail = true;
            break;
        }
        const std::size_t previousSamples =
            result.interleavedSamples.size();
        const std::size_t addedSamples =
            payloadBytes / sizeof(float);
        result.interleavedSamples.resize(
            previousSamples + addedSamples
        );
        std::memcpy(
            result.interleavedSamples.data()
                + static_cast<std::ptrdiff_t>(previousSamples),
            payload.data(),
            payload.size()
        );
        result.frameCount += frameCount;
        ++result.blockCount;
        ++expectedSequence;
        offset += payloadBytes;
    }
    return result;
}

} // namespace iramix::persistence
