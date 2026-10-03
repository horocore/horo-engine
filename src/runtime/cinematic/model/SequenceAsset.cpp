#include "Horo/Cinematic/SequenceAsset.h"

#include "Horo/Cinematic/CinematicErrors.h"
#include "JsonUtils.h"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace Horo::Cinematic {
    namespace {
        using Json = nlohmann::json;
        using Horo::Foundation::HasAllowedFields;
        using Horo::Foundation::JsonParseGuard;
        using namespace std::string_view_literals;

        [[nodiscard]] Error Failure(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            return MakeError(descriptor, std::move(message));
        }

        template <typename T> [[nodiscard]] Result<T> Failed(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            return Result<T>::Failure(Failure(descriptor, std::move(message)));
        }

        [[nodiscard]] Result<void> ValidateLimits(const SequenceSchemaLimits &limits) {
            if (const std::array admitted{limits.maximumSourceBytes > 0 &&
                                              limits.maximumSourceBytes <= SequenceSchemaHardLimits::SourceBytes,
                                          limits.maximumJsonDepth > 0 && limits.maximumJsonDepth <= SequenceSchemaHardLimits::JsonDepth,
                                          limits.maximumNameBytes > 0 && limits.maximumNameBytes <= SequenceSchemaHardLimits::NameBytes,
                                          limits.maximumTracks > 0 && limits.maximumTracks <= SequenceSchemaHardLimits::Tracks,
                                          limits.maximumReferences > 0 && limits.maximumReferences <= SequenceSchemaHardLimits::References,
                                          limits.maximumKeysPerTrack > 0 &&
                                              limits.maximumKeysPerTrack <= SequenceSchemaHardLimits::KeysPerTrack};
                !std::ranges::all_of(admitted, [](const bool value) {
                return value;
            }))
                return Result<void>::Failure(
                    Failure(CinematicErrors::SequenceSchemaLimitExceeded, "Invalid active sequence schema limits."));
            return Result<void>::Success();
        }

        template <typename Integer> [[nodiscard]] Result<Integer> ReadUnsigned(const Json &value, const std::string_view field) {
            if (!value.is_number_unsigned())
                return Failed<Integer>(CinematicErrors::SequenceSchemaMalformed, std::format("{} must be an unsigned integer.", field));
            const auto decoded = value.get<std::uint64_t>();
            if (decoded > std::numeric_limits<Integer>::max())
                return Failed<Integer>(CinematicErrors::SequenceSchemaMalformed, std::format("{} is outside its encoded range.", field));
            return Result<Integer>::Success(static_cast<Integer>(decoded));
        }

        [[nodiscard]] Result<SequenceSchemaVersion> DecodeVersion(const Json &value) {
            if (!HasAllowedFields(value, {"major", "minor"}))
                return Failed<SequenceSchemaVersion>(CinematicErrors::SequenceSchemaMalformed, "schemaVersion has an invalid shape.");
            auto major = ReadUnsigned<std::uint16_t>(value.at("major"), "schemaVersion.major");
            auto minor = ReadUnsigned<std::uint16_t>(value.at("minor"), "schemaVersion.minor");
            if (major.HasError())
                return Failed<SequenceSchemaVersion>(CinematicErrors::SequenceSchemaMalformed, major.ErrorValue().message);
            if (minor.HasError())
                return Failed<SequenceSchemaVersion>(CinematicErrors::SequenceSchemaMalformed, minor.ErrorValue().message);
            return Result<SequenceSchemaVersion>::Success({major.Value(), minor.Value()});
        }

        template <typename Identity> [[nodiscard]] Result<Identity> DecodeIdentity(const Json &value, const std::string_view field) {
            if (!HasAllowedFields(value, {"stableValue", "generation"}))
                return Failed<Identity>(CinematicErrors::SequenceSchemaMalformed, std::format("{} has an invalid shape.", field));
            auto stable = ReadUnsigned<std::uint64_t>(value.at("stableValue"), std::format("{}.stableValue", field));
            auto generation = ReadUnsigned<std::uint32_t>(value.at("generation"), std::format("{}.generation", field));
            if (stable.HasError() || generation.HasError())
                return Failed<Identity>(CinematicErrors::SequenceSchemaMalformed, std::format("{} has an invalid value.", field));
            auto identity = MakeCinematicIdentity<typename Identity::IdentityTag>(stable.Value(), generation.Value());
            if (identity.HasError())
                return Failed<Identity>(CinematicErrors::SequenceSchemaMalformed, std::format("{} uses a reserved identity.", field));
            return identity;
        }

        template <typename Enum, std::size_t Extent>
        [[nodiscard]] Result<Enum> DecodeEnum(const Json &value, const std::string_view field,
                                              const std::span<const std::pair<std::string_view, Enum>, Extent> values) {
            if (!value.is_string())
                return Failed<Enum>(CinematicErrors::SequenceSchemaMalformed, std::format("{} must be a string.", field));
            const std::string &text = value.get_ref<const std::string &>();
            const auto found = std::ranges::find(values, std::string_view{text}, &std::pair<std::string_view, Enum>::first);
            if (found == values.end())
                return Failed<Enum>(CinematicErrors::SequenceSchemaMalformed, std::format("{} has an unknown value.", field));
            return Result<Enum>::Success(found->second);
        }

        [[nodiscard]] Result<Assets::AssetId> DecodeAssetId(const Json &value, const std::string_view field) {
            if (!value.is_string())
                return Failed<Assets::AssetId>(CinematicErrors::SequenceSchemaMalformed, std::format("{} must be a UUID string.", field));
            auto asset = Assets::AssetId::Parse(value.get_ref<const std::string &>());
            if (asset.HasError())
                return Failed<Assets::AssetId>(CinematicErrors::SequenceSchemaMalformed, std::format("{} is not a valid AssetId.", field));
            return asset;
        }

        [[nodiscard]] Result<SequencePlaybackSettings> DecodePlayback(const Json &value) {
            using enum SequenceClockSource;
            if (!HasAllowedFields(value, {"loopMode", "clockSource", "pausePolicy", "dilationPolicy"}, {"pauseGameplay", "hideHUD"}))
                return Failed<SequencePlaybackSettings>(CinematicErrors::SequenceSchemaMalformed, "playback has an invalid shape.");
            constexpr std::array loopModes{std::pair{"once"sv, SequenceLoopMode::Once}, std::pair{"loop"sv, SequenceLoopMode::Loop},
                                           std::pair{"pingPong"sv, SequenceLoopMode::PingPong}};
            constexpr std::array clocks{std::pair{"committedSimulation"sv, CommittedSimulation},
                                        std::pair{"unscaledFixedControl"sv, UnscaledFixedControl},
                                        std::pair{"monotonicWall"sv, MonotonicWall}, std::pair{"external"sv, External}};
            constexpr std::array pauses{std::pair{"followGameplay"sv, SequencePausePolicy::FollowGameplay},
                                        std::pair{"playerOnly"sv, SequencePausePolicy::PlayerOnly}};
            constexpr std::array dilations{std::pair{"sourceNative"sv, SequenceDilationPolicy::SourceNative},
                                           std::pair{"applyGameplayScale"sv, SequenceDilationPolicy::ApplyGameplayScale}};
            auto loop = DecodeEnum(value.at("loopMode"), "playback.loopMode", std::span{loopModes});
            auto clock = DecodeEnum(value.at("clockSource"), "playback.clockSource", std::span{clocks});
            auto pause = DecodeEnum(value.at("pausePolicy"), "playback.pausePolicy", std::span{pauses});
            auto dilation = DecodeEnum(value.at("dilationPolicy"), "playback.dilationPolicy", std::span{dilations});
            if (loop.HasError() || clock.HasError() || pause.HasError() || dilation.HasError())
                return Failed<SequencePlaybackSettings>(CinematicErrors::SequenceSchemaMalformed, "playback contains an unknown policy.");
            if ((value.contains("pauseGameplay") && !value.at("pauseGameplay").is_boolean()) ||
                (value.contains("hideHUD") && !value.at("hideHUD").is_boolean()))
                return Failed<SequencePlaybackSettings>(CinematicErrors::SequenceSchemaMalformed,
                                                        "playback coordination flags must be boolean.");
            return Result<SequencePlaybackSettings>::Success({loop.Value(), clock.Value(), pause.Value(), dilation.Value(),
                                                              value.value("pauseGameplay", false), value.value("hideHUD", false)});
        }

        [[nodiscard]] Result<SequenceAssetReference> DecodeReference(const Json &value) {
            using enum SequenceReferenceKind;
            if (!HasAllowedFields(value, {"assetId", "kind"}))
                return Failed<SequenceAssetReference>(CinematicErrors::SequenceSchemaMalformed, "track reference has an invalid shape.");
            constexpr std::array kinds{std::pair{"audioClip"sv, AudioClip}, std::pair{"subSequence"sv, SubSequence},
                                       std::pair{"bindingDescriptor"sv, BindingDescriptor}};
            auto asset = DecodeAssetId(value.at("assetId"), "tracks[].references[].assetId");
            auto kind = DecodeEnum(value.at("kind"), "tracks[].references[].kind", std::span{kinds});
            if (asset.HasError() || kind.HasError())
                return Failed<SequenceAssetReference>(CinematicErrors::SequenceSchemaMalformed, "track reference is invalid.");
            return Result<SequenceAssetReference>::Success({std::move(asset).Value(), kind.Value()});
        }

        [[nodiscard]] Result<SequenceTrackSchema> DecodeTrack(const Json &value, const SequenceSchemaLimits &limits,
                                                              std::size_t &referenceCount) {
            using enum SequenceTrackType;
            if (!HasAllowedFields(value, {"id", "type", "keyframeCount", "references"}) || !value.at("references").is_array())
                return Failed<SequenceTrackSchema>(CinematicErrors::SequenceSchemaMalformed, "track has an invalid shape.");
            constexpr std::array types{std::pair{"transform"sv, Transform}, std::pair{"property"sv, Property},
                                       std::pair{"cameraCut"sv, CameraCut}, std::pair{"event"sv, Event},
                                       std::pair{"audio"sv, Audio},         std::pair{"subSequence"sv, SubSequence}};
            auto identity = DecodeIdentity<TrackId>(value.at("id"), "tracks[].id");
            auto type = DecodeEnum(value.at("type"), "tracks[].type", std::span{types});
            auto keys = ReadUnsigned<std::uint32_t>(value.at("keyframeCount"), "tracks[].keyframeCount");
            if (identity.HasError() || type.HasError() || keys.HasError())
                return Failed<SequenceTrackSchema>(CinematicErrors::SequenceSchemaMalformed, "track contains an invalid field.");
            const Json &encodedReferences = value.at("references");
            if (encodedReferences.size() > limits.maximumReferences - std::min(referenceCount, limits.maximumReferences))
                return Failed<SequenceTrackSchema>(CinematicErrors::SequenceSchemaLimitExceeded,
                                                   "Sequence dependency count exceeds the parser limit.");

            SequenceTrackSchema track{std::move(identity).Value(), type.Value(), keys.Value(), {}};
            track.references.reserve(encodedReferences.size());
            for (const Json &encoded : encodedReferences) {
                auto reference = DecodeReference(encoded);
                if (reference.HasError())
                    return Failed<SequenceTrackSchema>(CinematicErrors::SequenceSchemaMalformed, reference.ErrorValue().message);
                track.references.push_back(std::move(reference).Value());
            }
            referenceCount += encodedReferences.size();
            return Result<SequenceTrackSchema>::Success(std::move(track));
        }

        [[nodiscard]] bool HasValidRootShape(const Json &root) {
            return HasAllowedFields(root, {"schemaVersion", "assetId", "sequenceId", "name", "durationFrames", "frameRate", "playback",
                                           "tracks"}) &&
                   root.at("name").is_string() && root.at("tracks").is_array() &&
                   HasAllowedFields(root.at("frameRate"), {"numerator", "denominator"});
        }

        [[nodiscard]] Result<SequenceSchemaVersion> DecodeCurrentVersion(const Json &value) {
            auto version = DecodeVersion(value);
            if (version.HasError())
                return version;
            if (ClassifySequenceSchemaCompatibility(version.Value()) != SequenceSchemaCompatibility::Exact)
                return Failed<SequenceSchemaVersion>(CinematicErrors::SequenceSchemaVersionUnsupported,
                                                     std::format("Sequence schema {}.{} requires migration or a newer reader.",
                                                                 version.Value().major, version.Value().minor));
            return version;
        }

        [[nodiscard]] Result<SequenceAssetData> DecodeAssetHeader(const Json &root) {
            auto version = DecodeCurrentVersion(root.at("schemaVersion"));
            if (version.HasError())
                return Result<SequenceAssetData>::Failure(version.ErrorValue());
            auto asset = DecodeAssetId(root.at("assetId"), "assetId");
            auto sequence = DecodeIdentity<SequenceId>(root.at("sequenceId"), "sequenceId");
            auto duration = ReadUnsigned<std::uint64_t>(root.at("durationFrames"), "durationFrames");
            auto numerator = ReadUnsigned<std::uint32_t>(root.at("frameRate").at("numerator"), "frameRate.numerator");
            auto denominator = ReadUnsigned<std::uint32_t>(root.at("frameRate").at("denominator"), "frameRate.denominator");
            auto playback = DecodePlayback(root.at("playback"));
            if (const std::array decoded{!asset.HasError(), !sequence.HasError(), !duration.HasError(), !numerator.HasError(),
                                         !denominator.HasError(), !playback.HasError()};
                !std::ranges::all_of(decoded, [](const bool value) {
                return value;
            }))
                return Failed<SequenceAssetData>(CinematicErrors::SequenceSchemaMalformed, "Sequence root contains an invalid field.");
            return Result<SequenceAssetData>::Success({version.Value(),
                                                       std::move(asset).Value(),
                                                       sequence.Value(),
                                                       root.at("name").get<std::string>(),
                                                       duration.Value(),
                                                       {numerator.Value(), denominator.Value()},
                                                       playback.Value(),
                                                       {}});
        }

        [[nodiscard]] Result<void> DecodeTracks(const Json &encodedTracks, const SequenceSchemaLimits &limits, SequenceAssetData &data) {
            data.tracks.reserve(encodedTracks.size());
            std::size_t referenceCount{};
            for (const Json &encoded : encodedTracks) {
                auto track = DecodeTrack(encoded, limits, referenceCount);
                if (track.HasError())
                    return Result<void>::Failure(track.ErrorValue());
                data.tracks.push_back(std::move(track).Value());
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<SequenceAssetData> DecodeAsset(const Json &root, const SequenceSchemaLimits &limits) {
            if (!HasValidRootShape(root))
                return Failed<SequenceAssetData>(CinematicErrors::SequenceSchemaMalformed, "Sequence root has an invalid shape.");
            if (root.at("tracks").size() > limits.maximumTracks)
                return Failed<SequenceAssetData>(CinematicErrors::SequenceSchemaLimitExceeded,
                                                 "Sequence track count exceeds the parser limit.");
            auto header = DecodeAssetHeader(root);
            if (header.HasError())
                return header;
            SequenceAssetData data = std::move(header).Value();
            if (auto tracks = DecodeTracks(root.at("tracks"), limits, data); tracks.HasError())
                return Result<SequenceAssetData>::Failure(tracks.ErrorValue());
            return Result<SequenceAssetData>::Success(std::move(data));
        }

        [[nodiscard]] bool IsKnown(const SequenceTrackType type) noexcept {
            return type < SequenceTrackType::Count;
        }

        [[nodiscard]] bool IsKnown(const SequenceReferenceKind kind) noexcept {
            return kind < SequenceReferenceKind::Count;
        }

        [[nodiscard]] bool IsReferenceAllowed(const SequenceTrackType track, const SequenceReferenceKind reference) noexcept {
            return (track == SequenceTrackType::Audio && reference == SequenceReferenceKind::AudioClip) ||
                   (track == SequenceTrackType::SubSequence && reference == SequenceReferenceKind::SubSequence) ||
                   (track == SequenceTrackType::Property && reference == SequenceReferenceKind::BindingDescriptor);
        }

        [[nodiscard]] bool HasValidPlayback(const SequencePlaybackSettings &settings) noexcept {
            if (settings.loopMode >= SequenceLoopMode::Count || settings.clockSource >= SequenceClockSource::Count ||
                settings.pausePolicy >= SequencePausePolicy::Count || settings.dilationPolicy >= SequenceDilationPolicy::Count)
                return false;
            if (settings.clockSource == SequenceClockSource::CommittedSimulation)
                return settings.pausePolicy == SequencePausePolicy::FollowGameplay &&
                       settings.dilationPolicy == SequenceDilationPolicy::SourceNative && !settings.pauseGameplay;
            if (settings.clockSource == SequenceClockSource::External)
                return settings.dilationPolicy == SequenceDilationPolicy::SourceNative &&
                       (!settings.pauseGameplay || settings.pausePolicy == SequencePausePolicy::PlayerOnly);
            return !settings.pauseGameplay || settings.pausePolicy == SequencePausePolicy::PlayerOnly;
        }

        [[nodiscard]] Result<void> ValidateAssetHeader(const SequenceAssetData &data, const SequenceSchemaLimits &limits) {
            if (ClassifySequenceSchemaCompatibility(data.version) != SequenceSchemaCompatibility::Exact)
                return Result<void>::Failure(Failure(CinematicErrors::SequenceSchemaVersionUnsupported));
            if (data.name.size() > limits.maximumNameBytes)
                return Result<void>::Failure(Failure(CinematicErrors::SequenceSchemaLimitExceeded));
            if (const std::array valid{data.asset.IsValid(), data.sequence.IsValid(), !data.name.empty(),
                                       data.name.find('\0') == std::string::npos, data.durationFrames != 0, data.frameRate.numerator != 0,
                                       data.frameRate.denominator != 0, HasValidPlayback(data.playback)};
                !std::ranges::all_of(valid, [](const bool value) {
                return value;
            }))
                return Result<void>::Failure(Failure(CinematicErrors::SequenceSchemaMalformed));
            return Result<void>::Success();
        }

        [[nodiscard]] bool HasDuplicateTrack(const std::span<const SequenceTrackSchema> tracks, const std::size_t index) {
            return std::ranges::any_of(tracks.first(index), [&tracks, index](const SequenceTrackSchema &candidate) {
                return candidate.id.stableValue == tracks[index].id.stableValue;
            });
        }

        [[nodiscard]] bool HasDuplicateReference(const std::span<const SequenceAssetReference> references, const std::size_t index) {
            return std::ranges::any_of(references.first(index), [&references, index](const SequenceAssetReference &candidate) {
                return candidate.asset == references[index].asset && candidate.kind == references[index].kind;
            });
        }

        [[nodiscard]] Result<void> ValidateReferences(const SequenceTrackSchema &track) {
            const std::span references{track.references};
            for (std::size_t index = 0; index < references.size(); ++index) {
                const SequenceAssetReference &reference = references[index];
                if (!reference.asset.IsValid() || !IsKnown(reference.kind) || !IsReferenceAllowed(track.type, reference.kind))
                    return Result<void>::Failure(
                        Failure(CinematicErrors::SequenceSchemaMalformed,
                                std::format("Track {} contains an invalid dependency kind.", track.id.stableValue)));
                if (HasDuplicateReference(references, index))
                    return Result<void>::Failure(
                        Failure(CinematicErrors::SequenceSchemaDuplicate, std::format("Track {} contains a duplicate dependency {}.",
                                                                                      track.id.stableValue, reference.asset.ToString())));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateTrack(const std::span<const SequenceTrackSchema> tracks, const std::size_t index,
                                                 const SequenceSchemaLimits &limits, const std::size_t referenceCount) {
            const SequenceTrackSchema &track = tracks[index];
            if (track.keyframeCount > limits.maximumKeysPerTrack)
                return Result<void>::Failure(
                    Failure(CinematicErrors::SequenceSchemaLimitExceeded, std::format("Invalid sequence track at index {}.", index)));
            if (!track.id.IsValid() || !IsKnown(track.type))
                return Result<void>::Failure(
                    Failure(CinematicErrors::SequenceSchemaMalformed, std::format("Invalid sequence track at index {}.", index)));
            if (HasDuplicateTrack(tracks, index))
                return Result<void>::Failure(Failure(CinematicErrors::SequenceSchemaDuplicate,
                                                     std::format("Duplicate track stable identity {}.", track.id.stableValue)));
            if (track.references.size() > limits.maximumReferences - std::min(referenceCount, limits.maximumReferences))
                return Result<void>::Failure(
                    Failure(CinematicErrors::SequenceSchemaLimitExceeded, "Sequence dependency count exceeds the schema limit."));
            return ValidateReferences(track);
        }

        [[nodiscard]] Result<void> ValidateTracks(const SequenceAssetData &data, const SequenceSchemaLimits &limits) {
            std::size_t referenceCount{};
            const std::span tracks{data.tracks};
            for (std::size_t index = 0; index < tracks.size(); ++index) {
                if (auto validated = ValidateTrack(tracks, index, limits, referenceCount); validated.HasError())
                    return validated;
                referenceCount += tracks[index].references.size();
            }
            return Result<void>::Success();
        }

    }  // namespace

    /** @copydoc SequenceAsset::Create */
    Result<SequenceAsset> SequenceAsset::Create(SequenceAssetData data, const SequenceSchemaLimits &limits) {
        if (auto validLimits = ValidateLimits(limits); validLimits.HasError())
            return Result<SequenceAsset>::Failure(validLimits.ErrorValue());
        if (auto header = ValidateAssetHeader(data, limits); header.HasError())
            return Result<SequenceAsset>::Failure(header.ErrorValue());
        if (data.tracks.size() > limits.maximumTracks)
            return Failed<SequenceAsset>(CinematicErrors::SequenceSchemaLimitExceeded, "Sequence track count exceeds the schema limit.");
        if (auto tracks = ValidateTracks(data, limits); tracks.HasError())
            return Result<SequenceAsset>::Failure(tracks.ErrorValue());
        return Result<SequenceAsset>::Success(SequenceAsset{std::move(data)});
    }

    /** @copydoc SequenceAsset::Data */
    const SequenceAssetData &SequenceAsset::Data() const noexcept {
        return data_;
    }

    SequenceAsset::SequenceAsset(SequenceAssetData data) noexcept : data_(std::move(data)) {}

    /** @copydoc ParseSequenceAsset */
    Result<SequenceAsset> ParseSequenceAsset(const std::string_view source, const SequenceSchemaLimits &limits) {
        if (auto validLimits = ValidateLimits(limits); validLimits.HasError())
            return Result<SequenceAsset>::Failure(validLimits.ErrorValue());
        if (source.empty())
            return Failed<SequenceAsset>(CinematicErrors::SequenceSchemaMalformed, "Sequence source is empty.");
        if (source.size() > limits.maximumSourceBytes)
            return Failed<SequenceAsset>(CinematicErrors::SequenceSchemaLimitExceeded,
                                         "Sequence source byte count exceeds the parser limit.");

        try {
            JsonParseGuard guard{limits.maximumJsonDepth};
            Json root = Json::parse(source, std::ref(guard), true, false);
            if (guard.HasDuplicate())
                return Failed<SequenceAsset>(CinematicErrors::SequenceSchemaDuplicate, "Sequence JSON contains a duplicate object field.");
            if (guard.IsTooDeep())
                return Failed<SequenceAsset>(CinematicErrors::SequenceSchemaLimitExceeded,
                                             "Sequence JSON nesting exceeds the parser limit.");
            auto decoded = DecodeAsset(root, limits);
            if (decoded.HasError())
                return Result<SequenceAsset>::Failure(decoded.ErrorValue());
            return SequenceAsset::Create(std::move(decoded).Value(), limits);
        } catch (const Json::exception &) {
            return Failed<SequenceAsset>(CinematicErrors::SequenceSchemaMalformed, "Sequence source is not valid JSON.");
        }
    }

}  // namespace Horo::Cinematic
