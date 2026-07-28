#include "iramix/persistence/ProjectBackupStore.hpp"

#include "iramix/persistence/AsyncProjectSaver.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <system_error>

namespace iramix::persistence {
namespace {

constexpr auto backupPrefix = "revision-";
constexpr auto backupSuffix = ".irpx";
constexpr std::size_t revisionDigits = 20U;

[[nodiscard]] bool parseBackupRevision(
    const std::filesystem::path& path,
    std::uint64_t& revision
) noexcept {
    const auto filename = path.filename().string();
    const auto expectedSize =
        std::char_traits<char>::length(backupPrefix)
        + revisionDigits
        + std::char_traits<char>::length(backupSuffix);
    if (filename.size() != expectedSize
        || !filename.starts_with(backupPrefix)
        || !filename.ends_with(backupSuffix)) {
        return false;
    }

    const auto digitsBegin =
        filename.data() + std::char_traits<char>::length(backupPrefix);
    const auto digitsEnd = digitsBegin + revisionDigits;
    const auto parsed =
        std::from_chars(digitsBegin, digitsEnd, revision);
    return parsed.ec == std::errc {} && parsed.ptr == digitsEnd;
}

} // namespace

std::filesystem::path defaultProjectBackupDirectory(
    const std::filesystem::path& projectTarget
) {
    auto directory = projectTarget;
    directory += ".backups";
    return directory;
}

std::filesystem::path projectBackupPath(
    const std::filesystem::path& directory,
    const std::uint64_t revision
) {
    std::array<char, revisionDigits> digits {};
    digits.fill('0');
    std::array<char, revisionDigits + 1U> encoded {};
    const auto result = std::to_chars(
        encoded.data(),
        encoded.data() + encoded.size(),
        revision
    );
    const auto encodedSize =
        static_cast<std::size_t>(result.ptr - encoded.data());
    const auto offset = revisionDigits - encodedSize;
    std::copy_n(encoded.data(), encodedSize, digits.data() + offset);

    std::string filename {backupPrefix};
    filename.append(digits.data(), digits.size());
    filename += backupSuffix;
    return directory / filename;
}

ProjectBackupListResult listProjectBackups(
    const std::filesystem::path& directory
) {
    ProjectBackupListResult result;
    std::error_code errorCode;
    if (!std::filesystem::exists(directory, errorCode)) {
        if (errorCode) {
            result.error = "cannot inspect project backup directory: ";
            result.error += errorCode.message();
            return result;
        }
        result.ok = true;
        return result;
    }
    if (!std::filesystem::is_directory(directory, errorCode)) {
        result.error = "project backup path is not a directory";
        return result;
    }

    std::filesystem::directory_iterator iterator {directory, errorCode};
    const std::filesystem::directory_iterator end;
    while (!errorCode && iterator != end) {
        const auto& entry = *iterator;
        if (entry.is_regular_file(errorCode)) {
            std::uint64_t revision = 0U;
            if (!errorCode
                && parseBackupRevision(entry.path(), revision)) {
                result.entries.push_back({
                    .revision = revision,
                    .path = entry.path(),
                });
            }
        }
        if (!errorCode) {
            iterator.increment(errorCode);
        }
    }
    if (errorCode) {
        result.error = "cannot enumerate project backup directory: ";
        result.error += errorCode.message();
        return result;
    }

    std::sort(
        result.entries.begin(),
        result.entries.end(),
        [](const auto& left, const auto& right) {
            return left.revision > right.revision;
        }
    );
    result.ok = true;
    return result;
}

ProjectBackupSaveResult saveProjectBackup(
    const ProjectBackupPolicy& policy,
    const std::uint64_t revision,
    const std::span<const std::byte> payload
) {
    ProjectBackupSaveResult result;
    result.revision = revision;
    result.bytes = static_cast<std::uint64_t>(payload.size());
    if (!policy.enabled()) {
        result.error = "project backup policy is disabled";
        return result;
    }
    if (policy.directory.empty()) {
        result.error = "project backup directory must not be empty";
        return result;
    }
    if (revision == 0U || payload.empty()) {
        result.error = "project backup revision and payload must be non-zero";
        return result;
    }

    result.path = projectBackupPath(policy.directory, revision);
    if (!saveProjectSnapshot(result.path, payload, result.error)) {
        return result;
    }
    result.committed = true;

    auto listing = listProjectBackups(policy.directory);
    if (!listing.ok) {
        result.error = std::move(listing.error);
        return result;
    }

    for (std::size_t index = policy.retainedBackups;
         index < listing.entries.size();
         ++index) {
        std::error_code errorCode;
        const bool removed = std::filesystem::remove(
            listing.entries[index].path,
            errorCode
        );
        if (!removed || errorCode) {
            result.error = "backup committed but retention prune failed: ";
            result.error += errorCode
                ? errorCode.message()
                : "recognized backup was not removed";
            result.retainedCount =
                static_cast<std::uint64_t>(
                    listing.entries.size() - result.prunedCount
                );
            return result;
        }
        ++result.prunedCount;
    }

    result.retainedCount = static_cast<std::uint64_t>(
        listing.entries.size() - result.prunedCount
    );
    result.retentionApplied = true;
    return result;
}

ProjectBackupRecoveryResult recoverNewestProjectBackup(
    const std::filesystem::path& directory
) {
    ProjectBackupRecoveryResult result;
    const auto listing = listProjectBackups(directory);
    if (!listing.ok) {
        result.error = listing.error;
        return result;
    }

    for (const auto& entry : listing.entries) {
        auto load = loadProjectSnapshot(entry.path);
        if (load.ok) {
            result.path = entry.path;
            result.payload = std::move(load.payload);
            result.revision = entry.revision;
            result.recovered = true;
            return result;
        }
        ++result.skippedInvalidCount;
        result.error = std::move(load.error);
    }

    if (listing.entries.empty()) {
        result.error = "no recognized project backups exist";
    } else {
        result.error = "no envelope-valid project backup exists; last error: "
            + result.error;
    }
    return result;
}

} // namespace iramix::persistence
