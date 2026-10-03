#include "Horo/Prefab/PrefabSceneExpansion.h"

#include <array>

namespace {
    int VerifyDefinition(const Horo::Prefab::ExpandedPrefabSceneSubtree &expanded) {
        using namespace Horo;
        Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}};
        for (const auto &entity : expanded.Entities())
            builder.Add(entity);
        const auto definition = std::move(builder).Build();
        return definition.HasValue() && definition.Value().Entities().size() == 2 ? 0 : 8;
    }
}  // namespace

int main() {
    using namespace Horo;
    const auto limits = Prefab::PrefabLimitProfile::Create({});
    const auto version = Application::ParseHoroVersion("1.2.3");
    if (limits.HasError() || version.HasError())
        return 1;
    std::array<std::uint8_t, 16> bytes{};
    bytes.back() = 1;
    const auto asset = Assets::AssetId::FromBytes(bytes);
    Assets::AssetRegistry registry;
    if (registry
            .Publish(
                {{asset, Assets::AssetTypeId::Parse("core.prefab").Value(), ProjectPath::Parse("assets/prefabs/consumer.prefab").Value(),
                  ProjectPath::Parse("assets/prefabs/consumer.prefab.horo").Value()}})
            .status != Assets::AssetRegistryBuildStatus::Complete)
        return 2;
    auto document =
        Prefab::PrefabDocument::Create({.projectVersion = version.Value(),
                                        .assetId = asset,
                                        .objects = {{.localId = {}, .name = "Root"},
                                                    {.localId = {7}, .parentLocalId = Prefab::LocalObjectId{}, .name = "Child"}}},
                                       limits.Value());
    if (document.HasError())
        return 3;
    Sha256Digest digest{};
    digest.bytes.back() = 1;
    auto resolver = Prefab::BuildPrefabSourceResolverSnapshot(registry.Snapshot(),
                                                              {{std::move(document).Value(), {version.Value(), digest}}}, limits.Value());
    if (resolver.HasError())
        return 4;
    auto candidate = resolver.Value().Resolve(asset, Prefab::PrefabInstanceId::Create(1).Value(), limits.Value());
    if (candidate.HasError())
        return 5;
    auto identities = Prefab::RemapPrefabCandidateToScene(candidate.Value(), {}, {}, limits.Value());
    if (identities.HasError())
        return 6;
    std::vector<Prefab::PrefabRuntimeComponentProjection> projections;
    for (const auto &object : candidate.Value().Objects())
        projections.emplace_back(object.key, Runtime::RuntimeComponentSet{});
    auto expanded = Prefab::ExpandPrefabSceneSubtree(candidate.Value(), identities.Value(), projections, {}, limits.Value());
    if (expanded.HasError())
        return 7;
    return VerifyDefinition(expanded.Value());
}
