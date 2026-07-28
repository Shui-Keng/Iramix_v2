#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace iramix::persistence {

struct ProjectBackupPolicy final {
    std::filesystem::path directory;
    std::uint32_t retainedBackups {0U};

    [[nodiscard]] bool enabled() const noexcept {
        return retainedBackups != 0U;
    }
};

struct ProjectBackupEntry final {
    std::uint64_t revision {0U};
    std::filesystem::path path;
};

struct ProjectBackupListResult final {
    std::vector<ProjectBackupEntry> entries;
    std::string error;
    bool ok {false};
};

struct ProjectBackupSaveResult final {
    std::filesystem::path path;
    std::string error;
    std::uint64_t revision {0U};
    std::uint64_t bytes {0U};
    std::uint64_t prunedCount {0U};
    std::uint64_t retainedCount {0U};
    bool committed {false};
    bool retentionApplied {false};
};

struct ProjectBackupRecoveryResult final {
    std::filesystem::path path;
    std::vector<std::byte> payload;
    std::string error;
    std::uint64_t revision {0U};
    std::uint64_t skippedInvalidCount {0U};
    bool recovered {false};
};

[[nodiscard]] std::filesystem::path defaultProjectBackupDirectory(
    const std::filesystem::path& projectTarget
);

[[nodiscard]] std::filesystem::path projectBackupPath(
    const std::filesystem::path& directory,
    std::uint64_t revision
);

// Only strictly recognized revision-<20 digits>.irpx files are returned.
// Unknown files and transaction staging files are never retention targets.
[[nodiscard]] ProjectBackupListResult listProjectBackups(
    const std::filesystem::path& directory
);

// Commits the revisioned backup before pruning. A retention failure is
// reported separately and never rolls back the newly committed backup.
[[nodiscard]] ProjectBackupSaveResult saveProjectBackup(
    const ProjectBackupPolicy& policy,
    std::uint64_t revision,
    std::span<const std::byte> payload
);

// Finds the newest envelope-valid backup, skipping corrupt recognized files.
// The caller remains responsible for validating the payload schema and its
// embedded revision before restoring it as the active project.
[[nodiscard]] ProjectBackupRecoveryResult recoverNewestProjectBackup(
    const std::filesystem::path& directory
);

} // namespace iramix::persistence
