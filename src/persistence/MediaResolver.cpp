#include "iramix/persistence/MediaResolver.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <new>
#include <system_error>
#include <utility>

namespace iramix::persistence {
namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;
constexpr std::size_t kHashChunkBytes = 65'536U;
constexpr std::size_t kMaximumPathBytes = 4'096U;

[[nodiscard]] std::uint64_t mixHash(
    std::uint64_t hash,
    const std::uint64_t value
) noexcept {
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        hash ^= (value >> shift) & 0xFFULL;
        hash *= kFnvPrime;
    }
    return hash;
}

// Resolves a document path against the project root so relative paths keep
// working when a project directory is moved or copied wholesale.
[[nodiscard]] std::filesystem::path absoluteCandidate(
    const std::filesystem::path& declared,
    const std::filesystem::path& projectRoot
) {
    if (declared.empty() || declared.is_absolute()
        || projectRoot.empty()) {
        return declared;
    }
    return projectRoot / declared;
}

[[nodiscard]] bool isRegularFile(
    const std::filesystem::path& path
) noexcept {
    std::error_code code;
    return !path.empty()
        && std::filesystem::is_regular_file(path, code) && !code;
}

// Bounded, error-tolerant scan: a directory we cannot read is skipped
// rather than aborting the relink of every other source.
[[nodiscard]] std::filesystem::path findByFilename(
    const std::filesystem::path& filename,
    const std::uint64_t expectedHash,
    const MediaResolverConfig& config,
    std::size_t& entriesScanned,
    bool& budgetExhausted,
    std::uint64_t& observedHash,
    std::uint64_t& observedBytes
) {
    if (filename.empty() || expectedHash == 0U) {
        return {};
    }
    for (const auto& directory : config.searchDirectories) {
        std::error_code code;
        std::filesystem::recursive_directory_iterator iterator {
            directory,
            std::filesystem::directory_options::skip_permission_denied,
            code,
        };
        if (code) {
            continue;
        }
        const std::filesystem::recursive_directory_iterator end;
        for (; iterator != end; iterator.increment(code)) {
            if (code) {
                break;
            }
            if (entriesScanned >= config.maximumSearchEntries) {
                budgetExhausted = true;
                return {};
            }
            ++entriesScanned;
            const auto& candidate = iterator->path();
            if (candidate.filename() != filename
                || !isRegularFile(candidate)) {
                continue;
            }
            std::string hashError;
            std::uint64_t hashedBytes = 0U;
            const auto hash = hashMediaFile(
                candidate,
                config.maximumHashBytes,
                hashedBytes,
                hashError
            );
            if (hashError.empty() && hash == expectedHash) {
                observedHash = hash;
                observedBytes = hashedBytes;
                return candidate;
            }
        }
    }
    return {};
}

} // namespace

std::uint64_t hashMediaFile(
    const std::filesystem::path& path,
    const std::uint64_t maximumBytes,
    std::uint64_t& hashedBytes,
    std::string& error
) {
    error.clear();
    hashedBytes = 0U;
    std::error_code code;
    const auto measured = std::filesystem::file_size(path, code);
    if (code) {
        error = "cannot size media file";
        return 0U;
    }
    // std::uintmax_t is not std::uint64_t on every platform (it is
    // unsigned long on libc++, unsigned long long here), so narrow once
    // explicitly rather than relying on template deduction.
    const auto fileSize = static_cast<std::uint64_t>(measured);
    std::ifstream stream {path, std::ios::binary};
    if (!stream) {
        error = "cannot open media file";
        return 0U;
    }
    try {
        std::array<char, kHashChunkBytes> chunk {};
        std::uint64_t hash = kFnvOffsetBasis;
        std::uint64_t remaining =
            std::min<std::uint64_t>(fileSize, maximumBytes);
        while (remaining > 0U) {
            const auto wanted = static_cast<std::streamsize>(
                std::min<std::uint64_t>(remaining, chunk.size())
            );
            stream.read(chunk.data(), wanted);
            const auto read = stream.gcount();
            if (read <= 0) {
                break;
            }
            for (std::streamsize index = 0; index < read; ++index) {
                hash ^= static_cast<std::uint64_t>(
                    static_cast<unsigned char>(chunk[
                        static_cast<std::size_t>(index)
                    ])
                );
                hash *= kFnvPrime;
            }
            remaining -= static_cast<std::uint64_t>(read);
            hashedBytes += static_cast<std::uint64_t>(read);
        }
        if (stream.bad()) {
            error = "cannot read media file";
            return 0U;
        }
        // Mixing the full length in means a file whose first
        // maximumBytes match but which was extended or truncated past the
        // bound still fails verification.
        hash = mixHash(hash, fileSize);
        // A zero hash is the document's "unknown" sentinel, so never
        // return it for a file that was successfully read.
        return hash == 0U ? kFnvOffsetBasis : hash;
    } catch (const std::bad_alloc&) {
        error = "cannot allocate media hash buffer";
        return 0U;
    }
}

