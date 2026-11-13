#include "Horo/Audio/AudioAssetSchema.h"

#include "Horo/Audio/AudioErrors.h"

#include <array>
#include <cmath>
#include <span>

namespace Horo::Audio {
    namespace {
        [[nodiscard]] Result<void> Invalid() {
            return Result<void>::Failure(MakeError(AudioErrors::AssetSchemaInvalid));
        }

        [[nodiscard]] Result<void> LimitExceeded() {
            return Result<void>::Failure(MakeError(AudioErrors::AssetSchemaLimitExceeded));
        }

        [[nodiscard]] Result<void> ValidateVersion(const AudioAssetSchemaVersion version) {
            if (version != CurrentAudioAssetSchemaVersion)
                return Result<void>::Failure(MakeError(AudioErrors::AssetSchemaVersionUnsupported));
            return Result<void>::Success();
        }

        [[nodiscard]] bool IsFinite(const std::optional<float> value) noexcept {
            return !value.has_value() || std::isfinite(*value);
        }

        [[nodiscard]] bool HasLoudnessValue(const AudioLoudnessMetadata &metadata) noexcept {
            return metadata.integratedLufs.has_value() || metadata.shortTermLufs.has_value() || metadata.truePeakDbtp.has_value() ||
                   metadata.rmsDbfs.has_value() || metadata.normalizationGainDb.has_value();
        }

        [[nodiscard]] bool IsValid(const AudioLoudnessMetadata &metadata) noexcept {
            return HasLoudnessValue(metadata) && IsFinite(metadata.integratedLufs) && IsFinite(metadata.shortTermLufs) &&
                   IsFinite(metadata.truePeakDbtp) && IsFinite(metadata.rmsDbfs) && IsFinite(metadata.normalizationGainDb);
        }

        template <typename T, typename Key>
        [[nodiscard]] bool HasDuplicate(const std::span<const T> values, const Key T::*member) noexcept {
            for (std::size_t candidate = 0; candidate < values.size(); ++candidate) {
                for (std::size_t previous = 0; previous < candidate; ++previous) {
                    if (values[candidate].*member == values[previous].*member)
                        return true;
                }
            }
            return false;
        }

