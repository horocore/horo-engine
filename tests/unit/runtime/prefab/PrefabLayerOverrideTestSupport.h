#pragma once
#include "Horo/Prefab/PrefabErrors.h"
#include "PrefabValueOverrideTestSupport.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace Horo::Prefab::LayerTest {
    using namespace ValueTestSupport;

    /** @brief Keeps exact typed diagnostics visible without introducing fixture-only source resolution. */
    template <class T> T Take(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().message : "Canonical operation accepted."));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    /** @brief Supplies distinct real stable asset identities for the source/variant/nested test graph. */
    inline Assets::AssetId Id(std::uint8_t value) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return Assets::AssetId::FromBytes(bytes);
    }

    /** @brief Explicit semantic source revisions are separate from resolver-verified canonical byte commitments. */
    inline PrefabSourceRevision Revision(std::uint8_t value) {
        Sha256Digest bytes{};
        bytes.bytes.back() = value;
        return {Application::ParseHoroVersion("1.2.3").Value(), bytes};
    }

    /** @brief Addresses the same stable property inside one nested occurrence without relying on flattened array indexes. */
    inline PrefabPropertyAddress NestedProperty() {
        const std::array scope{LocalObjectId{7}};
        return Take(PrefabPropertyAddress::Create(Take(PrefabObjectAddress::Create(scope, {0})), Type(),
                                                  PrefabComponentInstanceId::Create(1).Value(), PrefabPropertyId::Create(7).Value()));
    }

    /** @brief Uses real canonical source documents and the existing registry/graph builder at each incremental layer. */
    class Graph final {
    public:
        Graph() : schemas(std::make_shared<const PrefabValueSchemaSnapshot>(Schemas())) {
            const auto original = Sources();
            sources.push_back(original.Sources().front());
        }

        PrefabSourceResolverSnapshot Snapshot(PrefabSourceRevisionPolicy policy = PrefabSourceRevisionPolicy::RequireExact) {
            std::vector<Assets::AssetRecord> records;
            for (const auto &source : sources) {
                const auto asset = source.document.Data().assetId;
                const std::string path = "assets/" + asset.ToString() + ".prefab";
                records.push_back({asset, Assets::AssetTypeId::Parse("core.prefab").Value(), ProjectPath::Parse(path).Value(),
                                   ProjectPath::Parse(path + ".horo").Value()});
            }
            REQUIRE(registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
            return Take(BuildPrefabSourceResolverSnapshot(registry.Snapshot(), sources, Limits(), {}, schemas, policy));
        }

        EffectivePrefabCandidate Resolve(Assets::AssetId asset) {
            return Take(Snapshot().Resolve(asset, PrefabInstanceId::Create(9).Value(), Limits()));
        }

        PrefabValueOverrideSet Assign(Assets::AssetId source, const PrefabPropertyAddress &target, double value) {
            auto resolved = Resolve(source);
            return Take(PreparePrefabValueEdit(resolved, {}, {PrefabValueEditKind::Assign, target, value}, schemas.get(), Limits()))
                .rebased;
        }

        void AddVariant(std::uint8_t owner, std::uint8_t parent, PrefabSourceRevision parentRevision, PrefabValueOverrideSet layer) {
            PrefabComposition composition{.variantParent = Take(PrefabAssetReference::Create(Id(parent))),
                                          .variantAuthoredAgainst = parentRevision,
                                          .variantOverrides = std::move(layer)};
            auto document = Take(PrefabDocument::Create({.projectVersion = Revision(owner).projectVersion,
                                                         .assetId = Id(owner),
                                                         .composition = std::move(composition),
                                                         .referencedAssets = {Id(parent)}},
                                                        Limits()));
            sources.push_back({std::move(document), Revision(owner)});
        }

        /** @brief Builds actual pure parent edges for exact boundary/cycle preflight tests. */
        void AddParent(std::uint8_t owner, std::uint8_t parent, PrefabSourceRevision parentRevision) {
            PrefabComposition composition{.variantParent = Take(PrefabAssetReference::Create(Id(parent))),
                                          .variantAuthoredAgainst = parentRevision};
            auto document = Take(PrefabDocument::Create({.projectVersion = Revision(owner).projectVersion,
                                                         .assetId = Id(owner),
                                                         .composition = std::move(composition),
                                                         .referencedAssets = {Id(parent)}},
                                                        Limits()));
            sources.push_back({std::move(document), Revision(owner)});
        }

        /** @brief Mounts one real child under a source root for nested-depth/occurrence ordering tests. */
        void AddNested(std::uint8_t owner, std::uint8_t child, PrefabSourceRevision childRevision) {
            PrefabComposition composition{.nestedPlacements = {{.placementLocalId = {7},
                                                                .sourcePrefab = Take(PrefabAssetReference::Create(Id(child))),
                                                                .authoredAgainst = childRevision}}};
            auto document = Take(PrefabDocument::Create({.projectVersion = Revision(owner).projectVersion,
                                                         .assetId = Id(owner),
                                                         .objects = {{.localId = {0}, .name = "Host"}},
                                                         .composition = std::move(composition),
                                                         .referencedAssets = {Id(child)}},
                                                        Limits()));
            sources.push_back({std::move(document), Revision(owner)});
        }

        std::vector<PrefabDependencySource> sources;
        Assets::AssetRegistry registry;
        std::shared_ptr<const PrefabValueSchemaSnapshot> schemas;
    };

    /** @brief Reads the real resolved payload at a stable source-local address. */
    inline double Speed(const EffectivePrefabCandidate &source, const PrefabObjectAddress &address) {
        const auto objects = source.Objects();
        const auto found = std::ranges::find_if(objects, [&](const auto &object) {
            return object.key.object == address;
        });
        REQUIRE(found != objects.end());
        REQUIRE(found->object.components.size() == 1);
        const auto &bytes = found->object.components[0].component.payload;
        const std::string payload(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        return nlohmann::json::parse(payload).at("speed").get<double>();
    }
}  // namespace Horo::Prefab::LayerTest
