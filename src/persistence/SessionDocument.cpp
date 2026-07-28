#include "iramix/persistence/SessionDocument.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

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
constexpr std::uint32_t kMaximumClipCount = 1'000'000U;
constexpr std::uint32_t kMaximumRouteCount = 1'000'000U;
constexpr std::uint32_t kMaximumAutomationLaneCount = 100'000U;
constexpr std::uint64_t kMaximumAutomationPointCount = 10'000'000U;
constexpr std::uint32_t kMaximumNameBytes = 4'096U;
constexpr std::uint32_t kMigratedSampleRate = 48'000U;

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

void appendString(
    std::vector<std::byte>& bytes,
    const std::string& value
) {
    appendU32(bytes, static_cast<std::uint32_t>(value.size()));
    const auto* const begin =
        reinterpret_cast<const std::byte*>(value.data());
    bytes.insert(bytes.end(), begin, begin + value.size());
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

[[nodiscard]] bool readString(
    const std::span<const std::byte> bytes,
    std::size_t& offset,
    std::string& value
) {
    std::uint32_t size = 0U;
    if (!readU32(bytes, offset, size)
        || size > kMaximumNameBytes
        || offset > bytes.size()
        || size > bytes.size() - offset) {
        return false;
    }
    value.assign(
        reinterpret_cast<const char*>(
            bytes.data() + static_cast<std::ptrdiff_t>(offset)
        ),
        size
    );
    offset += size;
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

[[nodiscard]] bool validParameter(
    const SessionParameterId parameter
) noexcept {
    switch (parameter) {
    case SessionParameterId::gain:
    case SessionParameterId::pan:
    case SessionParameterId::mute:
        return true;
    }
    return false;
}

[[nodiscard]] bool insertStableId(
    std::unordered_set<std::uint64_t>& stableIds,
    const std::uint64_t id,
    const char* const entity,
    std::string& error
) {
    if (id == 0U || !stableIds.insert(id).second) {
        error = std::string {"session "} + entity
            + " IDs must be non-zero and globally unique";
        return false;
    }
    return true;
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
    if (document.tracks.size() > kMaximumTrackCount
        || document.clips.size() > kMaximumClipCount
        || document.routes.size() > kMaximumRouteCount
        || document.automationLanes.size()
            > kMaximumAutomationLaneCount) {
        error = "session entity count exceeds format limit";
        return false;
    }

    const std::size_t entityCount =
        document.tracks.size() + document.clips.size()
        + document.routes.size()
        + document.automationLanes.size();
    std::unordered_set<std::uint64_t> stableIds;
    stableIds.reserve(entityCount);
    std::unordered_set<std::uint64_t> trackIds;
    trackIds.reserve(document.tracks.size());

    for (const auto& track : document.tracks) {
        if (!insertStableId(
                stableIds,
                track.stableId,
                "track",
                error
            )
            || !trackIds.insert(track.stableId).second) {
            return false;
        }
        if (!validTrackType(track.type)
            || !std::isfinite(track.gain)
            || track.name.size() > kMaximumNameBytes) {
            error = "session track fields are invalid";
            return false;
        }
    }

    for (const auto& clip : document.clips) {
        if (!insertStableId(
                stableIds,
                clip.stableId,
                "clip",
                error
            )) {
            return false;
        }
        if (!trackIds.contains(clip.trackId)
            || clip.sourceId == 0U
            || clip.lengthFrames == 0U
            || clip.startFrame
                > std::numeric_limits<std::uint64_t>::max()
                    - clip.lengthFrames
            || !std::isfinite(clip.gain)
            || clip.name.size() > kMaximumNameBytes) {
            error = "session clip fields or references are invalid";
            return false;
        }
    }

    for (const auto& route : document.routes) {
        if (!insertStableId(
                stableIds,
                route.stableId,
                "route",
                error
            )) {
            return false;
        }
        if (!trackIds.contains(route.sourceTrackId)
            || !trackIds.contains(route.destinationTrackId)
            || route.sourceTrackId == route.destinationTrackId
            || !std::isfinite(route.gain)) {
            error = "session route fields or references are invalid";
            return false;
        }
    }

    std::uint64_t totalPointCount = 0U;
    for (const auto& lane : document.automationLanes) {
        if (!insertStableId(
                stableIds,
                lane.stableId,
                "automation lane",
                error
            )) {
            return false;
        }
        if (!trackIds.contains(lane.targetTrackId)
            || !validParameter(lane.parameter)
            || lane.points.size()
                > kMaximumAutomationPointCount - totalPointCount) {
            error =
                "session automation fields or references are invalid";
            return false;
        }
        totalPointCount += lane.points.size();
        bool hasPrevious = false;
        std::uint64_t previousPosition = 0U;
        for (const auto& point : lane.points) {
            if (!std::isfinite(point.value)
                || (hasPrevious
                    && point.samplePosition <= previousPosition)) {
                error =
                    "session automation points must be finite and ordered";
                return false;
            }
            previousPosition = point.samplePosition;
            hasPrevious = true;
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
    if (targetSchemaVersion < 3U
        && (!document.clips.empty()
            || !document.routes.empty()
            || !document.automationLanes.empty())) {
        error = "legacy session export would discard schema v3 entities";
        return {};
    }
    try {
        if (!validateDocument(document, error)) {
            return {};
        }

        std::size_t estimatedBytes = 48U;
        for (const auto& track : document.tracks) {
            estimatedBytes += 24U + track.name.size();
        }
        for (const auto& clip : document.clips) {
            estimatedBytes += 64U + clip.name.size();
        }
        estimatedBytes += document.routes.size() * 40U;
        for (const auto& lane : document.automationLanes) {
            estimatedBytes += 24U + lane.points.size() * 12U;
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
        appendU64(
            bytes,
            std::bit_cast<std::uint64_t>(document.tempo)
        );
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
            appendString(bytes, track.name);
        }

        if (targetSchemaVersion >= 3U) {
            appendU32(
                bytes,
                static_cast<std::uint32_t>(document.clips.size())
            );
            for (const auto& clip : document.clips) {
                appendU64(bytes, clip.stableId);
                appendU64(bytes, clip.trackId);
                appendU64(bytes, clip.sourceId);
                appendU64(bytes, clip.startFrame);
                appendU64(bytes, clip.lengthFrames);
                appendU64(bytes, clip.sourceOffsetFrames);
                appendU32(
                    bytes,
                    std::bit_cast<std::uint32_t>(clip.gain)
                );
                appendU32(bytes, clip.muted ? 1U : 0U);
                appendString(bytes, clip.name);
            }

            appendU32(
                bytes,
                static_cast<std::uint32_t>(document.routes.size())
            );
            for (const auto& route : document.routes) {
                appendU64(bytes, route.stableId);
                appendU64(bytes, route.sourceTrackId);
                appendU64(bytes, route.destinationTrackId);
                appendU32(
                    bytes,
                    std::bit_cast<std::uint32_t>(route.gain)
                );
                appendU32(bytes, route.enabled ? 1U : 0U);
            }

            appendU32(
                bytes,
                static_cast<std::uint32_t>(
                    document.automationLanes.size()
                )
            );
            for (const auto& lane : document.automationLanes) {
                appendU64(bytes, lane.stableId);
                appendU64(bytes, lane.targetTrackId);
                appendU32(
                    bytes,
                    static_cast<std::uint32_t>(lane.parameter)
                );
                appendU32(
                    bytes,
                    static_cast<std::uint32_t>(lane.points.size())
                );
                for (const auto& point : lane.points) {
                    appendU64(bytes, point.samplePosition);
                    appendU32(
                        bytes,
                        std::bit_cast<std::uint32_t>(point.value)
                    );
                }
            }
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
            }
            if (!readString(bytes, offset, track.name)) {
                result.error = "invalid session track name";
                return result;
            }
            result.document.tracks.push_back(std::move(track));
        }

        if (result.sourceSchemaVersion >= 3U) {
            std::uint32_t clipCount = 0U;
            if (!readU32(bytes, offset, clipCount)
                || clipCount > kMaximumClipCount) {
                result.error = "invalid session clip count";
                return result;
            }
            result.document.clips.reserve(clipCount);
            for (std::uint32_t index = 0U; index < clipCount; ++index) {
                SessionClip clip;
                std::uint32_t gainBits = 0U;
                std::uint32_t muted = 0U;
                if (!readU64(bytes, offset, clip.stableId)
                    || !readU64(bytes, offset, clip.trackId)
                    || !readU64(bytes, offset, clip.sourceId)
                    || !readU64(bytes, offset, clip.startFrame)
                    || !readU64(bytes, offset, clip.lengthFrames)
                    || !readU64(
                        bytes,
                        offset,
                        clip.sourceOffsetFrames
                    )
                    || !readU32(bytes, offset, gainBits)
                    || !readU32(bytes, offset, muted)
                    || muted > 1U
                    || !readString(bytes, offset, clip.name)) {
                    result.error = "invalid or truncated session clip";
                    return result;
                }
                clip.gain = std::bit_cast<float>(gainBits);
                clip.muted = muted != 0U;
                result.document.clips.push_back(std::move(clip));
            }

            std::uint32_t routeCount = 0U;
            if (!readU32(bytes, offset, routeCount)
                || routeCount > kMaximumRouteCount) {
                result.error = "invalid session route count";
                return result;
            }
            result.document.routes.reserve(routeCount);
            for (std::uint32_t index = 0U; index < routeCount; ++index) {
                SessionRoute route;
                std::uint32_t gainBits = 0U;
                std::uint32_t enabled = 0U;
                if (!readU64(bytes, offset, route.stableId)
                    || !readU64(bytes, offset, route.sourceTrackId)
                    || !readU64(
                        bytes,
                        offset,
                        route.destinationTrackId
                    )
                    || !readU32(bytes, offset, gainBits)
                    || !readU32(bytes, offset, enabled)
                    || enabled > 1U) {
                    result.error = "invalid or truncated session route";
                    return result;
                }
                route.gain = std::bit_cast<float>(gainBits);
                route.enabled = enabled != 0U;
                result.document.routes.push_back(route);
            }

            std::uint32_t laneCount = 0U;
            if (!readU32(bytes, offset, laneCount)
                || laneCount > kMaximumAutomationLaneCount) {
                result.error = "invalid session automation count";
                return result;
            }
            result.document.automationLanes.reserve(laneCount);
            std::uint64_t totalPointCount = 0U;
            for (std::uint32_t index = 0U; index < laneCount; ++index) {
                SessionAutomationLane lane;
                std::uint32_t parameter = 0U;
                std::uint32_t pointCount = 0U;
                if (!readU64(bytes, offset, lane.stableId)
                    || !readU64(bytes, offset, lane.targetTrackId)
                    || !readU32(bytes, offset, parameter)
                    || !readU32(bytes, offset, pointCount)
                    || pointCount
                        > kMaximumAutomationPointCount
                            - totalPointCount) {
                    result.error =
                        "invalid or truncated automation lane";
                    return result;
                }
                totalPointCount += pointCount;
                lane.parameter =
                    static_cast<SessionParameterId>(parameter);
                lane.points.reserve(pointCount);
                for (
                    std::uint32_t pointIndex = 0U;
                    pointIndex < pointCount;
                    ++pointIndex
                ) {
                    SessionAutomationPoint point;
                    std::uint32_t valueBits = 0U;
                    if (!readU64(
                            bytes,
                            offset,
                            point.samplePosition
                        )
                        || !readU32(bytes, offset, valueBits)) {
                        result.error =
                            "truncated session automation point";
                        return result;
                    }
                    point.value = std::bit_cast<float>(valueBits);
                    lane.points.push_back(point);
                }
                result.document.automationLanes.push_back(
                    std::move(lane)
                );
            }
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
