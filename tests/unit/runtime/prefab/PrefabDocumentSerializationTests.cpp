#include "Horo/Prefab/PrefabDocument.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "PrefabTestUtils.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Horo::Prefab {
    namespace {
        using Test::Asset;

        const Application::HoroVersion kProjectVersion = Application::ParseHoroVersion("1.2.3").Value();
        const Gameplay::ComponentTypeId kComponentType = Gameplay::ComponentTypeId::Parse("game.tests.prefab_serialization").Value();
        const Gameplay::BehaviorTypeId kBehaviorType = Gameplay::BehaviorTypeId::Parse("game.tests.prefab_serialization_behavior").Value();

        PrefabSourceRevision Revision() {
            const auto digest = [] {
                Sha256Digest value{};
                value.bytes.back() = 7;
                return value;
            }();
            return {.projectVersion = kProjectVersion, .contentDigest = digest};
        }

        RawComponentPayload Component(const std::uint64_t instance, std::vector<std::byte> bytes) {
            return {.instance = PrefabComponentInstanceId::Create(instance).Value(),
                    .component = {.typeId = kComponentType,
                                  .schemaVersion = 3,
                                  .encoding = Gameplay::ComponentPayloadEncoding::CanonicalJson,
                                  .payload = std::move(bytes)}};
        }

        Gameplay::BehaviorComponent Behavior(const std::uint64_t instance) {
            return {.instanceId = {instance},
                    .typeId = kBehaviorType,
                    .schemaVersion = 2,
                    .enabled = true,
                    .fields = {{.name = "speed", .value = 2.0},
                               {.name = "offset", .value = Math::Vec3{1.0F, -0.0F, 3.0F}},
                               {.name = "label", .value = std::string{"héllo"}}}};
        }

        PrefabObjectNode Root() {
            return {.localId = {}, .parentLocalId = std::nullopt, .name = "Rööt"};
        }

        PrefabDocumentData CanonicalData() {
            PrefabDocumentData data = {.projectVersion = kProjectVersion,
                                       .assetId = Asset(1),
                                       .objects = {Root(),
                                                   {.localId = {2}, .parentLocalId = LocalObjectId{}, .name = "Second"},
                                                   {.localId = {1}, .parentLocalId = LocalObjectId{}, .name = "First"}},
                                       .referencedAssets = {Asset(3), Asset(2)}};
            data.objects.front().components = {Component(2, {std::byte{0x42}}),
                                               Component(1, {std::byte{0x00}, std::byte{0x7f}, std::byte{0xff}})};
            data.objects.front().behaviors = {Behavior(9), Behavior(3)};
            return data;
        }

        PrefabLimitProfile Limits() {
            const PrefabProjectPolicy policy{};
            return PrefabLimitProfile::Create(policy).Value();
        }

        Result<PrefabDocument> CreateDocument(PrefabDocumentData data) {
            const auto limits = Limits();
            return PrefabDocument::Create(std::move(data), limits);
        }

        Result<std::string> SerializeDocument(PrefabDocumentData data) {
            const auto document = CreateDocument(std::move(data));
            if (document.HasError())
                return Result<std::string>::Failure(document.ErrorValue());
            return document.Value().SerializeCanonical();
        }

        Result<PrefabDocument> ParseSerialized(PrefabDocumentData data) {
            const auto encoded = SerializeDocument(std::move(data));
            if (encoded.HasError())
                return Result<PrefabDocument>::Failure(encoded.ErrorValue());
            return PrefabDocument::Parse(encoded.Value(), Limits());
        }

        PrefabDocument ParsedDocument(PrefabDocumentData data) {
            const auto parsed = ParseSerialized(std::move(data));
            REQUIRE(parsed.HasValue());
            return parsed.Value();
        }

        Result<std::string> CanonicalSource() {
            return SerializeDocument(CanonicalData());
        }

        std::string ReplaceFirst(std::string source, const std::string_view from, const std::string_view to) {
            const std::size_t position = source.find(from);
            if (position == std::string::npos)
                return {};
            source.replace(position, from.size(), to);
            return source;
        }
    }  // namespace

    TEST_CASE("Prefab canonical serialization round-trips project identity and opaque bytes", "[unit][prefab][serialization]") {
        const auto encoded = CanonicalSource();
        REQUIRE(encoded.HasValue());

        const auto parsed = PrefabDocument::Parse(encoded.Value(), Limits());
        REQUIRE(parsed.HasValue());
        const auto reencoded = parsed.Value().SerializeCanonical();
        REQUIRE(reencoded.HasValue());
        REQUIRE(reencoded.Value() == encoded.Value());
        REQUIRE(parsed.Value().Data().assetId == Asset(1));
        REQUIRE(parsed.Value().Data().objects.front().components.front().component.payload ==
                std::vector<std::byte>{std::byte{0x00}, std::byte{0x7f}, std::byte{0xff}});
        REQUIRE(parsed.Value().Data().objects.front().name == "Rööt");
    }

    TEST_CASE("Prefab equivalent source order serializes byte-identically", "[unit][prefab][serialization]") {
        PrefabDocumentData firstData = CanonicalData();
        PrefabDocumentData secondData = firstData;
        std::reverse(secondData.referencedAssets.begin(), secondData.referencedAssets.end());
        std::reverse(secondData.objects.begin() + 1, secondData.objects.end());
        std::reverse(secondData.objects.front().components.begin(), secondData.objects.front().components.end());
        std::reverse(secondData.objects.front().behaviors.begin(), secondData.objects.front().behaviors.end());
        std::reverse(secondData.objects.front().behaviors.front().fields.begin(),
                     secondData.objects.front().behaviors.front().fields.end());

        const auto first = CreateDocument(std::move(firstData));
        const auto second = CreateDocument(std::move(secondData));
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        const auto firstBytes = first.Value().SerializeCanonical();
        const auto secondBytes = second.Value().SerializeCanonical();
        REQUIRE(firstBytes.HasValue());
        REQUIRE(secondBytes.HasValue());
        REQUIRE(firstBytes.Value() == secondBytes.Value());
    }

    TEST_CASE("Prefab typed behavior values preserve every canonical kind", "[unit][prefab][serialization]") {
        PrefabDocumentData data = CanonicalData();
        data.objects.front().behaviors.front().fields = {
            {.name = "null", .value = std::monostate{}},
            {.name = "bool", .value = true},
            {.name = "integer", .value = std::int64_t{-7}},
            {.name = "number", .value = 2.0},
            {.name = "string", .value = std::string{"text"}},
            {.name = "vec2", .value = Math::Vec2{1.0F, 2.0F}},
            {.name = "vec3", .value = Math::Vec3{3.0F, 4.0F, 5.0F}},
            {.name = "quaternion", .value = Math::Quaternion{0.0F, 1.0F, 0.0F, 0.0F}},
        };
        const auto parsed = ParsedDocument(std::move(data));
        REQUIRE(parsed.Data().objects.front().behaviors.back().fields.size() == 8);
    }

    TEST_CASE("Prefab nested composition serializes and parses as typed AssetId data", "[unit][prefab][serialization]") {
        PrefabDocumentData data = CanonicalData();
        data.composition = PrefabComposition{.nestedPlacements = {{.placementLocalId = {99},
                                                                   .parentLocalId = LocalObjectId{},
                                                                   .sourcePrefab = PrefabAssetReference::Create(Asset(2)).Value(),
                                                                   .authoredAgainst = Revision()}}};
        const auto parsed = ParsedDocument(std::move(data));
        REQUIRE(parsed.Data().composition.has_value());
        REQUIRE(parsed.Data().composition->nestedPlacements.front().sourcePrefab.Asset() == Asset(2));
    }

    TEST_CASE("Prefab variant composition serializes and parses its exact parent revision", "[unit][prefab][serialization]") {
        PrefabDocumentData data = CanonicalData();
        data.objects.clear();
        data.composition =
            PrefabComposition{.variantParent = PrefabAssetReference::Create(Asset(2)).Value(), .variantAuthoredAgainst = Revision()};
        const auto parsed = ParsedDocument(std::move(data));
        REQUIRE(parsed.Data().composition->variantParent->Asset() == Asset(2));
        REQUIRE(parsed.Data().composition->variantAuthoredAgainst == Revision());
    }

    TEST_CASE("Prefab parser rejects duplicate, malformed, oversized and path-authoritative input", "[unit][prefab][serialization]") {
        const auto encoded = CanonicalSource();
        REQUIRE(encoded.HasValue());

        const std::string duplicate =
            ReplaceFirst(encoded.Value(), "\"projectVersion\": \"1.2.3\"", "\"projectVersion\": \"1.2.3\", \"projectVersion\": \"1.2.3\"");
        REQUIRE_FALSE(duplicate.empty());
        const auto duplicateResult = PrefabDocument::Parse(duplicate, Limits());
        REQUIRE(duplicateResult.HasError());

        REQUIRE(PrefabDocument::Parse("{", Limits()).HasError());

        PrefabSourceParseLimits sourceLimits;
        sourceLimits.maximumSourceBytes = encoded.Value().size() - 1;
        const auto oversized = PrefabDocument::Parse(encoded.Value(), Limits(), sourceLimits);
        REQUIRE(oversized.HasError());

        sourceLimits = {};
        sourceLimits.maximumJsonDepth = 1;
        REQUIRE(PrefabDocument::Parse(encoded.Value(), Limits(), sourceLimits).HasError());

        const std::string pathReference = ReplaceFirst(encoded.Value(), Asset(1).ToString(), "assets/prefabs/hero.prefab");
        REQUIRE_FALSE(pathReference.empty());
        const auto pathResult = PrefabDocument::Parse(pathReference, Limits());
        REQUIRE(pathResult.HasError());
        REQUIRE(pathResult.ErrorValue().code.Value() == PrefabErrors::ReferenceInvalid.code.Value());

        const std::string pathField = "{\"projectVersion\":\"1.2.3\",\"assetId\":\"" + Asset(1).ToString() +
                                      "\",\"objects\":[],\"referencedAssets\":[],\"sourcePath\":\"assets/prefabs/hero.prefab\"}";
        REQUIRE(PrefabDocument::Parse(pathField, Limits()).HasError());
    }

    TEST_CASE("Prefab parser rejects unsupported versions, non-finite values and invalid UTF-8", "[unit][prefab][serialization]") {
        const auto encoded = CanonicalSource();
        REQUIRE(encoded.HasValue());

        const std::string future = ReplaceFirst(encoded.Value(), "\"1.2.3\"", "\"1.2.4\"");
        REQUIRE_FALSE(future.empty());
        PrefabSourceParseLimits expected;
        expected.expectedProjectVersion = kProjectVersion;
        const auto unsupported = PrefabDocument::Parse(future, Limits(), expected);
        REQUIRE(unsupported.HasError());
        REQUIRE(unsupported.ErrorValue().code.Value() == PrefabErrors::UnsupportedPrefabSchema.code.Value());

        const std::string nonFinite = ReplaceFirst(encoded.Value(), "2.0", "1e9999");
        REQUIRE_FALSE(nonFinite.empty());
        REQUIRE(PrefabDocument::Parse(nonFinite, Limits()).HasError());

        std::string invalidUtf8{"\xc3\x28", 2};
        REQUIRE(PrefabDocument::Parse(invalidUtf8, Limits()).HasError());

        PrefabDocumentData invalid = CanonicalData();
        invalid.objects.front().behaviors.front().fields.front().value = std::numeric_limits<double>::infinity();
        REQUIRE(CreateDocument(std::move(invalid)).HasError());
    }

    TEST_CASE("Prefab parser admits exact source-byte bounds and rejects unsupported encodings without publication",
              "[unit][prefab][serialization]") {
        const auto encoded = CanonicalSource();
        REQUIRE(encoded.HasValue());

        PrefabSourceParseLimits exact;
        exact.maximumSourceBytes = encoded.Value().size();
        REQUIRE(PrefabDocument::Parse(encoded.Value(), Limits(), exact).HasValue());

        const std::string unsupportedEncoding = ReplaceFirst(encoded.Value(), "canonicalJson", "binary");
        REQUIRE_FALSE(unsupportedEncoding.empty());
        const auto rejected = PrefabDocument::Parse(unsupportedEncoding, Limits());
        REQUIRE(rejected.HasError());
        REQUIRE(rejected.ErrorValue().code.Value() == PrefabErrors::UnsupportedPrefabSchema.code.Value());
        const auto reparsed = PrefabDocument::Parse(encoded.Value(), Limits());
        REQUIRE(reparsed.HasValue());
        REQUIRE(reparsed.Value().Data().assetId == Asset(1));

        PrefabDocumentData invalid = CanonicalData();
        invalid.objects.front().components.front().component.encoding = static_cast<Gameplay::ComponentPayloadEncoding>(99);
        REQUIRE(CreateDocument(std::move(invalid)).HasError());

        PrefabProjectPolicy lowerPolicy;
        lowerPolicy.maximumSourcePayloadBytes = 64;
        const auto lowerLimits = PrefabLimitProfile::Create(lowerPolicy);
        REQUIRE(lowerLimits.HasValue());
        REQUIRE(PrefabDocument::Parse(encoded.Value(), lowerLimits.Value()).HasError());
    }
}  // namespace Horo::Prefab