        [[nodiscard]] Result<void> ValidateMedia(const AudioMediaDescriptor &media) {
            if (media.frameCount == 0 || media.frameCount > (1ULL << 40U) || !ValidateAudioProcessingFormat(media.format))
                return Invalid();
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateLoops(const std::vector<AudioLoopRegion> &loops, const std::uint64_t frameCount,
                                                 const std::size_t maximumLoops) {
            if (loops.size() > maximumLoops)
                return LimitExceeded();
            for (const AudioLoopRegion &loop : loops) {
                if (!loop.id.IsValid() || loop.startFrame >= loop.endFrame || loop.endFrame > frameCount)
                    return Invalid();
            }
            if (HasDuplicate<AudioLoopRegion>(loops, &AudioLoopRegion::id))
                return Invalid();
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateMarkers(const std::vector<AudioMarker> &markers, const std::uint64_t frameCount,
                                                   const AudioAssetSchemaLimits &limits) {
            if (markers.size() > limits.maximumMarkers)
                return LimitExceeded();
            for (const AudioMarker &marker : markers) {
                if (!marker.id.IsValid() || marker.frame >= frameCount)
                    return Invalid();
                if (marker.label.size() > limits.maximumMarkerLabelBytes)
                    return LimitExceeded();
            }
            if (HasDuplicate<AudioMarker>(markers, &AudioMarker::id))
                return Invalid();
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateAnnotations(const AudioMediaAnnotations &annotations, const std::uint64_t frameCount,
                                                       const AudioAssetSchemaLimits &limits) {
            if (const Result<void> loops = ValidateLoops(annotations.loops, frameCount, limits.maximumLoopRegions); loops.HasError())
                return loops;
            if (const Result<void> markers = ValidateMarkers(annotations.markers, frameCount, limits); markers.HasError())
                return markers;
            if (annotations.loudness.has_value() && !IsValid(*annotations.loudness))
                return Invalid();
            return Result<void>::Success();
        }

        [[nodiscard]] bool IsKnownSelection(const AudioVariationSelection selection) noexcept {
            using enum AudioVariationSelection;
            switch (selection) {
                case Random:
                case RoundRobin:
                case Shuffle:
                case WeightedRandom:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsValid(const AudioVariationRange range) noexcept {
            return std::isfinite(range.minimum) && std::isfinite(range.maximum) && range.minimum <= range.maximum;
        }

        [[nodiscard]] Result<void> ValidateVariationWeights(const AudioVariationAssetSchema &schema) {
            float totalWeight = 0.0F;
            for (const AudioVariationEntry &entry : schema.entries) {
                if (!std::isfinite(entry.weight))
                    return Invalid();
                if (schema.selection == AudioVariationSelection::WeightedRandom) {
                    if (entry.weight <= 0.0F)
                        return Invalid();
                    totalWeight += entry.weight;
                    if (!std::isfinite(totalWeight))
                        return Invalid();
                } else if (entry.weight != 1.0F) {
                    return Invalid();
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] bool IsAdmittedLimit(const std::size_t value, const std::size_t ceiling) noexcept {
            return value > 0 && value <= ceiling;
        }

        [[nodiscard]] Result<void> ValidateSchemaPreamble(const AudioAssetSchemaVersion version, const AudioAssetSchemaLimits &limits) {
            if (const Result<void> validLimits = ValidateAudioAssetSchemaLimits(limits); validLimits.HasError())
                return validLimits;
            return ValidateVersion(version);
        }

        [[nodiscard]] Result<void> ValidateMediaSchema(const AudioAssetSchemaVersion version, const AudioMediaDescriptor &media,
                                                       const AudioAssetSchemaLimits &limits) {
            if (const Result<void> preamble = ValidateSchemaPreamble(version, limits); preamble.HasError())
                return preamble;
            return ValidateMedia(media);
        }

        [[nodiscard]] Result<void> ValidateSeekPoints(const AudioStreamPayloadDescriptor &payload, const AudioAssetSchemaLimits &limits) {
            if (payload.seekPoints.size() > limits.maximumSeekPoints)
                return LimitExceeded();
            for (std::size_t index = 0; index < payload.seekPoints.size(); ++index) {
                const AudioSeekPoint point = payload.seekPoints[index];
                if (point.frame >= payload.media.frameCount || point.payloadByteOffset >= payload.payloadByteCount)
                    return Invalid();
                if (index > 0) {
                    const AudioSeekPoint previous = payload.seekPoints[index - 1];
                    if (point.frame <= previous.frame || point.payloadByteOffset <= previous.payloadByteOffset)
                        return Invalid();
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateVariationEntries(const AudioVariationAssetSchema &schema, const AudioAssetSchemaLimits &limits) {
            if (schema.entries.empty())
                return Invalid();
            if (schema.entries.size() > limits.maximumVariationEntries)
                return LimitExceeded();
            for (const AudioVariationEntry &entry : schema.entries) {
                if (!entry.clip.IsValid())
                    return Invalid();
            }
            if (HasDuplicate<AudioVariationEntry>(schema.entries, &AudioVariationEntry::clip))
                return Invalid();
            return ValidateVariationWeights(schema);
        }
    }  // namespace

    /** @copydoc ValidateAudioAssetSchemaLimits */
    Result<void> ValidateAudioAssetSchemaLimits(const AudioAssetSchemaLimits &limits) {
        const std::array admitted{IsAdmittedLimit(limits.maximumLoopRegions, MaximumAudioAssetLoopRegions),
                                  IsAdmittedLimit(limits.maximumMarkers, MaximumAudioAssetMarkers),
                                  IsAdmittedLimit(limits.maximumSeekPoints, MaximumAudioAssetSeekPoints),
                                  IsAdmittedLimit(limits.maximumVariationEntries, MaximumAudioVariationEntries),
                                  IsAdmittedLimit(limits.maximumMarkerLabelBytes, MaximumAudioMarkerLabelBytes)};
        for (const bool valid : admitted) {
            if (!valid)
                return LimitExceeded();
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidateAudioClipAssetSchema */
    Result<void> ValidateAudioClipAssetSchema(const AudioClipAssetSchema &schema, const AudioAssetSchemaLimits &limits) {
        if (const Result<void> valid = ValidateMediaSchema(schema.version, schema.payload.media, limits); valid.HasError())
            return valid;
        return ValidateAnnotations(schema.annotations, schema.payload.media.frameCount, limits);
    }

    /** @copydoc ValidateAudioStreamAssetSchema */
    Result<void> ValidateAudioStreamAssetSchema(const AudioStreamAssetSchema &schema, const AudioAssetSchemaLimits &limits) {
        if (const Result<void> valid = ValidateMediaSchema(schema.version, schema.payload.media, limits); valid.HasError())
            return valid;
        if (schema.payload.payloadByteCount == 0 || schema.payload.decodeBlockFrameCount == 0 ||
            schema.payload.decodeBlockFrameCount > schema.payload.media.frameCount)
            return Invalid();
        if (const Result<void> seek = ValidateSeekPoints(schema.payload, limits); seek.HasError())
            return seek;
        return ValidateAnnotations(schema.annotations, schema.payload.media.frameCount, limits);
    }

    /** @copydoc ValidateAudioVariationAssetSchema */
    Result<void> ValidateAudioVariationAssetSchema(const AudioVariationAssetSchema &schema, const AudioAssetSchemaLimits &limits) {
        if (const Result<void> preamble = ValidateSchemaPreamble(schema.version, limits); preamble.HasError())
            return preamble;
        if (!IsKnownSelection(schema.selection) || !IsValid(schema.pitchDeltaSemitones) || !IsValid(schema.gainDeltaDb))
            return Invalid();
        return ValidateVariationEntries(schema, limits);
    }
}  // namespace Horo::Audio
