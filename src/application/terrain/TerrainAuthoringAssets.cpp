#include "Horo/Application/TerrainAuthoringAssets.h"

#include "Horo/Assets/AssetSourcePublication.h"

#include <new>
#include <type_traits>

namespace Horo::Application {
    static_assert(std::is_nothrow_move_constructible_v<TerrainAuthoringAssetCreated>);

    namespace {
        bool Destination(const TerrainAuthoringAssetRequest &request) {
            const auto path = std::string_view(request.destination.String());
            const auto extension = request.kind == TerrainAuthoringAssetKind::LayerSet ? ".terrainlayers" : ".foliagetype";
            const auto relative = ProjectLayout::AssetRoot(request.root).lexically_relative(request.root).generic_u8string();
            const auto prefix = std::string(relative.begin(), relative.end()) + "/";
            return path.starts_with(prefix) && path.ends_with(extension);
        }

        Result<void> Reference(const Assets::AssetRegistrySnapshot &registry, Assets::AssetId asset, std::string_view type) {
            const auto *record = registry.Find(asset);
            if (!record || record->type.Value() != type)
                return Result<void>::Failure(MakeError(Terrain::TerrainErrors::IdentityInvalid));
            return Result<void>::Success();
        }

        Result<std::vector<std::byte>> LayerSet(const TerrainAuthoringAssetEnvironment &environment,
                                                const TerrainAuthoringAssetRequest &request, Assets::AssetId asset,
                                                const Assets::AssetRegistrySnapshot &registry) {
            using Return = Result<std::vector<std::byte>>;
            if (request.materials.empty() || request.materials.size() > 16)
                return Return::Failure(MakeError(Terrain::TerrainSourceErrors::LimitExceeded));
            Terrain::TerrainAuthoringLayerSet source{asset, {}};
            source.layers.id = Terrain::TerrainLayerSetId::Create(asset.Bytes()).Value();
            source.layers.revision = Terrain::TerrainContentRevision::Create(1).Value();
            source.layers.layerCount = static_cast<std::uint8_t>(request.materials.size());
            for (std::size_t i = 0; i < request.materials.size(); ++i) {
                if (auto valid = Reference(registry, request.materials[i], "core.material"); valid.HasError())
                    return Return::Failure(std::move(valid).ErrorValue());
                auto identity = environment.newIdentity();
                if (identity.HasError())
                    return Return::Failure(std::move(identity).ErrorValue());
                auto layer = Terrain::TerrainLayerId::Create(identity.Value().Bytes());
                if (layer.HasError())
                    return Return::Failure(std::move(layer).ErrorValue());
                source.layers.layers[i] = {layer.Value(), Terrain::TerrainMaterialAssetId::Create(request.materials[i].Bytes()).Value(),
                                           1000};
            }
            return Terrain::EncodeTerrainAuthoringLayerSet(source, environment.configuration);
        }

        Result<void> FoliageReferences(const Assets::AssetRegistrySnapshot &registry, const Terrain::FoliageTypeDefinitionData &data) {
            if (auto material = Reference(registry, Assets::AssetId::FromBytes(data.assets.material.Bytes()), "core.material");
                material.HasError())
                return material;
            if (data.assets.meshLodCount > data.assets.meshLods.size())
                return Result<void>::Failure(MakeError(Terrain::TerrainSourceErrors::LimitExceeded));
            for (std::size_t i = 0; i < data.assets.meshLodCount; ++i) {
                auto mesh = Reference(registry, Assets::AssetId::FromBytes(data.assets.meshLods[i].Bytes()), "core.mesh");
                if (mesh.HasError())
                    return mesh;
            }
            return Result<void>::Success();
        }

        Result<std::vector<std::byte>> Foliage(const TerrainAuthoringAssetEnvironment &environment,
                                               const TerrainAuthoringAssetRequest &request, Assets::AssetId asset,
                                               const Assets::AssetRegistrySnapshot &registry) {
            using Return = Result<std::vector<std::byte>>;
            Terrain::FoliageTypeSource source{asset, request.foliage};
            source.definition.type = Terrain::FoliageTypeId::Create(asset.Bytes()).Value();
            source.definition.revision = Terrain::FoliageDefinitionRevision::Create(1).Value();
            if (auto references = FoliageReferences(registry, source.definition); references.HasError())
                return Return::Failure(std::move(references).ErrorValue());
            return Terrain::EncodeFoliageTypeSource(source, environment.configuration,
                                                    {Terrain::FoliageDefinitionCapabilityBit<
                                                        Terrain::FoliageDefinitionCapability::CpuCulling>});
        }

        Result<std::vector<std::byte>> Payload(const TerrainAuthoringAssetEnvironment &environment,
                                               const TerrainAuthoringAssetRequest &request, Assets::AssetId asset,
                                               const Assets::AssetRegistrySnapshot &registry) {
            switch (request.kind) {
                case TerrainAuthoringAssetKind::LayerSet:
                    return LayerSet(environment, request, asset, registry);
                case TerrainAuthoringAssetKind::FoliageType:
                    return Foliage(environment, request, asset, registry);
            }
            return Result<std::vector<std::byte>>::Failure(MakeError(Terrain::TerrainSourceErrors::UnsupportedFormat));
        }
    }  // namespace

    /** @copydoc CreateTerrainAuthoringAsset */
    Result<TerrainAuthoringAssetCreated> CreateTerrainAuthoringAsset(const TerrainAuthoringAssetEnvironment &environment,
                                                                     const TerrainAuthoringAssetRequest &request,
                                                                     const CancellationToken &cancellation) {
        using Return = Result<TerrainAuthoringAssetCreated>;
        if (!environment.newIdentity)
            return Return::Failure(MakeError(Terrain::TerrainErrors::IdentityInvalid));
        try {
            if (!Destination(request))
                return Return::Failure(MakeError(Assets::AssetSourceErrors::InvalidPath));
            auto identity = environment.newIdentity();
            if (identity.HasError())
                return Return::Failure(std::move(identity).ErrorValue());
            if (!identity.Value().IsValid())
                return Return::Failure(MakeError(Terrain::TerrainErrors::IdentityInvalid));
            auto payload = Payload(environment, request, identity.Value(), environment.registry.Snapshot());
            if (payload.HasError())
                return Return::Failure(std::move(payload).ErrorValue());
            const auto type = Assets::AssetTypeId::Parse(request.kind == TerrainAuthoringAssetKind::LayerSet ? "core.terrain.layerset"
                                                                                                             : "core.foliage.type")
                                  .Value();
            TerrainAuthoringAssetCreated created{identity.Value(), request.destination, {}, {}};
            PreparedAuthoredProjectAssetBatch batch{identity.Value().ToString(),
                                                    request.root,
                                                    {{identity.Value(), type, request.destination, std::move(payload).Value()}}};
            ProjectAssetImportCommitter publication(environment.mutations, environment.files, &environment.registry);
            auto committed = publication.CommitAuthored(batch, cancellation);
            created.publication = publication.LastReceipt();
            if (committed.HasError())
                created.failure.emplace(std::move(committed).ErrorValue());
            return Return::Success(std::move(created));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(Terrain::TerrainSourceErrors::LimitExceeded));
        } catch (...) {
            return Return::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        }
    }
}  // namespace Horo::Application
