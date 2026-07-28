#pragma once

#include "iramix/persistence/SessionDocument.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace iramix::persistence {

inline constexpr std::uint64_t defaultMaximumHashBytes =
    67'108'864U;
inline constexpr std::size_t defaultMaximumSearchEntries = 100'000U;

enum class MediaResolutionStatus : std::uint32_t {
    // Declared path opened and its content hash matched the document.
    verified = 1U,
    // Declared path opened but the bytes no longer match. The document is
    // never rewritten in this case: the file is a different take.
    mismatched = 2U,
    // Declared path was gone; a search directory held a hash-matching file.
    relocated = 3U,
    // A file exists but the source names no verifiable identity, so
    // sameness cannot be established. Schema v3 placeholders land here
    // only once a path has been supplied by other means.
    unverifiable = 4U,
    // No candidate file was found, including every v3 placeholder.
    missing = 5U,
};

struct MediaResolution final {
    std::uint64_t sourceId {0U};
    MediaResolutionStatus status {MediaResolutionStatus::missing};
    std::filesystem::path resolvedPath;
    std::uint64_t observedHash {0U};
    std::uint64_t observedBytes {0U};
};

struct MediaResolutionReport final {
    std::vector<MediaResolution> resolutions;
    std::size_t verifiedCount {0U};
    std::size_t mismatchedCount {0U};
    std::size_t relocatedCount {0U};
    std::size_t unverifiableCount {0U};
    std::size_t missingCount {0U};
    std::size_t searchEntriesScanned {0U};
    bool searchBudgetExhausted {false};

    // True only when every source is playable and proven to be the file
    // the document named. Anything else needs a user decision.
    [[nodiscard]] bool complete() const noexcept {
        return mismatchedCount == 0U && unverifiableCount == 0U
            && missingCount == 0U;
    }
};

struct MediaResolverConfig final {
    std::filesystem::path projectRoot;
    std::vector<std::filesystem::path> searchDirectories;
    std::uint64_t maximumHashBytes {defaultMaximumHashBytes};
    std::size_t maximumSearchEntries {defaultMaximumSearchEntries};
};

// Content hash over at most maximumBytes of file content, mixed with the
// full byte length so a file that only differs past the bound still
// separates. Not cryptographic: it detects substitution and truncation,
// not a deliberately crafted collision.
[[nodiscard]] std::uint64_t hashMediaFile(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes,
    std::uint64_t& hashedBytes,
    std::string& error
);

// Read-only: inspects the filesystem and reports what it found. Never
// mutates the document, so a caller can present the result before acting.
[[nodiscard]] MediaResolutionReport resolveSessionMedia(
    const SessionDocument& document,
    const MediaResolverConfig& config
);

// Applies relocations only. Mismatched, unverifiable, and missing sources
// are left exactly as they were, so a failed relink never silently binds a
// session to the wrong audio. Returns the number of paths rewritten.
[[nodiscard]] std::size_t applyMediaResolution(
    SessionDocument& document,
    const MediaResolutionReport& report,
    const MediaResolverConfig& config
);

} // namespace iramix::persistence
