#pragma once

#include "Horo/Runtime/Scene/PrimitiveCatalog.h"
#include "editor/document/EditorViewportSceneExtractor.h"
#include "editor/document/RuntimeSceneConversion.h"
#include "editor/document/SceneDocument.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Editor::SceneDocumentTestSupport {

    [[nodiscard]] inline Horo::Prefab::PrefabAssetReference PrefabAsset(const std::uint8_t suffix = 1) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = suffix;
        return Horo::Prefab::PrefabAssetReference::Create(Horo::Assets::AssetId::FromBytes(bytes)).Value();
    }

    [[nodiscard]] inline Horo::Runtime::NavigationSurfaceComponent NavigationSurface(const std::uint64_t id = 1) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = 7;
        return {
            .id = Horo::Navigation::SurfaceId::Create(id).Value(),
            .definition = Horo::Assets::AssetId::FromBytes(bytes),
            .profiles = {Horo::Navigation::NavigationAgentProfileId::Create(3).Value()},
        };
    }

    [[nodiscard]] inline Horo::Runtime::NavigationRegionComponent NavigationRegion(const std::uint64_t id = 1,
                                                                                   const std::uint64_t surface = 1) {
        return {
            .id = Horo::Navigation::NavigationRegionId::Create(id).Value(),
            .surface = Horo::Navigation::SurfaceId::Create(surface).Value(),
        };
    }

    [[nodiscard]] inline Horo::Runtime::NavigationModifierComponent NavigationModifier(const std::uint64_t id = 1,
                                                                                       const std::uint64_t surface = 1) {
        return {
            .id = Horo::Navigation::NavigationModifierId::Create(id).Value(),
            .surface = Horo::Navigation::SurfaceId::Create(surface).Value(),
        };
    }

    [[nodiscard]] inline Horo::Runtime::NavigationLinkComponent NavigationLink(const std::uint64_t id = 1,
                                                                               const std::uint64_t surface = 1) {
        return {
            .id = Horo::Navigation::NavigationLinkId::Create(id).Value(),
            .start = {.surface = Horo::Navigation::SurfaceId::Create(surface).Value()},
            .end = {.surface = Horo::Navigation::SurfaceId::Create(surface).Value(), .localPosition = {3.0F, 0.0F, 0.0F}},
            .profiles = {Horo::Navigation::NavigationAgentProfileId::Create(3).Value()},
        };
    }

    [[nodiscard]] inline Horo::Runtime::NavigationAgentComponent NavigationAgent(const std::uint64_t profile = 3,
                                                                                 const std::uint64_t filter = 5) {
        return {.profile = Horo::Navigation::NavigationAgentProfileId::Create(profile).Value(),
                .filter = Horo::Navigation::NavigationFilterId::Create(filter).Value(),
                .radiusOverride = 0.55F};
    }

    [[nodiscard]] inline std::unique_ptr<Horo::Runtime::RuntimeScene> MakeRuntimeScene(const Horo::Editor::SceneDocument &document) {
        auto definition = Horo::Editor::ConvertSceneDocumentToRuntime(document.Snapshot(), Horo::Runtime::SceneDefinitionId{1});
        REQUIRE((definition.HasValue()));
        auto scene = Horo::Runtime::RuntimeScene::Create(definition.Value(), Horo::Runtime::SceneRuntimeId{1});
        REQUIRE((scene.HasValue()));
        return std::move(scene).Value();
    }

    [[nodiscard]] inline bool NearlyEqual(const float lhs, const float rhs) noexcept {
        return std::fabs(lhs - rhs) < 0.0001F;
    }

    [[nodiscard]] inline Horo::Prefab::PrefabLimitProfile ScenePrefabLimits(Horo::Prefab::PrefabProjectPolicy policy = {}) {
        return Horo::Prefab::PrefabLimitProfile::Create(std::move(policy)).Value();
    }

    [[nodiscard]] inline Horo::Application::HoroVersion ScenePrefabVersion() {
        return Horo::Application::ParseHoroVersion("1.2.3").Value();
    }

    [[nodiscard]] inline Horo::Prefab::PrefabSourceRevision ScenePrefabRevision(const std::uint8_t suffix = 1) {
        Horo::Sha256Digest digest{};
        digest.bytes.back() = suffix;
        return {ScenePrefabVersion(), digest};
    }

    [[nodiscard]] inline Horo::Prefab::PrefabDocument ScenePrefabDocument(const Horo::Assets::AssetId asset,
                                                                          std::vector<Horo::Prefab::PrefabObjectNode> objects) {
        return Horo::Prefab::PrefabDocument::Create({.projectVersion = ScenePrefabVersion(),
                                                     .assetId = asset,
                                                     .objects = std::move(objects)},
                                                    ScenePrefabLimits())
            .Value();
    }

    [[nodiscard]] inline Horo::Assets::AssetRecord ScenePrefabRecord(const Horo::Assets::AssetId asset, const std::string &name) {
        return {asset, Horo::Assets::AssetTypeId::Parse("core.prefab").Value(),
                Horo::ProjectPath::Parse("assets/prefabs/" + name + ".prefab").Value(),
                Horo::ProjectPath::Parse("assets/prefabs/" + name + ".prefab.horo").Value()};
    }

    [[nodiscard]] inline Horo::Prefab::PrefabSourceResolverSnapshot ScenePrefabResolver(
        const Horo::Assets::AssetId asset, std::vector<Horo::Prefab::PrefabObjectNode> objects) {
        Horo::Assets::AssetRegistry registry;
        REQUIRE(registry.Publish({ScenePrefabRecord(asset, "scene-test")}).status == Horo::Assets::AssetRegistryBuildStatus::Complete);
        return Horo::Prefab::BuildPrefabSourceResolverSnapshot(registry.Snapshot(),
                                                               {{ScenePrefabDocument(asset, std::move(objects)), ScenePrefabRevision()}},
                                                               ScenePrefabLimits())
            .Value();
    }

    [[nodiscard]] inline std::vector<std::byte> Bytes(const std::string_view value) {
        std::vector<std::byte> bytes;
        bytes.reserve(value.size());
        for (const char character : value)
            bytes.push_back(static_cast<std::byte>(character));
        return bytes;
    }
}  // namespace Horo::Editor::SceneDocumentTestSupport