MediaResolutionReport resolveSessionMedia(
    const SessionDocument& document,
    const MediaResolverConfig& config
) {
    MediaResolutionReport report;
    report.resolutions.reserve(document.mediaSources.size());
    for (const auto& source : document.mediaSources) {
        MediaResolution resolution;
        resolution.sourceId = source.stableId;

        const auto declared =
            absoluteCandidate(source.path, config.projectRoot);
        if (isRegularFile(declared)) {
            std::string error;
            std::uint64_t hashedBytes = 0U;
            const auto hash = hashMediaFile(
                declared,
                config.maximumHashBytes,
                hashedBytes,
                error
            );
            if (error.empty()) {
                resolution.resolvedPath = declared;
                resolution.observedHash = hash;
                resolution.observedBytes = hashedBytes;
                if (source.contentHash == 0U) {
                    resolution.status =
                        MediaResolutionStatus::unverifiable;
                    ++report.unverifiableCount;
                } else if (hash == source.contentHash) {
                    resolution.status =
                        MediaResolutionStatus::verified;
                    ++report.verifiedCount;
                } else {
                    resolution.status =
                        MediaResolutionStatus::mismatched;
                    ++report.mismatchedCount;
                }
                report.resolutions.push_back(std::move(resolution));
                continue;
            }
        }

        auto relocated = findByFilename(
            std::filesystem::path {source.path}.filename(),
            source.contentHash,
            config,
            report.searchEntriesScanned,
            report.searchBudgetExhausted,
            resolution.observedHash,
            resolution.observedBytes
        );
        if (!relocated.empty()) {
            resolution.resolvedPath = std::move(relocated);
            resolution.status = MediaResolutionStatus::relocated;
            ++report.relocatedCount;
        } else {
            resolution.status = MediaResolutionStatus::missing;
            ++report.missingCount;
        }
        report.resolutions.push_back(std::move(resolution));
    }
    return report;
}

std::size_t applyMediaResolution(
    SessionDocument& document,
    const MediaResolutionReport& report,
    const MediaResolverConfig& config
) {
    std::size_t applied = 0U;
    for (const auto& resolution : report.resolutions) {
        if (resolution.status != MediaResolutionStatus::relocated
            || resolution.resolvedPath.empty()) {
            continue;
        }
        const auto source = std::find_if(
            document.mediaSources.begin(),
            document.mediaSources.end(),
            [&resolution](const SessionMediaSource& candidate) {
                return candidate.stableId == resolution.sourceId;
            }
        );
        if (source == document.mediaSources.end()) {
            continue;
        }
        // Prefer a project-relative path so a relinked project stays
        // portable, but never fabricate one that escapes the root.
        std::filesystem::path rewritten = resolution.resolvedPath;
        if (!config.projectRoot.empty()) {
            std::error_code code;
            auto relative = std::filesystem::relative(
                resolution.resolvedPath,
                config.projectRoot,
                code
            );
            if (!code && !relative.empty()
                && *relative.begin() != "..") {
                rewritten = std::move(relative);
            }
        }
        auto text = rewritten.generic_string();
        if (text.empty() || text.size() > kMaximumPathBytes) {
            continue;
        }
        source->path = std::move(text);
        ++applied;
    }
    return applied;
}

} // namespace iramix::persistence
