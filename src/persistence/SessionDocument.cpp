#include "iramix/persistence/SessionDocument.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace iramix::persistence {
namespace {

[[nodiscard]] constexpr std::byte asByte(const char value) noexcept {
    return static_cast<std::byte>(
        static_cast<unsigned char>(value)
    );
}

constexpr std::array<std::byte, 4> kSessionMagic {
    asByte('I'), asByte('R'), asByte('S'), asByte('D'),
};
constexpr std::uint32_t kMinimumSchemaVersion = 1U;
constexpr std::uint32_t kMaximumTrackCount = 100'000U;
constexpr std::uint32_t kMaximumTrackNameBytes = 4'096U;
constexpr std::uint32_t kMigratedSampleRate = 48'000U;
constexpr std::uint32_t kMigratedTrackColor = 0U;

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

[[nodiscard]] bool validTrackType(
    const SessionTrackType type
) noexcept {
    switch (type) {
    case SessionTrackType::audio:
    case SessionTrackType::instrument:
    case SessionTrackType::group:
    case SessionTrackType::effectReturn:
    case SessionTrackType::master:
        return true;
    }
    return false;
}

[[nodiscard]] bool validateDocument(
    const SessionDocument& document,
    std::string& error
) {
    if (document.revision == 0U) {
        error = "session revision must be non-zero";
        return false;
    }
    if (document.sampleRate < 8'000U
        || document.sampleRate > 768'000U) {
        error = "session sample rate is outside supported bounds";
        return false;
    }
    if (!std::isfinite(document.tempo)
        || document.tempo <= 0.0
        || document.tempo > 1'000.0) {
        error = "session tempo is invalid";
        return false;
    }
    if (document.tracks.size() > kMaximumTrackCount) {
        error = "session track count exceeds format limit";
        return false;
    }

    std::unordered_set<std::uint64_t> stableIds;
    stableIds.reserve(document.tracks.size());
    for (const auto& track : document.tracks) {
        if (track.stableId == 0U
            || !stableIds.insert(track.stableId).second) {
            error = "session track IDs must be non-zero and unique";
            return false;
        }
        if (!validTrackType(track.type)) {
            error = "session track type is invalid";
            return false;
        }
        if (!std::isfinite(track.gain)) {
            error = "session track gain is invalid";
            return false;
        }
        if (track.name.size() > kMaximumTrackNameBytes) {
            error = "session track name exceeds format limit";
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<std::byte> serializeSessionDocument(
    const SessionDocument& document,
    std::string& error,
    const std::uint32_t targetSchemaVersion
) {
    error.clear();
    if (targetSchemaVersion < kMinimumSchemaVersion
        || targetSchemaVersion > currentSessionSchemaVersion) {
        error = "unsupported session target schema";
        return {};
    }
    try {
        if (!validateDocument(document, error)) {
            return {};
        }

        std::size_t estimatedBytes = 32U;
        for (const auto& track : document.tracks) {
            estimatedBytes += 24U + track.name.size();
        }
        std::vector<std::byte> bytes;
        bytes.reserve(estimatedBytes);
        bytes.insert(
            bytes.end(),
            kSessionMagic.begin(),
            kSessionMagic.end()
        );
        appendU32(bytes, targetSchemaVersion);
        appendU64(bytes, document.revision);
        if (targetSchemaVersion >= 2U) {
            appendU32(bytes, document.sampleRate);
        }
        appendU64(bytes, std::bit_cast<std::uint64_t>(
            document.tempo
        ));
        appendU32(
            bytes,
            static_cast<std::uint32_t>(document.tracks.size())
        );
        for (const auto& track : document.tracks) {
            appendU64(bytes, track.stableId);
            appendU32(bytes, static_cast<std::uint32_t>(track.type));
            appendU32(
                bytes,
                std::bit_cast<std::uint32_t>(track.gain)
            );
            if (targetSchemaVersion >= 2U) {
                appendU32(bytes, track.color);
            }
            appendU32(
                bytes,
                static_cast<std::uint32_t>(track.name.size())
            );
            const auto* const begin =
                reinterpret_cast<const std::byte*>(
                    track.name.data()
                );
            bytes.insert(
                bytes.end(),
                begin,
                begin + track.name.size()
            );
        }
        return bytes;
    } catch (const std::bad_alloc&) {
        error = "cannot allocate serialized session document";
        return {};
    }
}

SessionDecodeResult deserializeSessionDocument(
    const std::span<const std::byte> bytes
) {
    SessionDecodeResult result;
    try {
        std::size_t offset = 0U;
        if (bytes.size() < kSessionMagic.size()
            || !std::equal(
                kSessionMagic.begin(),
                kSessionMagic.end(),
                bytes.begin()
            )) {
            result.error = "invalid session document magic";
            return result;
        }
        offset += kSessionMagic.size();
        if (!readU32(bytes, offset, result.sourceSchemaVersion)
            || result.sourceSchemaVersion < kMinimumSchemaVersion
            || result.sourceSchemaVersion
                > currentSessionSchemaVersion
            || !readU64(
                bytes,
                offset,
                result.document.revision
            )) {
            result.error = "unsupported or truncated session schema";
            return result;
        }

        if (result.sourceSchemaVersion >= 2U) {
            if (!readU32(
                bytes,
                offset,
                result.document.sampleRate
            )) {
                result.error = "truncated session sample rate";
                return result;
            }
        } else {
            result.document.sampleRate = kMigratedSampleRate;
        }

        std::uint64_t tempoBits = 0U;
        std::uint32_t trackCount = 0U;
        if (!readU64(bytes, offset, tempoBits)
            || !readU32(bytes, offset, trackCount)
            || trackCount > kMaximumTrackCount) {
            result.error = "invalid session document header";
            return result;
        }
        result.document.tempo = std::bit_cast<double>(tempoBits);
        result.document.tracks.reserve(trackCount);

        for (std::uint32_t index = 0U; index < trackCount; ++index) {
            SessionTrack track;
            std::uint32_t type = 0U;
            std::uint32_t gainBits = 0U;
            std::uint32_t nameBytes = 0U;
            if (!readU64(bytes, offset, track.stableId)
                || !readU32(bytes, offset, type)
                || !readU32(bytes, offset, gainBits)) {
                result.error = "truncated session track";
                return result;
            }
            track.type = static_cast<SessionTrackType>(type);
            track.gain = std::bit_cast<float>(gainBits);
            if (result.sourceSchemaVersion >= 2U) {
                if (!readU32(bytes, offset, track.color)) {
                    result.error = "truncated session track color";
                    return result;
                }
            } else {
                track.color = kMigratedTrackColor;
            }
            if (!readU32(bytes, offset, nameBytes)
                || nameBytes > kMaximumTrackNameBytes
                || offset > bytes.size()
                || nameBytes > bytes.size() - offset) {
                result.error = "invalid session track name";
                return result;
            }
            track.name.assign(
                reinterpret_cast<const char*>(
                    bytes.data()
                        + static_cast<std::ptrdiff_t>(offset)
                ),
                nameBytes
            );
            offset += nameBytes;
            result.document.tracks.push_back(std::move(track));
        }
        if (offset != bytes.size()) {
            result.error = "session document has trailing bytes";
            return result;
        }

        std::string validationError;
        if (!validateDocument(result.document, validationError)) {
            result.error = "invalid session document: "
                + validationError;
            return result;
        }
        result.migrated = result.sourceSchemaVersion
            < currentSessionSchemaVersion;
        return result;
    } catch (const std::bad_alloc&) {
        result.error = "cannot allocate decoded session document";
        return result;
    }
}

} // namespace iramix::persistence
