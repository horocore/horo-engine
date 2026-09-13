#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabSceneIdentityRemap.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Prefab {
    namespace {
        Assets::AssetId RemapAsset(const std::uint8_t suffix) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = suffix;
            return Assets::AssetId::FromBytes(bytes);
        }

        Application::HoroVersion RemapProjectVersion() {
            return Application::ParseHoroVersion("1.2.3").Value();
        }

        PrefabSourceRevision RemapRevision() {
            Sha256Digest digest{};
            digest.bytes.back() = 1;
            return {RemapProjectVersion(), digest};
        }

        PrefabLimitProfile RemapLimits(PrefabProjectPolicy policy = {}) {
            return PrefabLimitProfile::Create(std::move(policy)).Value();
        }

        EffectivePrefabCandidate RemapCandidate(const PrefabInstanceId instance) {
            const Assets::AssetId asset = RemapAsset(1);
            PrefabDocument document =
                PrefabDocument::Create({.projectVersion = RemapProjectVersion(),
                                        .assetId = asset,
                                        .objects = {{.localId = {}, .name = "Root"},
                                                    {.localId = {7}, .parentLocalId = LocalObjectId{}, .name = "Child"}}},
                                       RemapLimits())
                    .Value();
            Assets::AssetRegistry registry;
            REQUIRE(registry
                        .Publish({{asset, Assets::AssetTypeId::Parse("core.prefab").Value(),
                                   ProjectPath::Parse("assets/prefabs/remap.prefab").Value(),
                                   ProjectPath::Parse("assets/prefabs/remap.prefab.horo").Value()}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            auto resolver = BuildPrefabSourceResolverSnapshot(registry.Snapshot(), {{std::move(document), RemapRevision()}}, RemapLimits());
            REQUIRE(resolver.HasValue());
            auto candidate = resolver.Value().Resolve(asset, instance, RemapLimits());
            REQUIRE(candidate.HasValue());
            return std::move(candidate).Value();
        }

        TEST_CASE("Prefab scene identity remap is deterministic and separates repeated instances", "[unit][prefab][scene_identity]") {
            const auto firstCandidate = RemapCandidate(PrefabInstanceId::Create(1).Value());
            const auto secondCandidate = RemapCandidate(PrefabInstanceId::Create(2).Value());
            const auto first = RemapPrefabCandidateToScene(firstCandidate, {}, {}, RemapLimits());
            const auto repeated = RemapPrefabCandidateToScene(firstCandidate, {}, {}, RemapLimits());
            const auto second = RemapPrefabCandidateToScene(secondCandidate, {}, {}, RemapLimits());
            REQUIRE(first.HasValue());
            REQUIRE(repeated.HasValue());
            REQUIRE(second.HasValue());
            REQUIRE(std::ranges::equal(first.Value().Mappings(), repeated.Value().Mappings()));
            REQUIRE(first.Value().Mappings()[0].scene != second.Value().Mappings()[0].scene);
            CHECK(first.Value().Mappings()[1].source.object.SourceObject() == LocalObjectId{7});
            CHECK(first.Value().Mappings()[1].sourcePrefab == RemapAsset(1));
        }

        TEST_CASE("Prefab scene identity remap rewrites every typed reference in one result",
                  "[unit][prefab][scene_identity][references]") {
            const auto candidate = RemapCandidate(PrefabInstanceId::Create(3).Value());
            const auto root = candidate.Objects()[0].key;
            const auto child = candidate.Objects()[1].key;
            const auto component = PrefabComponentInstanceId::Create(4).Value();
            const Gameplay::BehaviorInstanceId behavior{5};
            const auto asset = RemapAsset(9);
            const std::array requests{
                PrefabReferenceRewriteRequest{.owner = root, .kind = PrefabReferenceKind::Entity, .objectTarget = child},
                PrefabReferenceRewriteRequest{.owner = root,
                                              .kind = PrefabReferenceKind::Component,
                                              .objectTarget = child,
                                              .componentTarget = component},
                PrefabReferenceRewriteRequest{.owner = child,
                                              .kind = PrefabReferenceKind::Behavior,
                                              .objectTarget = root,
                                              .behaviorTarget = behavior},
                PrefabReferenceRewriteRequest{.owner = child, .kind = PrefabReferenceKind::Asset, .assetTarget = asset},
            };
            const auto remapped = RemapPrefabCandidateToScene(candidate, {}, requests, RemapLimits());
            REQUIRE(remapped.HasValue());
            REQUIRE(remapped.Value().References().size() == requests.size());
            CHECK(remapped.Value().References()[0].objectTarget == remapped.Value().Find(child));
            CHECK(remapped.Value().References()[1].componentTarget == component);
            CHECK(remapped.Value().References()[2].behaviorTarget == behavior);
            CHECK(remapped.Value().References()[3].assetTarget == asset);
            CHECK(remapped.Value().Revision() == candidate.Revision());
        }

        TEST_CASE("Prefab scene identity remap rejects collisions and malformed references atomically",
                  "[unit][prefab][scene_identity][malformed]") {
            const auto candidate = RemapCandidate(PrefabInstanceId::Create(6).Value());
            const auto baseline = RemapPrefabCandidateToScene(candidate, {}, {}, RemapLimits()).Value();
            const std::array occupied{baseline.Mappings().front().scene};
            const auto collision = RemapPrefabCandidateToScene(candidate, occupied, {}, RemapLimits());
            REQUIRE(collision.HasError());
            CHECK(collision.ErrorValue().code.Value() == PrefabErrors::IdentityCollision.code.Value());

            PrefabReferenceRewriteRequest malformed{.owner = candidate.Objects()[0].key,
                                                    .kind = PrefabReferenceKind::Entity,
                                                    .assetTarget = RemapAsset(9)};
            const auto invalid = RemapPrefabCandidateToScene(candidate, {}, std::span{&malformed, 1}, RemapLimits());
            REQUIRE(invalid.HasError());
            CHECK(invalid.ErrorValue().code.Value() == PrefabErrors::ReferenceRewriteInvalid.code.Value());
        }

        TEST_CASE("Prefab scene identity remap enforces the captured reference boundary", "[unit][prefab][scene_identity][boundary]") {
            const auto candidate = RemapCandidate(PrefabInstanceId::Create(7).Value());
            std::vector<PrefabReferenceRewriteRequest> requests(RemapLimits().Policy().maximumReferencedAssets + 1,
                                                                {.owner = candidate.Objects()[0].key,
                                                                 .kind = PrefabReferenceKind::Asset,
                                                                 .assetTarget = RemapAsset(9)});
            const auto result = RemapPrefabCandidateToScene(candidate, {}, requests, RemapLimits());
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == PrefabErrors::ReferenceCountExceeded.code.Value());
        }
    }  // namespace
}  // namespace Horo::Prefab
