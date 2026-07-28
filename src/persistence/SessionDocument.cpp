#include "iramix/persistence/SessionDocument.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>
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
constexpr std::uint32_t kMaximumMediaSourceCount = 1'000'000U;
constexpr std::uint32_t kMaximumMidiSequenceCount = 1'000'000U;
constexpr std::uint64_t kMaximumMidiNoteCount = 10'000'000U;
constexpr std::uint32_t kMaximumPluginCount = 100'000U;
constexpr std::uint32_t kMaximumChannelCount = 1'024U;
constexpr std::uint32_t kMaximumBufferFrames = 65'536U;
constexpr std::uint32_t kMaximumMidiChannel = 15U;
constexpr std::uint32_t kMaximumMidiKey = 127U;
constexpr std::uint32_t kMaximumPluginStateBytes = 16'777'216U;
constexpr std::uint64_t kMaximumTotalPluginStateBytes = 268'435'456U;

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

void appendBlob(
    std::vector<std::byte>& bytes,
    const std::vector<std::byte>& value
) {
    appendU32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
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

[[nodiscard]] bool readBlob(
    const std::span<const std::byte> bytes,
    std::size_t& offset,
    std::vector<std::byte>& value,
    const std::uint64_t remainingBudget
) {
    std::uint32_t size = 0U;
    if (!readU32(bytes, offset, size)
        || size > kMaximumPluginStateBytes
        || size > remainingBudget
        || offset > bytes.size()
        || size > bytes.size() - offset) {
        return false;
    }
    value.assign(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)
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

[[nodiscard]] bool validBackend(
    const SessionAudioBackend backend
) noexcept {
    switch (backend) {
    case SessionAudioBackend::unspecified:
    case SessionAudioBackend::wasapi:
    case SessionAudioBackend::asio:
    case SessionAudioBackend::coreAudio:
    case SessionAudioBackend::alsa:
    case SessionAudioBackend::jack:
        return true;
    }
    return false;
}

[[nodiscard]] bool validPluginFormat(
    const SessionPluginFormat format
) noexcept {
    switch (format) {
    case SessionPluginFormat::internal:
    case SessionPluginFormat::vst3:
    case SessionPluginFormat::clap:
        return true;
    }
    return false;
}

[[nodiscard]] bool validOptionalSampleRate(
    const std::uint32_t sampleRate
) noexcept {
    return sampleRate == 0U
        || (sampleRate >= 8'000U && sampleRate <= 768'000U);
}

// A placeholder carries nothing beyond the identity a v3 clip already
// encodes, so dropping it on legacy export discards no project data.
[[nodiscard]] bool isPlaceholderMediaSource(
    const SessionMediaSource& source
) noexcept {
    return source.contentHash == 0U
        && source.frameCount == 0U
        && source.sampleRate == 0U
        && source.channelCount == 0U
        && source.path.empty()
        && source.name.empty();
}

[[nodiscard]] bool isDefaultDeviceConfiguration(
    const SessionDeviceConfiguration& device
) noexcept {
    return device.backend == SessionAudioBackend::unspecified
        && device.sampleRate == 0U
        && device.bufferFrames == 0U
        && device.inputChannelCount == 0U
        && device.outputChannelCount == 0U
        && device.inputDeviceId.empty()
        && device.outputDeviceId.empty();
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
            > kMaximumAutomationLaneCount
        || document.mediaSources.size() > kMaximumMediaSourceCount
        || document.midiSequences.size()
            > kMaximumMidiSequenceCount
        || document.plugins.size() > kMaximumPluginCount) {
        error = "session entity count exceeds format limit";
        return false;
    }

    const std::size_t entityCount =
        document.tracks.size() + document.clips.size()
        + document.routes.size()
        + document.automationLanes.size()
        + document.mediaSources.size()
        + document.midiSequences.size()
        + document.plugins.size();
    std::unordered_set<std::uint64_t> stableIds;
    stableIds.reserve(entityCount);
    std::unordered_set<std::uint64_t> trackIds;
    trackIds.reserve(document.tracks.size());
    std::unordered_set<std::uint64_t> sourceIds;
    sourceIds.reserve(
        document.mediaSources.size() + document.midiSequences.size()
    );

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

    for (const auto& source : document.mediaSources) {
        if (!insertStableId(
                stableIds,
                source.stableId,
                "media source",
                error
            )) {
            return false;
        }
        sourceIds.insert(source.stableId);
        if (!validOptionalSampleRate(source.sampleRate)
            || source.channelCount > kMaximumChannelCount
            || source.path.size() > kMaximumNameBytes
            || source.name.size() > kMaximumNameBytes) {
            error = "session media source fields are invalid";
            return false;
        }
    }

    std::uint64_t totalNoteCount = 0U;
    for (const auto& sequence : document.midiSequences) {
        if (!insertStableId(
                stableIds,
                sequence.stableId,
                "MIDI sequence",
                error
            )) {
            return false;
        }
        sourceIds.insert(sequence.stableId);
        if (sequence.name.size() > kMaximumNameBytes
            || sequence.notes.size()
                > kMaximumMidiNoteCount - totalNoteCount) {
            error = "session MIDI sequence fields are invalid";
            return false;
        }
        totalNoteCount += sequence.notes.size();
        bool hasPrevious = false;
        SessionMidiNote previous;
        for (const auto& note : sequence.notes) {
            if (note.lengthFrames == 0U
                || note.startFrame
                    > std::numeric_limits<std::uint64_t>::max()
                        - note.lengthFrames
                || note.channel > kMaximumMidiChannel
                || note.key > kMaximumMidiKey
                || !std::isfinite(note.velocity)
                || note.velocity <= 0.0F
                || note.velocity > 1.0F) {
                error = "session MIDI note fields are invalid";
                return false;
            }
            // Total order on (startFrame, channel, key) keeps decoded
            // sequences byte-identical and forbids duplicate notes.
            if (hasPrevious
                && std::tuple {
                        previous.startFrame,
                        previous.channel,
                        previous.key,
                    }
                    >= std::tuple {
                        note.startFrame,
                        note.channel,
                        note.key,
                    }) {
                error =
                    "session MIDI notes must be strictly ordered by "
                    "position, channel, and key";
                return false;
            }
            previous = note;
            hasPrevious = true;
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
            || !sourceIds.contains(clip.sourceId)
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

    std::uint64_t totalStateBytes = 0U;
    std::set<std::pair<std::uint64_t, std::uint32_t>> pluginSlots;
    for (const auto& plugin : document.plugins) {
        if (!insertStableId(
                stableIds,
                plugin.stableId,
                "plugin",
                error
            )) {
            return false;
        }
        if (!trackIds.contains(plugin.targetTrackId)
            || !validPluginFormat(plugin.format)
            || plugin.identifier.empty()
            || plugin.identifier.size() > kMaximumNameBytes
            || plugin.name.size() > kMaximumNameBytes
            || plugin.state.size() > kMaximumPluginStateBytes
            || plugin.state.size()
                > kMaximumTotalPluginStateBytes - totalStateBytes) {
            error = "session plugin fields or references are invalid";
            return false;
        }
        totalStateBytes += plugin.state.size();
        // Slot occupancy must be unique per track or the restored chain
        // order would depend on decode order rather than the document.
        if (!pluginSlots
                .insert({plugin.targetTrackId, plugin.slotIndex})
                .second) {
            error = "session plugin slots must be unique per track";
            return false;
        }
    }

    if (!validBackend(document.device.backend)
        || !validOptionalSampleRate(document.device.sampleRate)
        || document.device.bufferFrames > kMaximumBufferFrames
        || document.device.inputChannelCount > kMaximumChannelCount
        || document.device.outputChannelCount > kMaximumChannelCount
        || document.device.inputDeviceId.size() > kMaximumNameBytes
        || document.device.outputDeviceId.size()
            > kMaximumNameBytes) {
        error = "session device configuration is invalid";
        return false;
    }
    // A device ID without a backend cannot be resolved on restore, so
    // reject it rather than silently opening the wrong hardware.
    if (document.device.backend == SessionAudioBackend::unspecified
        && (!document.device.inputDeviceId.empty()
            || !document.device.outputDeviceId.empty())) {
        error =
            "session device IDs require an explicit audio backend";
        return false;
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
    if (targetSchemaVersion < 4U) {
        // Placeholder media sources are recoverable from the clips that
        // reference them, so only describable state blocks the export.
        std::unordered_set<std::uint64_t> clipSourceIds;
        clipSourceIds.reserve(document.clips.size());
        for (const auto& clip : document.clips) {
            clipSourceIds.insert(clip.sourceId);
        }
        const bool describedMedia = std::any_of(
            document.mediaSources.begin(),
            document.mediaSources.end(),
            [&clipSourceIds](const SessionMediaSource& source) {
                return !isPlaceholderMediaSource(source)
                    || !clipSourceIds.contains(source.stableId);
            }
        );
        if (describedMedia
            || !document.midiSequences.empty()
            || !document.plugins.empty()
            || !isDefaultDeviceConfiguration(document.device)) {
            error =
                "legacy session export would discard schema v4 entities";
            return {};
        }
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
        if (targetSchemaVersion >= 4U) {
            for (const auto& source : document.mediaSources) {
                estimatedBytes +=
                    40U + source.path.size() + source.name.size();
            }
            for (const auto& sequence : document.midiSequences) {
                estimatedBytes += 16U + sequence.name.size()
                    + sequence.notes.size() * 28U;
            }
            for (const auto& plugin : document.plugins) {
                estimatedBytes += 32U + plugin.identifier.size()
                    + plugin.name.size() + plugin.state.size();
            }
            estimatedBytes += 32U
                + document.device.inputDeviceId.size()
                + document.device.outputDeviceId.size();
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

        if (targetSchemaVersion >= 4U) {
            appendU32(
                bytes,
                static_cast<std::uint32_t>(
                    document.mediaSources.size()
                )
            );
            for (const auto& source : document.mediaSources) {
                appendU64(bytes, source.stableId);
                appendU64(bytes, source.contentHash);
                appendU64(bytes, source.frameCount);
                appendU32(bytes, source.sampleRate);
                appendU32(bytes, source.channelCount);
                appendString(bytes, source.path);
                appendString(bytes, source.name);
            }

            appendU32(
                bytes,
                static_cast<std::uint32_t>(
                    document.midiSequences.size()
                )
            );
            for (const auto& sequence : document.midiSequences) {
                appendU64(bytes, sequence.stableId);
                appendString(bytes, sequence.name);
                appendU32(
                    bytes,
                    static_cast<std::uint32_t>(sequence.notes.size())
                );
                for (const auto& note : sequence.notes) {
                    appendU64(bytes, note.startFrame);
                    appendU64(bytes, note.lengthFrames);
                    appendU32(bytes, note.channel);
                    appendU32(bytes, note.key);
                    appendU32(
                        bytes,
                        std::bit_cast<std::uint32_t>(note.velocity)
                    );
                }
            }

            appendU32(
                bytes,
                static_cast<std::uint32_t>(document.device.backend)
            );
            appendU32(bytes, document.device.sampleRate);
            appendU32(bytes, document.device.bufferFrames);
            appendU32(bytes, document.device.inputChannelCount);
            appendU32(bytes, document.device.outputChannelCount);
            appendString(bytes, document.device.inputDeviceId);
            appendString(bytes, document.device.outputDeviceId);

            appendU32(
                bytes,
                static_cast<std::uint32_t>(document.plugins.size())
            );
            for (const auto& plugin : document.plugins) {
                appendU64(bytes, plugin.stableId);
                appendU64(bytes, plugin.targetTrackId);
                appendU32(
                    bytes,
                    static_cast<std::uint32_t>(plugin.format)
                );
                appendU32(bytes, plugin.slotIndex);
                appendU32(bytes, plugin.bypassed ? 1U : 0U);
                appendString(bytes, plugin.identifier);
                appendString(bytes, plugin.name);
                appendBlob(bytes, plugin.state);
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

        if (result.sourceSchemaVersion >= 4U) {
            std::uint32_t mediaSourceCount = 0U;
            if (!readU32(bytes, offset, mediaSourceCount)
                || mediaSourceCount > kMaximumMediaSourceCount) {
                result.error = "invalid session media source count";
                return result;
            }
            result.document.mediaSources.reserve(mediaSourceCount);
            for (
                std::uint32_t index = 0U;
                index < mediaSourceCount;
                ++index
            ) {
                SessionMediaSource source;
                if (!readU64(bytes, offset, source.stableId)
                    || !readU64(bytes, offset, source.contentHash)
                    || !readU64(bytes, offset, source.frameCount)
                    || !readU32(bytes, offset, source.sampleRate)
                    || !readU32(bytes, offset, source.channelCount)
                    || !readString(bytes, offset, source.path)
                    || !readString(bytes, offset, source.name)) {
                    result.error =
                        "invalid or truncated session media source";
                    return result;
                }
                result.document.mediaSources.push_back(
                    std::move(source)
                );
            }

            std::uint32_t midiSequenceCount = 0U;
            if (!readU32(bytes, offset, midiSequenceCount)
                || midiSequenceCount > kMaximumMidiSequenceCount) {
                result.error = "invalid session MIDI sequence count";
                return result;
            }
            result.document.midiSequences.reserve(midiSequenceCount);
            std::uint64_t totalNoteCount = 0U;
            for (
                std::uint32_t index = 0U;
                index < midiSequenceCount;
                ++index
            ) {
                SessionMidiSequence sequence;
                std::uint32_t noteCount = 0U;
                if (!readU64(bytes, offset, sequence.stableId)
                    || !readString(bytes, offset, sequence.name)
                    || !readU32(bytes, offset, noteCount)
                    || noteCount
                        > kMaximumMidiNoteCount - totalNoteCount) {
                    result.error =
                        "invalid or truncated MIDI sequence";
                    return result;
                }
                totalNoteCount += noteCount;
                sequence.notes.reserve(noteCount);
                for (
                    std::uint32_t noteIndex = 0U;
                    noteIndex < noteCount;
                    ++noteIndex
                ) {
                    SessionMidiNote note;
                    std::uint32_t velocityBits = 0U;
                    if (!readU64(bytes, offset, note.startFrame)
                        || !readU64(bytes, offset, note.lengthFrames)
                        || !readU32(bytes, offset, note.channel)
                        || !readU32(bytes, offset, note.key)
                        || !readU32(bytes, offset, velocityBits)) {
                        result.error =
                            "truncated session MIDI note";
                        return result;
                    }
                    note.velocity =
                        std::bit_cast<float>(velocityBits);
                    sequence.notes.push_back(note);
                }
                result.document.midiSequences.push_back(
                    std::move(sequence)
                );
            }

            auto& device = result.document.device;
            std::uint32_t backend = 0U;
            if (!readU32(bytes, offset, backend)
                || !readU32(bytes, offset, device.sampleRate)
                || !readU32(bytes, offset, device.bufferFrames)
                || !readU32(bytes, offset, device.inputChannelCount)
                || !readU32(bytes, offset, device.outputChannelCount)
                || !readString(bytes, offset, device.inputDeviceId)
                || !readString(
                    bytes,
                    offset,
                    device.outputDeviceId
                )) {
                result.error =
                    "invalid or truncated session device "
                    "configuration";
                return result;
            }
            device.backend = static_cast<SessionAudioBackend>(backend);

            std::uint32_t pluginCount = 0U;
            if (!readU32(bytes, offset, pluginCount)
                || pluginCount > kMaximumPluginCount) {
                result.error = "invalid session plugin count";
                return result;
            }
            result.document.plugins.reserve(pluginCount);
            std::uint64_t totalStateBytes = 0U;
            for (
                std::uint32_t index = 0U;
                index < pluginCount;
                ++index
            ) {
                SessionPlugin plugin;
                std::uint32_t format = 0U;
                std::uint32_t bypassed = 0U;
                if (!readU64(bytes, offset, plugin.stableId)
                    || !readU64(bytes, offset, plugin.targetTrackId)
                    || !readU32(bytes, offset, format)
                    || !readU32(bytes, offset, plugin.slotIndex)
                    || !readU32(bytes, offset, bypassed)
                    || bypassed > 1U
                    || !readString(bytes, offset, plugin.identifier)
                    || !readString(bytes, offset, plugin.name)
                    || !readBlob(
                        bytes,
                        offset,
                        plugin.state,
                        kMaximumTotalPluginStateBytes
                            - totalStateBytes
                    )) {
                    result.error =
                        "invalid or truncated session plugin";
                    return result;
                }
                totalStateBytes += plugin.state.size();
                plugin.format =
                    static_cast<SessionPluginFormat>(format);
                plugin.bypassed = bypassed != 0U;
                result.document.plugins.push_back(std::move(plugin));
            }
        } else {
            // Schema v3 and older encode clip sources as bare IDs. Name
            // each one as an unresolved placeholder so the migrated
            // document keeps total referential integrity and a relink
            // pass has somewhere to write the located path.
            std::unordered_set<std::uint64_t> declared;
            declared.reserve(result.document.clips.size());
            for (const auto& clip : result.document.clips) {
                if (declared.insert(clip.sourceId).second) {
                    SessionMediaSource placeholder;
                    placeholder.stableId = clip.sourceId;
                    result.document.mediaSources.push_back(
                        placeholder
                    );
                }
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
