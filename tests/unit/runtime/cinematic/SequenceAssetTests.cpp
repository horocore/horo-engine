#include "Horo/Cinematic/CinematicErrors.h"
#include "Horo/Cinematic/SequenceAsset.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace Horo::Cinematic {
    namespace {
        Assets::AssetId Asset(const char digit = '1') {
            std::string text = "00000000-0000-0000-0000-000000000001";
            text.back() = digit;
            auto parsed = Assets::AssetId::Parse(text);
            REQUIRE(parsed.HasValue());
            return std::move(parsed).Value();
        }

        SequenceTrackSchema Track(const std::uint64_t id = 10, const SequenceTrackType type = SequenceTrackType::Transform,
                                  const std::uint32_t keys = 1) {
            return {{id, 1}, type, keys, {}};
        }

        SequenceAssetData ValidData() {
            return {CurrentSequenceSchemaVersion, Asset(), {1, 1}, "Intro", 240, {24, 1}, {}, {Track()}};
        }

        SequenceAsset ValidAsset(SequenceAssetData data = ValidData()) {
            auto created = SequenceAsset::Create(std::move(data));
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        std::string ValidJson() {
            return R"({
                "schemaVersion":{"major":1,"minor":0},
                "assetId":"00000000-0000-0000-0000-000000000001",
                "sequenceId":{"stableValue":1,"generation":1},
                "name":"Intro",
                "durationFrames":240,
                "frameRate":{"numerator":24,"denominator":1},
                "playback":{"loopMode":"once","clockSource":"committedSimulation","pausePolicy":"followGameplay","dilationPolicy":"sourceNative"},
                "tracks":[{"id":{"stableValue":10,"generation":1},"type":"audio","keyframeCount":2,
                           "references":[{"assetId":"00000000-0000-0000-0000-000000000002","kind":"audioClip"}]}]
            })";
        }

        void ReplaceFirst(std::string &text, const std::string &from, const std::string &to) {
            const std::size_t position = text.find(from);
            REQUIRE(position != std::string::npos);
            text.replace(position, from.size(), to);
        }

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            const auto actual = std::pair{result.ErrorValue().domain.Value(), result.ErrorValue().code.Value()};
            const auto expected = std::pair{descriptor.domain.Value(), descriptor.code.Value()};
            CHECK(actual == expected);
        }

        SequenceAsset Referencing(const SequenceTrackType trackType, const SequenceReferenceKind kind) {
            auto data = ValidData();
            data.tracks = {Track(10, trackType)};
            data.tracks.front().references = {{Asset('2'), kind}};
            return ValidAsset(std::move(data));
        }

        SequenceResolvedReference Available(const SequenceReferenceKind kind = SequenceReferenceKind::AudioClip,
                                            const std::uint32_t depth = 1) {
            return {Asset('2'), kind, SequenceReferenceAvailability::Available, depth};
        }
    }  // namespace

    TEST_CASE("Sequence schema owns stable typed data after validation", "[unit][cinematic][sequence-schema]") {
        const SequenceAsset asset = ValidAsset();
        CHECK(asset.Data().version == CurrentSequenceSchemaVersion);
        CHECK(asset.Data().asset == Asset());
        CHECK(asset.Data().sequence == SequenceId{1, 1});
        CHECK(asset.Data().durationFrames == 240);
        CHECK(asset.Data().frameRate == SequenceFrameRate{24, 1});
        CHECK(asset.Data().tracks.front().id == TrackId{10, 1});
        static_assert(std::is_copy_constructible_v<SequenceAsset>);
        static_assert(std::is_same_v<decltype(SequenceAssetReference::asset), Assets::AssetId>);
    }

    TEST_CASE("Sequence parser decodes the canonical bounded source schema", "[unit][cinematic][sequence-parser]") {
        auto parsed = ParseSequenceAsset(ValidJson());
        REQUIRE(parsed.HasValue());
        const SequenceAssetData &data = parsed.Value().Data();
        CHECK(data.name == "Intro");
        CHECK(data.playback.clockSource == SequenceClockSource::CommittedSimulation);
        CHECK_FALSE(data.playback.pauseGameplay);
        CHECK_FALSE(data.playback.hideHud);
        REQUIRE(data.tracks.size() == 1);
        CHECK(data.tracks.front().type == SequenceTrackType::Audio);
        CHECK(data.tracks.front().keyframeCount == 2);
        REQUIRE(data.tracks.front().references.size() == 1);
        CHECK(data.tracks.front().references.front() == SequenceAssetReference{Asset('2'), SequenceReferenceKind::AudioClip});
    }

    TEST_CASE("Sequence parser admits optional gameplay pause and HUD settings", "[unit][cinematic][sequence-parser]") {
        std::string source = ValidJson();
        ReplaceFirst(source, R"("clockSource":"committedSimulation","pausePolicy":"followGameplay")",
                     R"("clockSource":"unscaledFixedControl","pausePolicy":"playerOnly")");
        ReplaceFirst(source, R"("dilationPolicy":"sourceNative")",
                     R"("dilationPolicy":"sourceNative","pauseGameplay":true,"hideHUD":true)");
        auto parsed = ParseSequenceAsset(source);
        REQUIRE(parsed.HasValue());
        CHECK(parsed.Value().Data().playback.pauseGameplay);
        CHECK(parsed.Value().Data().playback.hideHud);

        std::string invalid = source;
        ReplaceFirst(invalid, R"("hideHUD":true)", R"("hideHUD":"yes")");
        RequireError(ParseSequenceAsset(invalid), CinematicErrors::SequenceSchemaMalformed);
        invalid = source;
        ReplaceFirst(invalid, R"("pausePolicy":"playerOnly")", R"("pausePolicy":"followGameplay")");
        RequireError(ParseSequenceAsset(invalid), CinematicErrors::SequenceSchemaMalformed);
    }

    TEST_CASE("Sequence parser rejects malformed duplicate and unknown source fields", "[unit][cinematic][sequence-parser]") {
        RequireError(ParseSequenceAsset(""), CinematicErrors::SequenceSchemaMalformed);
        RequireError(ParseSequenceAsset("{"), CinematicErrors::SequenceSchemaMalformed);
        RequireError(ParseSequenceAsset("[]"), CinematicErrors::SequenceSchemaMalformed);

        std::string duplicate = ValidJson();
        ReplaceFirst(duplicate, R"("name":"Intro")", R"("name":"Intro","name":"Duplicate")");
        RequireError(ParseSequenceAsset(duplicate), CinematicErrors::SequenceSchemaDuplicate);

        std::string unknown = ValidJson();
        ReplaceFirst(unknown, R"("durationFrames":240)", R"("durationFrames":240,"legacy":true)");
        RequireError(ParseSequenceAsset(unknown), CinematicErrors::SequenceSchemaMalformed);

        std::string invalidUuid = ValidJson();
        ReplaceFirst(invalidUuid, "00000000-0000-0000-0000-000000000001", "not-an-asset-id");
        RequireError(ParseSequenceAsset(invalidUuid), CinematicErrors::SequenceSchemaMalformed);

        std::string invalidTrack = ValidJson();
        ReplaceFirst(invalidTrack, R"("type":"audio")", R"("type":"native-backend")");
        RequireError(ParseSequenceAsset(invalidTrack), CinematicErrors::SequenceSchemaMalformed);
    }

    TEST_CASE("Sequence source rejects path-shaped dependencies and hostile numeric encodings",
              "[unit][cinematic][sequence-parser][qualification]") {
        const std::array invalidAssets{"../Audio/intro.wav", R"(C:\\Cinematics\\intro.wav)", "/assets/cinematics/intro.wav",
                                       "assets/\xc3\xbc"
                                       "ber/intro.wav",
                                       "00000000-0000-0000-0000-00000000000A"};
        for (const std::string &candidate : invalidAssets) {
            std::string source = ValidJson();
            ReplaceFirst(source, "00000000-0000-0000-0000-000000000002", candidate);
            RequireError(ParseSequenceAsset(source), CinematicErrors::SequenceSchemaMalformed);
        }

        const std::array invalidNumbers{std::pair{R"("durationFrames":240)", R"("durationFrames":-1)"},
                                        std::pair{R"("durationFrames":240)", R"("durationFrames":1.5)"},
                                        std::pair{R"("durationFrames":240)", R"("durationFrames":18446744073709551616)"},
                                        std::pair{R"("numerator":24)", R"("numerator":4294967296)"},
                                        std::pair{R"("keyframeCount":2)", R"("keyframeCount":65537)"}};
        for (const auto &[from, to] : invalidNumbers) {
            std::string source = ValidJson();
            ReplaceFirst(source, from, to);
            const auto parsed = ParseSequenceAsset(source);
            REQUIRE(parsed.HasError());
            CHECK((parsed.ErrorValue().code.Value() == CinematicErrors::SequenceSchemaMalformed.code.Value() ||
                   parsed.ErrorValue().code.Value() == CinematicErrors::SequenceSchemaLimitExceeded.code.Value()));
        }

        std::string embeddedNull = ValidJson();
        ReplaceFirst(embeddedNull, R"("name":"Intro")", R"("name":"Intro\u0000Extra")");
        RequireError(ParseSequenceAsset(embeddedNull), CinematicErrors::SequenceSchemaMalformed);

        std::string invalidUtf8 = ValidJson();
        ReplaceFirst(invalidUtf8, "Intro", std::string{"\xff"});
        RequireError(ParseSequenceAsset(invalidUtf8), CinematicErrors::SequenceSchemaMalformed);
    }

    TEST_CASE("Sequence parser rejects oversized and deeply nested hostile input", "[unit][cinematic][sequence-parser]") {
        SequenceSchemaLimits limits;
        limits.maximumSourceBytes = ValidJson().size();
        REQUIRE(ParseSequenceAsset(ValidJson(), limits).HasValue());
        limits.maximumSourceBytes -= 1;
        RequireError(ParseSequenceAsset(ValidJson(), limits), CinematicErrors::SequenceSchemaLimitExceeded);

        limits = {};
        limits.maximumJsonDepth = 3;
        RequireError(ParseSequenceAsset(ValidJson(), limits), CinematicErrors::SequenceSchemaLimitExceeded);

        limits = {};
        limits.maximumTracks = 1;
        std::string twoTracks = ValidJson();
        const std::string track = R"({"id":{"stableValue":11,"generation":1},"type":"event","keyframeCount":0,"references":[]})";
        ReplaceFirst(twoTracks, "]\n            }", "," + track + "]\n            }");
        RequireError(ParseSequenceAsset(twoTracks, limits), CinematicErrors::SequenceSchemaLimitExceeded);

        limits = {};
        limits.maximumReferences = 1;
        std::string twoReferences = ValidJson();
        const std::string dependency = R"({"assetId":"00000000-0000-0000-0000-000000000003","kind":"audioClip"})";
        ReplaceFirst(twoReferences, "]}]", "," + dependency + "]}]");
        RequireError(ParseSequenceAsset(twoReferences, limits), CinematicErrors::SequenceSchemaLimitExceeded);
    }

    TEST_CASE("Sequence version compatibility is explicit and parser requires exact migration", "[unit][cinematic][sequence-version]") {
        CHECK(ClassifySequenceSchemaCompatibility({1, 0}) == SequenceSchemaCompatibility::Exact);
        CHECK(ClassifySequenceSchemaCompatibility({0, 9}) == SequenceSchemaCompatibility::Unsupported);
        CHECK(ClassifySequenceSchemaCompatibility({1, 1}) == SequenceSchemaCompatibility::Unsupported);
        CHECK(ClassifySequenceSchemaCompatibility({2, 0}) == SequenceSchemaCompatibility::Unsupported);

        std::string future = ValidJson();
        ReplaceFirst(future, R"("major":1,"minor":0)", R"("major":2,"minor":0)");
        RequireError(ParseSequenceAsset(future), CinematicErrors::SequenceSchemaVersionUnsupported);
        std::string futureMinor = ValidJson();
        ReplaceFirst(futureMinor, R"("major":1,"minor":0)", R"("major":1,"minor":1)");
        RequireError(ParseSequenceAsset(futureMinor), CinematicErrors::SequenceSchemaVersionUnsupported);
    }

    TEST_CASE("Sequence validation enforces identity frame and playback invariants", "[unit][cinematic][sequence-schema]") {
        auto data = ValidData();
        data.asset = {};
        RequireError(SequenceAsset::Create(data), CinematicErrors::SequenceSchemaMalformed);
        data = ValidData();
        data.sequence = {};
        RequireError(SequenceAsset::Create(data), CinematicErrors::SequenceSchemaMalformed);
        data = ValidData();
        data.name.clear();
        RequireError(SequenceAsset::Create(data), CinematicErrors::SequenceSchemaMalformed);
        data = ValidData();
        data.durationFrames = 0;
        RequireError(SequenceAsset::Create(data), CinematicErrors::SequenceSchemaMalformed);
        data = ValidData();
        data.frameRate.denominator = 0;
        RequireError(SequenceAsset::Create(data), CinematicErrors::SequenceSchemaMalformed);

        data = ValidData();
        data.playback.pausePolicy = SequencePausePolicy::PlayerOnly;
        RequireError(SequenceAsset::Create(data), CinematicErrors::SequenceSchemaMalformed);
        data = ValidData();
        data.playback.pauseGameplay = true;
        RequireError(SequenceAsset::Create(data), CinematicErrors::SequenceSchemaMalformed);
        data = ValidData();
        data.playback.clockSource = SequenceClockSource::External;
        data.playback.dilationPolicy = SequenceDilationPolicy::ApplyGameplayScale;
        RequireError(SequenceAsset::Create(data), CinematicErrors::SequenceSchemaMalformed);
        data = ValidData();
        data.playback.clockSource = SequenceClockSource::MonotonicWall;
        data.playback.pausePolicy = SequencePausePolicy::PlayerOnly;
        data.playback.dilationPolicy = SequenceDilationPolicy::ApplyGameplayScale;
        REQUIRE(SequenceAsset::Create(data).HasValue());
    }

    TEST_CASE("Sequence validation rejects duplicate tracks dependencies and incompatible kinds", "[unit][cinematic][sequence-schema]") {
        auto data = ValidData();
        data.tracks.push_back(Track());
        RequireError(SequenceAsset::Create(data), CinematicErrors::SequenceSchemaDuplicate);

        data = ValidData();
        data.tracks = {Track(10, SequenceTrackType::Audio)};
        data.tracks.front().references = {{Asset('2'), SequenceReferenceKind::AudioClip}, {Asset('2'), SequenceReferenceKind::AudioClip}};
        RequireError(SequenceAsset::Create(data), CinematicErrors::SequenceSchemaDuplicate);

        data = ValidData();
        data.tracks.front().references = {{Asset('2'), SequenceReferenceKind::AudioClip}};
        RequireError(SequenceAsset::Create(data), CinematicErrors::SequenceSchemaMalformed);

        data = ValidData();
        data.tracks.front().id = {};
        RequireError(SequenceAsset::Create(data), CinematicErrors::SequenceSchemaMalformed);
        data = ValidData();
        data.tracks.front().keyframeCount = SequenceSchemaHardLimits::KeysPerTrack + 1;
        RequireError(SequenceAsset::Create(data), CinematicErrors::SequenceSchemaLimitExceeded);
    }

    TEST_CASE("Sequence schema limits may lower but never raise compiled ceilings", "[unit][cinematic][sequence-schema]") {
        SequenceSchemaLimits limits;
        limits.maximumNameBytes = 5;
        REQUIRE(SequenceAsset::Create(ValidData(), limits).HasValue());
        auto data = ValidData();
        data.name.push_back('!');
        RequireError(SequenceAsset::Create(data, limits), CinematicErrors::SequenceSchemaLimitExceeded);

        limits = {};
        limits.maximumTracks = 0;
        RequireError(SequenceAsset::Create(ValidData(), limits), CinematicErrors::SequenceSchemaLimitExceeded);
        limits = {};
        limits.maximumKeysPerTrack = SequenceSchemaHardLimits::KeysPerTrack + 1;
        RequireError(SequenceAsset::Create(ValidData(), limits), CinematicErrors::SequenceSchemaLimitExceeded);
    }

    TEST_CASE("Sequence cook profiles expose exact architecture tier limits", "[unit][cinematic][sequence-cook]") {
        auto compact = GetSequenceCookProfile(SequenceCookTier::Compact);
        auto standard = GetSequenceCookProfile(SequenceCookTier::Standard);
        auto large = GetSequenceCookProfile(SequenceCookTier::Large);
        REQUIRE(compact.HasValue());
        REQUIRE(standard.HasValue());
        REQUIRE(large.HasValue());
        CHECK(compact.Value().limits == SequenceCookLimits{32, 4'096, 1, 256});
        CHECK(standard.Value().limits == SequenceCookLimits{256, 16'384, 4, 2'048});
        CHECK(large.Value().limits == SequenceCookLimits{1'024, 65'536, 8, 4'096});
        RequireError(GetSequenceCookProfile(SequenceCookTier::Count), CinematicErrors::SequenceSchemaMalformed);
    }

    TEST_CASE("Sequence cook returns a deterministic bounded summary", "[unit][cinematic][sequence-cook]") {
        const SequenceAsset asset = Referencing(SequenceTrackType::Audio, SequenceReferenceKind::AudioClip);
        const auto profile = GetSequenceCookProfile(SequenceCookTier::Standard).Value();
        const std::array references{Available()};
        auto plan = BuildSequenceCookPlan(asset, profile, references);
        REQUIRE(plan.HasValue());
        CHECK(plan.Value().profile == profile);
        CHECK(plan.Value().trackCount == 1);
        CHECK(plan.Value().keyframeCount == 1);
        CHECK(plan.Value().referenceCount == 1);
        CHECK(plan.Value().rootInclusiveNestingDepth == 1);
        auto repeated = BuildSequenceCookPlan(asset, profile, references);
        REQUIRE(repeated.HasValue());
        CHECK(repeated.Value() == plan.Value());
    }

    TEST_CASE("Sequence cook rejects every unavailable external reference state", "[unit][cinematic][sequence-cook]") {
        const SequenceAsset asset = Referencing(SequenceTrackType::Audio, SequenceReferenceKind::AudioClip);
        const auto profile = GetSequenceCookProfile(SequenceCookTier::Standard).Value();
        const std::array cases{std::pair{SequenceReferenceAvailability::Missing, &CinematicErrors::SequenceReferenceMissing},
                               std::pair{SequenceReferenceAvailability::Moved, &CinematicErrors::SequenceReferenceMoved},
                               std::pair{SequenceReferenceAvailability::Unloadable, &CinematicErrors::SequenceReferenceUnloadable},
                               std::pair{SequenceReferenceAvailability::TypeMismatch, &CinematicErrors::SequenceReferenceTypeMismatch},
                               std::pair{SequenceReferenceAvailability::Cycle, &CinematicErrors::SequenceReferenceCycle}};
        for (const auto &[state, expected] : cases) {
            const std::array evidence{SequenceResolvedReference{Asset('2'), SequenceReferenceKind::AudioClip, state, 1}};
            auto result = BuildSequenceCookPlan(asset, profile, evidence);
            RequireError(result, *expected);
            CHECK(result.ErrorValue().message.find("Track 10") != std::string::npos);
            CHECK(result.ErrorValue().message.find(Asset('2').ToString()) != std::string::npos);
        }
        RequireError(BuildSequenceCookPlan(asset, profile, {}), CinematicErrors::SequenceReferenceMissing);
    }

    TEST_CASE("Moved sequence references retain stable identity and recover only in a new available snapshot",
              "[unit][cinematic][sequence-cook][reload]") {
        const SequenceAsset asset = Referencing(SequenceTrackType::Audio, SequenceReferenceKind::AudioClip);
        const auto profile = GetSequenceCookProfile(SequenceCookTier::Standard).Value();
        const std::array moved{
            SequenceResolvedReference{Asset('2'), SequenceReferenceKind::AudioClip, SequenceReferenceAvailability::Moved, 1}};
        RequireError(BuildSequenceCookPlan(asset, profile, moved), CinematicErrors::SequenceReferenceMoved);

        const std::array refreshed{Available()};
        auto recovered = BuildSequenceCookPlan(asset, profile, refreshed);
        REQUIRE(recovered.HasValue());
        CHECK(asset.Data().tracks.front().references.front().asset == refreshed.front().asset);
    }

    TEST_CASE("Sequence cook rejects ambiguous and type-skewed resolution evidence", "[unit][cinematic][sequence-cook]") {
        const SequenceAsset asset = Referencing(SequenceTrackType::Audio, SequenceReferenceKind::AudioClip);
        const auto profile = GetSequenceCookProfile(SequenceCookTier::Standard).Value();
        const std::array duplicates{Available(), Available()};
        RequireError(BuildSequenceCookPlan(asset, profile, duplicates), CinematicErrors::SequenceSchemaDuplicate);
        const std::array wrongKind{Available(SequenceReferenceKind::SubSequence)};
        RequireError(BuildSequenceCookPlan(asset, profile, wrongKind), CinematicErrors::SequenceReferenceTypeMismatch);

        auto modifiedProfile = profile;
        ++modifiedProfile.limits.maximumTracks;
        RequireError(BuildSequenceCookPlan(asset, modifiedProfile, {}), CinematicErrors::SequenceSchemaMalformed);
    }

    TEST_CASE("Sequence cook enforces track key reference and nesting limits", "[unit][cinematic][sequence-cook]") {
        auto profile = GetSequenceCookProfile(SequenceCookTier::Compact).Value();
        auto data = ValidData();
        data.tracks.clear();
        for (std::uint64_t id = 1; id <= 33; ++id)
            data.tracks.push_back(Track(id));
        RequireError(BuildSequenceCookPlan(ValidAsset(std::move(data)), profile, {}), CinematicErrors::SequenceCookTierExceeded);

        data = ValidData();
        data.tracks.front().keyframeCount = profile.limits.maximumKeysPerTrack + 1;
        RequireError(BuildSequenceCookPlan(ValidAsset(std::move(data)), profile, {}), CinematicErrors::SequenceCookTierExceeded);

        const SequenceAsset nested = Referencing(SequenceTrackType::SubSequence, SequenceReferenceKind::SubSequence);
        const std::array child{Available(SequenceReferenceKind::SubSequence, 1)};
        RequireError(BuildSequenceCookPlan(nested, profile, child), CinematicErrors::SequenceCookTierExceeded);

        profile = GetSequenceCookProfile(SequenceCookTier::Standard).Value();
        const std::array admittedChild{Available(SequenceReferenceKind::SubSequence, 3)};
        auto admitted = BuildSequenceCookPlan(nested, profile, admittedChild);
        REQUIRE(admitted.HasValue());
        CHECK(admitted.Value().rootInclusiveNestingDepth == 4);
        const std::array excessiveChild{Available(SequenceReferenceKind::SubSequence, 4)};
        RequireError(BuildSequenceCookPlan(nested, profile, excessiveChild), CinematicErrors::SequenceCookTierExceeded);
    }

    TEST_CASE("Sequence error descriptors retain stable machine-readable identities", "[unit][cinematic][sequence-errors]") {
        CHECK(CinematicErrors::SequenceSchemaMalformed.code.Value() == "cinematic.sequence_schema.malformed");
        CHECK(CinematicErrors::SequenceSchemaDuplicate.code.Value() == "cinematic.sequence_schema.duplicate");
        CHECK(CinematicErrors::SequenceSchemaVersionUnsupported.code.Value() == "cinematic.sequence_schema.version_unsupported");
        CHECK(CinematicErrors::SequenceSchemaLimitExceeded.code.Value() == "cinematic.sequence_schema.limit_exceeded");
        CHECK(CinematicErrors::SequenceCookTierExceeded.code.Value() == "cinematic.sequence_cook.tier_exceeded");
        CHECK(CinematicErrors::SequenceReferenceMissing.code.Value() == "cinematic.sequence_reference.missing");
        CHECK(CinematicErrors::SequenceReferenceMoved.code.Value() == "cinematic.sequence_reference.move_pending");
        CHECK(CinematicErrors::SequenceReferenceUnloadable.code.Value() == "cinematic.sequence_reference.unloadable");
        CHECK(CinematicErrors::SequenceReferenceTypeMismatch.code.Value() == "cinematic.sequence_reference.type_mismatch");
        CHECK(CinematicErrors::SequenceReferenceCycle.code.Value() == "cinematic.sequence_reference.cycle");
    }
}  // namespace Horo::Cinematic
