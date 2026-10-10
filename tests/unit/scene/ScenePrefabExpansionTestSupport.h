#pragma once
#include "Horo/Scene/ScenePrefabExpansionOwner.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::SceneSource::ExpansionTestSupport {
    using namespace Horo;
    using namespace Horo::Prefab;
    using namespace Horo::SceneSource;

    /** @brief Real immutable source/resolver and authored Scene fixtures; no resolver/provider stub participates. */
    struct ExpansionFixture final {
        const Assets::AssetId asset = Assets::AssetId::FromBytes({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 17});
        const Application::HoroVersion version = Application::ParseHoroVersion("1.2.3").Value();
        const PrefabLimitProfile limits = PrefabLimitProfile::Create({}).Value();
        Assets::AssetRegistry registry;

        ExpansionFixture() {
            Publish();
        }

        void Publish() {
            REQUIRE(registry.Publish({RootRecord()}).status == Assets::AssetRegistryBuildStatus::Complete);
        }

        Assets::AssetRecord RootRecord() const {
            return {asset, Assets::AssetTypeId::Parse("core.prefab").Value(), ProjectPath::Parse("assets/root.prefab").Value(),
                    ProjectPath::Parse("assets/root.prefab.horo").Value()};
        }

        /** @brief Publishes the same authoritative root plus one real prefab/resource dependency. */
        void PublishDependency(const Assets::AssetId dependency, const std::string_view type, const std::string_view filename) {
            const std::string path = "assets/" + std::string{filename};
            REQUIRE(registry
                        .Publish({RootRecord(),
                                  {dependency, Assets::AssetTypeId::Parse(type).Value(), ProjectPath::Parse(path).Value(),
                                   ProjectPath::Parse(path + ".horo").Value()}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
        }

        /** @brief Owns a validated document and its canonical commitment, with an explicit stale-claim seam for negative tests. */
        PrefabDependencySource Source(PrefabDocumentData data, const std::optional<PrefabSourceRevision> claimed = std::nullopt) const {
            auto source = PrefabDocument::Create(std::move(data), limits);
            REQUIRE(source.HasValue());
            const auto bytes = source.Value().SerializeCanonical();
            REQUIRE(bytes.HasValue());
            const auto revision = claimed.value_or(PrefabSourceRevision{version, ComputeSha256(std::as_bytes(std::span{bytes.Value()}))});
            return {std::move(source).Value(), revision};
        }

        PrefabSourceResolverSnapshot Resolver(const std::string_view name = "Root",
                                              const std::optional<PrefabSourceRevision> claimed = std::nullopt) const {
            auto source = Source({.projectVersion = version,
                                  .assetId = asset,
                                  .objects = {{.localId = {0}, .name = std::string{name}},
                                              {.localId = {1}, .parentLocalId = LocalObjectId{0}, .name = "Child"}}},
                                 claimed);
            auto resolver = BuildPrefabSourceResolverSnapshot(registry.Snapshot(), {std::move(source)}, limits);
            REQUIRE(resolver.HasValue());
            return std::move(resolver).Value();
        }

        ScenePrefabExpansionRequest Request() const {
            return {.documentSession = 1,
                    .scene = {5},
                    .revision = {7},
                    .document = {.objects = {{.id = {9}, .name = "Owner"}},
                                 .prefabInstances = {{.instanceId = PrefabInstanceId::Create(4).Value(),
                                                      .sourcePrefab = PrefabAssetReference::Create(asset).Value(),
                                                      .parent = SceneObjectId{9},
                                                      .rootTransform = {.translation = {3, 0, 0}}}}},
                    .resolver = Resolver(),
                    .limits = limits};
        }
    };

    constexpr JoinOptions TestJoin{WaitPolicy::MainThreadPumpAllowed, Duration::FromMilliseconds(5'000)};

    /** @brief Proves a valid retry schedules, joins, publishes and retires the same actual owner. */
    inline void RequireCompletedRetryAndShutdown(ScenePrefabExpansionOwner &owner, const ScenePrefabExpansionRequest &request) {
        REQUIRE(owner.Submit(request).HasValue());
        REQUIRE(owner.Join(TestJoin).HasValue());
        REQUIRE(owner.TakeCompleted(request).Value());
        REQUIRE(owner.Shutdown(TestJoin).HasValue());
    }

    /** @brief Checks typed allocation failure and immutable source bytes after the injection scope has ended. */
    inline void RequireAllocationFailurePreservesSource(const Error &failure, const PrefabDocument &document,
                                                        const std::string &canonical) {
        CHECK(failure.code.Value() == PrefabErrors::ExpansionCacheAllocationFailed.code.Value());
        CHECK(document.SerializeCanonical().Value() == canonical);
    }
}  // namespace Horo::SceneSource::ExpansionTestSupport
