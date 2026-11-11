#include "Horo/Audio/AudioAssetSchema.h"
#include "Horo/Audio/AudioErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <type_traits>
#include <utility>

namespace Horo::Audio {
    namespace {
        void RequireError(const Result<void> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == descriptor.domain.Value());
            CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
        }

        AudioClipId Clip(const std::uint8_t marker) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = marker;
            auto clip = AudioClipId::Create(Assets::AssetId::FromBytes(bytes));
            REQUIRE(clip.HasValue());
            return std::move(clip).Value();
        }

        AudioMediaDescriptor Media(const std::uint64_t frames = 48'000) {
            return {{48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)}, frames};
        }

        AudioLoudnessMetadata Loudness() {
            return {.integratedLufs = -16.0F,
                    .shortTermLufs = -14.0F,
                    .truePeakDbtp = -1.0F,
                    .rmsDbfs = -18.0F,
                    .normalizationGainDb = 2.0F};
        }

        AudioMediaAnnotations Annotations() {
            return {{{{1}, 12'000, 36'000}}, {{{2}, 47'999, "outro"}}, Loudness()};
        }

        AudioClipAssetSchema ValidClip() {
            return {CurrentAudioAssetSchemaVersion, {Media()}, Annotations()};
        }

        AudioStreamAssetSchema ValidStream() {
            return {CurrentAudioAssetSchemaVersion,
                    {Media(96'000), 24'000, 1'024, {{0, 0}, {48'000, 12'000}, {95'999, 23'999}}},
                    Annotations()};
        }

        AudioVariationAssetSchema ValidVariation(const AudioVariationSelection selection = AudioVariationSelection::Random) {
            const float weight = selection == AudioVariationSelection::WeightedRandom ? 0.5F : 1.0F;
            return {CurrentAudioAssetSchemaVersion, selection, {{Clip(1), weight}, {Clip(2), weight}}, {-1.0F, 1.0F}, {-3.0F, 0.0F}, 0};
        }

    }  // namespace

    TEST_CASE("Audio clip and stream schemas preserve valid bounded semantic metadata", "[unit][audio][asset-schema]") {
        const AudioClipAssetSchema clip = ValidClip();
        REQUIRE(ValidateAudioClipAssetSchema(clip).HasValue());
        CHECK(clip.payload.media.frameCount == 48'000);
        CHECK(clip.annotations.loops.front().endFrame == 36'000);
        CHECK(clip.annotations.markers.front().frame == clip.payload.media.frameCount - 1);
        CHECK(clip.annotations.loudness->integratedLufs == -16.0F);

        const AudioStreamAssetSchema stream = ValidStream();
        REQUIRE(ValidateAudioStreamAssetSchema(stream).HasValue());
        CHECK(stream.payload.seekPoints.back() == AudioSeekPoint{95'999, 23'999});
        static_assert(std::is_copy_constructible_v<AudioClipAssetSchema>);
        static_assert(std::is_copy_constructible_v<AudioStreamAssetSchema>);
        static_assert(std::is_same_v<decltype(AudioVariationEntry::clip), AudioClipId>);
    }

    TEST_CASE("Audio asset duration is bounded before runtime arithmetic", "[unit][audio][asset-schema]") {
        auto clip = ValidClip();
        clip.payload.media.frameCount = (1ULL << 40U) + 1;
        RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaInvalid);
        auto stream = ValidStream();
        stream.payload.media.frameCount = std::numeric_limits<std::uint64_t>::max();
        RequireError(ValidateAudioStreamAssetSchema(stream), AudioErrors::AssetSchemaInvalid);
    }

    TEST_CASE("Audio asset schemas reject unsupported versions and invalid semantic media", "[unit][audio][asset-schema]") {
        AudioClipAssetSchema clip = ValidClip();
        const AudioClipAssetSchema original = clip;
        clip.version = {0, 0};
        RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaVersionUnsupported);
        clip.version = {2, 0};
        RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaVersionUnsupported);
        clip.version = {1, 1};
        RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaVersionUnsupported);
        CHECK(original == ValidClip());

        clip = ValidClip();
        clip.payload.media.frameCount = 0;
        RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaInvalid);
        clip = ValidClip();
        clip.payload.media.format.sampleRate = MinimumAudioSampleRate - 1;
        RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaInvalid);
        clip = ValidClip();
        clip.payload.media.format.layout.orderedChannels.clear();
        RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaInvalid);
    }

    TEST_CASE("Audio annotation validation enforces half-open frames, stable identities and limits", "[unit][audio][asset-schema]") {
        AudioClipAssetSchema clip = ValidClip();
        clip.annotations.loops.front().id = {};
        RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaInvalid);
        clip = ValidClip();
        clip.annotations.loops.front().endFrame = clip.annotations.loops.front().startFrame;
        RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaInvalid);
        clip = ValidClip();
        clip.annotations.loops.front().endFrame = clip.payload.media.frameCount + 1;
        RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaInvalid);
        clip = ValidClip();
        clip.annotations.loops.push_back(clip.annotations.loops.front());
        RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaInvalid);

        clip = ValidClip();
        clip.annotations.markers.front().id = {};
        RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaInvalid);
        clip = ValidClip();
        clip.annotations.markers.front().frame = clip.payload.media.frameCount;
        RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaInvalid);
        clip = ValidClip();
        clip.annotations.markers.push_back(clip.annotations.markers.front());
        RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaInvalid);

        AudioAssetSchemaLimits limits;
        limits.maximumLoopRegions = 1;
        limits.maximumMarkers = 1;
        limits.maximumMarkerLabelBytes = 5;
        REQUIRE(ValidateAudioClipAssetSchema(ValidClip(), limits).HasValue());
        clip = ValidClip();
        clip.annotations.loops.push_back({{3}, 1, 2});
        RequireError(ValidateAudioClipAssetSchema(clip, limits), AudioErrors::AssetSchemaLimitExceeded);
        clip = ValidClip();
        clip.annotations.markers.front().label.push_back('!');
        RequireError(ValidateAudioClipAssetSchema(clip, limits), AudioErrors::AssetSchemaLimitExceeded);
    }

    TEST_CASE("Audio loudness metadata accepts optional finite values and rejects invalid numbers", "[unit][audio][asset-schema]") {
        AudioClipAssetSchema clip = ValidClip();
        clip.annotations.loudness.reset();
        REQUIRE(ValidateAudioClipAssetSchema(clip).HasValue());
        clip.annotations.loudness = AudioLoudnessMetadata{.integratedLufs = 0.0F};
        REQUIRE(ValidateAudioClipAssetSchema(clip).HasValue());
        clip.annotations.loudness = AudioLoudnessMetadata{};
        RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaInvalid);

        const std::array invalidValues{std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                       -std::numeric_limits<float>::infinity()};
        for (const float value : invalidValues) {
            clip.annotations.loudness = Loudness();
            clip.annotations.loudness->integratedLufs = value;
            RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaInvalid);
            clip.annotations.loudness = Loudness();
            clip.annotations.loudness->shortTermLufs = value;
            RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaInvalid);
            clip.annotations.loudness = Loudness();
            clip.annotations.loudness->truePeakDbtp = value;
            RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaInvalid);
            clip.annotations.loudness = Loudness();
            clip.annotations.loudness->rmsDbfs = value;
            RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaInvalid);
            clip.annotations.loudness = Loudness();
            clip.annotations.loudness->normalizationGainDb = value;
            RequireError(ValidateAudioClipAssetSchema(clip), AudioErrors::AssetSchemaInvalid);
        }
    }

    TEST_CASE("Audio stream seek metadata is bounded and strictly monotonic", "[unit][audio][asset-schema]") {
        AudioStreamAssetSchema stream = ValidStream();
        stream.payload.seekPoints.clear();
        REQUIRE(ValidateAudioStreamAssetSchema(stream).HasValue());
        stream = ValidStream();
        stream.payload.payloadByteCount = 0;
        RequireError(ValidateAudioStreamAssetSchema(stream), AudioErrors::AssetSchemaInvalid);
        stream = ValidStream();
        stream.payload.decodeBlockFrameCount = 0;
        RequireError(ValidateAudioStreamAssetSchema(stream), AudioErrors::AssetSchemaInvalid);
        stream = ValidStream();
        stream.payload.decodeBlockFrameCount = static_cast<std::uint32_t>(stream.payload.media.frameCount + 1);
        RequireError(ValidateAudioStreamAssetSchema(stream), AudioErrors::AssetSchemaInvalid);

        stream = ValidStream();
        stream.payload.seekPoints[1].frame = stream.payload.seekPoints[0].frame;
        RequireError(ValidateAudioStreamAssetSchema(stream), AudioErrors::AssetSchemaInvalid);
        stream = ValidStream();
        stream.payload.seekPoints[1].payloadByteOffset = stream.payload.seekPoints[0].payloadByteOffset;
        RequireError(ValidateAudioStreamAssetSchema(stream), AudioErrors::AssetSchemaInvalid);
        stream = ValidStream();
        stream.payload.seekPoints.back().frame = stream.payload.media.frameCount;
        RequireError(ValidateAudioStreamAssetSchema(stream), AudioErrors::AssetSchemaInvalid);
        stream = ValidStream();
        stream.payload.seekPoints.back().payloadByteOffset = stream.payload.payloadByteCount;
        RequireError(ValidateAudioStreamAssetSchema(stream), AudioErrors::AssetSchemaInvalid);

        AudioAssetSchemaLimits limits;
        limits.maximumSeekPoints = 3;
        REQUIRE(ValidateAudioStreamAssetSchema(ValidStream(), limits).HasValue());
        stream = ValidStream();
        stream.payload.seekPoints.push_back({95'999, 23'999});
        RequireError(ValidateAudioStreamAssetSchema(stream, limits), AudioErrors::AssetSchemaLimitExceeded);
    }

    TEST_CASE("Audio variation schemas preserve selection order and deterministic zero seed", "[unit][audio][asset-schema]") {
        const std::array selections{AudioVariationSelection::Random, AudioVariationSelection::RoundRobin, AudioVariationSelection::Shuffle,
                                    AudioVariationSelection::WeightedRandom};
        for (const AudioVariationSelection selection : selections) {
            const AudioVariationAssetSchema variation = ValidVariation(selection);
            REQUIRE(ValidateAudioVariationAssetSchema(variation).HasValue());
            CHECK(variation.deterministicSeed == 0);
            CHECK(variation.entries[0].clip == Clip(1));
            CHECK(variation.entries[1].clip == Clip(2));
        }
    }

    TEST_CASE("Audio variation validation rejects ambiguous identities, policies and weights", "[unit][audio][asset-schema]") {
        AudioVariationAssetSchema variation = ValidVariation();
        variation.entries.clear();
        RequireError(ValidateAudioVariationAssetSchema(variation), AudioErrors::AssetSchemaInvalid);
        variation = ValidVariation();
        variation.entries.front().clip = {};
        RequireError(ValidateAudioVariationAssetSchema(variation), AudioErrors::AssetSchemaInvalid);
        variation = ValidVariation();
        variation.entries.back().clip = variation.entries.front().clip;
        RequireError(ValidateAudioVariationAssetSchema(variation), AudioErrors::AssetSchemaInvalid);
        variation = ValidVariation();
        variation.selection = static_cast<AudioVariationSelection>(255);
        RequireError(ValidateAudioVariationAssetSchema(variation), AudioErrors::AssetSchemaInvalid);
        variation = ValidVariation();
        variation.entries.front().weight = 2.0F;
        RequireError(ValidateAudioVariationAssetSchema(variation), AudioErrors::AssetSchemaInvalid);

        variation = ValidVariation(AudioVariationSelection::WeightedRandom);
        variation.entries.front().weight = 0.0F;
        RequireError(ValidateAudioVariationAssetSchema(variation), AudioErrors::AssetSchemaInvalid);
        variation.entries.front().weight = -1.0F;
        RequireError(ValidateAudioVariationAssetSchema(variation), AudioErrors::AssetSchemaInvalid);
        variation.entries.front().weight = std::numeric_limits<float>::quiet_NaN();
        RequireError(ValidateAudioVariationAssetSchema(variation), AudioErrors::AssetSchemaInvalid);
        variation.entries.front().weight = std::numeric_limits<float>::max();
        variation.entries.back().weight = std::numeric_limits<float>::max();
        RequireError(ValidateAudioVariationAssetSchema(variation), AudioErrors::AssetSchemaInvalid);
    }

    TEST_CASE("Audio variation ranges and active caps fail without normalization", "[unit][audio][asset-schema]") {
        AudioVariationAssetSchema variation = ValidVariation();
        variation.pitchDeltaSemitones = {1.0F, 1.0F};
        variation.gainDeltaDb = {0.0F, 0.0F};
        REQUIRE(ValidateAudioVariationAssetSchema(variation).HasValue());
        variation.pitchDeltaSemitones = {2.0F, 1.0F};
        RequireError(ValidateAudioVariationAssetSchema(variation), AudioErrors::AssetSchemaInvalid);
        variation = ValidVariation();
        variation.gainDeltaDb.maximum = std::numeric_limits<float>::infinity();
        RequireError(ValidateAudioVariationAssetSchema(variation), AudioErrors::AssetSchemaInvalid);

        AudioAssetSchemaLimits limits;
        limits.maximumVariationEntries = 2;
        REQUIRE(ValidateAudioVariationAssetSchema(ValidVariation(), limits).HasValue());
        variation = ValidVariation();
        variation.entries.push_back({Clip(3), 1.0F});
        RequireError(ValidateAudioVariationAssetSchema(variation, limits), AudioErrors::AssetSchemaLimitExceeded);
    }

    TEST_CASE("Audio asset schema limits enforce positive compiled ceilings", "[unit][audio][asset-schema]") {
        AudioAssetSchemaLimits limits;
        REQUIRE(ValidateAudioAssetSchemaLimits(limits).HasValue());

        limits.maximumLoopRegions = 0;
        RequireError(ValidateAudioAssetSchemaLimits(limits), AudioErrors::AssetSchemaLimitExceeded);
        limits = {};
        limits.maximumMarkers = MaximumAudioAssetMarkers + 1;
        RequireError(ValidateAudioAssetSchemaLimits(limits), AudioErrors::AssetSchemaLimitExceeded);
        limits = {};
        limits.maximumSeekPoints = MaximumAudioAssetSeekPoints + 1;
        RequireError(ValidateAudioAssetSchemaLimits(limits), AudioErrors::AssetSchemaLimitExceeded);
        limits = {};
        limits.maximumVariationEntries = MaximumAudioVariationEntries + 1;
        RequireError(ValidateAudioAssetSchemaLimits(limits), AudioErrors::AssetSchemaLimitExceeded);
        limits = {};
        limits.maximumMarkerLabelBytes = MaximumAudioMarkerLabelBytes + 1;
        RequireError(ValidateAudioAssetSchemaLimits(limits), AudioErrors::AssetSchemaLimitExceeded);
    }

    TEST_CASE("Audio asset schema errors expose stable actionable descriptors", "[unit][audio][asset-schema]") {
        CHECK(AudioErrors::AssetSchemaInvalid.domain.Value() == "horo.audio");
        CHECK(AudioErrors::AssetSchemaInvalid.code.Value() == "audio.asset_schema.invalid");
        CHECK(AudioErrors::AssetSchemaVersionUnsupported.code.Value() == "audio.asset_schema.version_unsupported");
        CHECK(AudioErrors::AssetSchemaLimitExceeded.code.Value() == "audio.asset_schema.limit_exceeded");
        CHECK_FALSE(AudioErrors::AssetSchemaInvalid.retryable);
        CHECK(AudioErrors::AssetSchemaInvalid.userActionable);
    }
}  // namespace Horo::Audio
