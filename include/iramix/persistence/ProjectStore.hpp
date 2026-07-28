#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace iramix::persistence {

enum class AtomicSaveFailurePoint {
    none,
    afterStagingFlush,
};

struct ProjectLoadResult final {
    std::vector<std::byte> payload;
    bool ok {false};
    bool recoveredFromStaging {false};
    std::string error;
};

// Writes a checksummed staging file, durably flushes it, then atomically
// replaces the target. A successful return is the acknowledgement boundary.
[[nodiscard]] bool saveProjectSnapshot(
    const std::filesystem::path& target,
    std::span<const std::byte> payload,
    std::string& error,
    AtomicSaveFailurePoint failurePoint =
        AtomicSaveFailurePoint::none
);

// Loads the committed target. If it is absent or corrupt and a complete
// staging file exists, promotes the staging file atomically and reports
// recoveredFromStaging.
[[nodiscard]] ProjectLoadResult loadProjectSnapshot(
    const std::filesystem::path& target
);

[[nodiscard]] std::filesystem::path projectStagingPath(
    const std::filesystem::path& target
);

} // namespace iramix::persistence
