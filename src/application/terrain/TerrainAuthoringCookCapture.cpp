#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Foundation/PathUtils.h"
#include "TerrainAuthoringCookInternal.h"

#include <algorithm>
#include <set>

namespace Horo::Application::TerrainCookDetail {
    namespace {
        using IdSet = std::set<Assets::AssetId>;

        Result<Assets::AssetSourceCapture> Read(const TerrainAuthoringCookRequest &request, const Assets::AssetRegistrySnapshot &registry,
                                                Assets::AssetId asset, std::string_view type, std::size_t maximum,
                                                const CancellationToken &cancellation) {
            using Return = Result<Assets::AssetSourceCapture>;
            const auto *record = registry.Find(asset);
            if (!record || record->type.Value() != type)
                return Return::Failure(MakeError(TerrainAuthoringCookErrors::Invalid));
            auto source = Assets::CaptureAssetSource(request.sourceRoot, Foundation::Paths::Resolve(request.sourceRoot, record->sourcePath),
                                                     maximum, cancellation);
            if (source.HasError())
                return source;
            if (!source.Value().identity.exists)
                return Return::Failure(MakeError(TerrainAuthoringCookErrors::Stale));
            return source;
        }

        Result<void> Require(IdSet &ids, const Assets::AssetRegistrySnapshot &registry, Assets::AssetId asset, std::string_view type,
                             std::size_t maximum) {
            const auto *record = registry.Find(asset);
            if (!record || record->type.Value() != type)
                return Result<void>::Failure(MakeError(TerrainAuthoringCookErrors::Invalid));
            ids.insert(asset);
            if (ids.size() > maximum)
                return Result<void>::Failure(MakeError(Terrain::TerrainSourceErrors::LimitExceeded));
            return Result<void>::Success();
        }

        Result<void> AddLayers(const TerrainAuthoringCookRequest &request, const Assets::AssetRegistrySnapshot &registry,
                               const Terrain::TerrainAuthoringSource &source, IdSet &ids, const CancellationToken &cancellation) {
            if (!source.layers)
                return Result<void>::Success();
            auto captured = Read(request, registry, source.layers->asset, "core.terrain.layerset", 4096, cancellation);
            if (captured.HasError())
                return Result<void>::Failure(std::move(captured).ErrorValue());
            auto layers =
                Terrain::DecodeTerrainAuthoringLayerSet(captured.Value().bytes, source.layers->asset, request.profile.configuration);
            if (layers.HasError())
                return Result<void>::Failure(std::move(layers).ErrorValue());
            if (auto valid = Terrain::ValidateTerrainAuthoringLayerBinding(source, layers.Value(), captured.Value().identity.digest);
                valid.HasError())
                return valid;
            ids.insert(source.layers->asset);
            for (std::size_t i = 0; i < layers.Value().layers.layerCount; ++i) {
                auto required = Require(ids, registry, Assets::AssetId::FromBytes(layers.Value().layers.layers[i].material.Bytes()),
                                        "core.material", request.profile.assets.maximumAssets);
                if (required.HasError())
                    return required;
            }
            return Result<void>::Success();
        }

        Result<void> AddDefinitionAssets(const TerrainAuthoringCookRequest &request, const Assets::AssetRegistrySnapshot &registry,
                                         const Terrain::FoliageTypeDefinitionData &definition, IdSet &ids) {
            auto material = Require(ids, registry, Assets::AssetId::FromBytes(definition.assets.material.Bytes()), "core.material",
                                    request.profile.assets.maximumAssets);
            if (material.HasError())
                return material;
            for (std::size_t i = 0; i < definition.assets.meshLodCount; ++i) {
                auto mesh = Require(ids, registry, Assets::AssetId::FromBytes(definition.assets.meshLods[i].Bytes()), "core.mesh",
                                    request.profile.assets.maximumAssets);
                if (mesh.HasError())
                    return mesh;
            }
            return Result<void>::Success();
        }

        Result<Terrain::FoliageTypeSource> ReadDefinition(const TerrainAuthoringCookRequest &request,
                                                          const Assets::AssetRegistrySnapshot &registry,
                                                          const Terrain::FoliageAuthoringDefinitionReference &reference,
                                                          const CancellationToken &cancellation) {
            using Return = Result<Terrain::FoliageTypeSource>;
            auto source = Read(request, registry, reference.asset, "core.foliage.type", 1024, cancellation);
            if (source.HasError())
                return Return::Failure(std::move(source).ErrorValue());
            const Terrain::FoliageDefinitionCapabilitySet capabilities{
                Terrain::FoliageDefinitionCapabilityBit<Terrain::FoliageDefinitionCapability::CpuCulling>};
            auto definition = Terrain::DecodeFoliageTypeSource(source.Value().bytes, reference.asset, request.profile.configuration,
                                                               capabilities, cancellation);
            if (definition.HasError())
                return definition;
            if (source.Value().identity.digest != reference.sourceDigest || definition.Value().definition.type != reference.type ||
                definition.Value().definition.revision != reference.revision)
                return Return::Failure(MakeError(TerrainAuthoringCookErrors::Stale));
            return definition;
        }

        Result<std::vector<Terrain::FoliageTypeSource>> AddDefinitions(const TerrainAuthoringCookRequest &request,
                                                                       const Assets::AssetRegistrySnapshot &registry,
                                                                       const Terrain::TerrainAuthoringSource &source, IdSet &ids,
                                                                       const CancellationToken &cancellation) {
            using Return = Result<std::vector<Terrain::FoliageTypeSource>>;
            std::vector<Terrain::FoliageTypeSource> definitions;
            definitions.reserve(source.foliage.definitions.size());
            for (const auto &reference : source.foliage.definitions) {
                auto definition = ReadDefinition(request, registry, reference, cancellation);
                if (definition.HasError())
                    return Return::Failure(std::move(definition).ErrorValue());
                ids.insert(reference.asset);
                if (auto assets = AddDefinitionAssets(request, registry, definition.Value().definition, ids); assets.HasError())
                    return Return::Failure(std::move(assets).ErrorValue());
                definitions.push_back(std::move(definition).Value());
            }
            return Return::Success(std::move(definitions));
        }

        Result<Terrain::TerrainAuthoringSource> ReadRoot(const TerrainAuthoringCookRequest &request,
                                                         const Assets::AssetRegistrySnapshot &registry,
                                                         const CancellationToken &cancellation) {
            using Return = Result<Terrain::TerrainAuthoringSource>;
            auto bytes = Read(request, registry, request.asset, "core.terrain", request.profile.source.maximumBytes, cancellation);
            if (bytes.HasError())
                return Return::Failure(std::move(bytes).ErrorValue());
            if (bytes.Value().identity.digest != request.fence.sourceDigest)
                return Return::Failure(MakeError(TerrainAuthoringCookErrors::Stale));
            // Portable cook capability is explicit policy, never the workspace/session admission token.
            auto source =
                Terrain::DecodeTerrainAuthoringSource(bytes.Value().bytes, request.asset, request.profile.configuration.Data().capability,
                                                      request.profile.source, cancellation);
            if (source.HasError())
                return source;
            if (source.Value().source.dataset != request.fence.dataset || source.Value().source.revision != request.fence.source)
                return Return::Failure(MakeError(TerrainAuthoringCookErrors::Stale));
            const auto layerDigest = source.Value().layers ? source.Value().layers->digest : Sha256Digest{};
            if (layerDigest != request.fence.layerDigest)
                return Return::Failure(MakeError(TerrainAuthoringCookErrors::Stale));
            return source;
        }

        Result<void> BindingCaptures(const Capture &capture) {
            if (capture.source.layers) {
                const auto *layers = capture.inputs->Find(capture.source.layers->asset);
                if (!layers || layers->sourceDigest != capture.source.layers->digest)
                    return Result<void>::Failure(MakeError(TerrainAuthoringCookErrors::Stale));
            }
            for (const auto &reference : capture.source.foliage.definitions) {
                const auto *definition = capture.inputs->Find(reference.asset);
                if (!definition || definition->sourceDigest != reference.sourceDigest)
                    return Result<void>::Failure(MakeError(TerrainAuthoringCookErrors::Stale));
            }
            return Result<void>::Success();
        }

        Result<void> HistoricMetadata(const TerrainAuthoringCookRequest &request, const Assets::AssetRegistrySnapshot &registry,
                                      const CancellationToken &cancellation) {
            const auto *record = registry.Find(request.asset);
            if (!record)
                return Result<void>::Failure(MakeError(TerrainAuthoringCookErrors::Stale));
            auto metadata =
                Assets::CaptureAssetSource(request.sourceRoot, Foundation::Paths::Resolve(request.sourceRoot, record->metadataPath),
                                           256 * 1024, cancellation);
            if (metadata.HasError())
                return Result<void>::Failure(std::move(metadata).ErrorValue());
            if (!metadata.Value().identity.exists || metadata.Value().identity.digest != request.acceptedMetadataDigest)
                return Result<void>::Failure(MakeError(TerrainAuthoringCookErrors::Stale));
            return Result<void>::Success();
        }

        Result<void> CurrentRegistry(const Assets::AssetRegistrySnapshot &current, const Assets::AssetCookInputSnapshot &inputs) {
            for (const auto &source : inputs.Sources()) {
                const auto *record = current.Find(source.record.id);
                if (!record)
                    return Result<void>::Failure(MakeError(TerrainAuthoringCookErrors::Stale));
                if (record->type != source.record.type || record->sourcePath.String() != source.record.sourcePath.String() ||
                    record->metadataPath.String() != source.record.metadataPath.String())
                    return Result<void>::Failure(MakeError(TerrainAuthoringCookErrors::Stale));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CaptureSources */
    Result<Capture> CaptureSources(const TerrainAuthoringCookEnvironment &environment, const TerrainAuthoringCookRequest &request,
                                   const CancellationToken &cancellation) {
        using Return = Result<Capture>;
        const auto registry = environment.registry->Snapshot();
        if (auto metadata = HistoricMetadata(request, registry, cancellation); metadata.HasError())
            return Return::Failure(std::move(metadata).ErrorValue());
        auto source = ReadRoot(request, registry, cancellation);
        if (source.HasError())
            return Return::Failure(std::move(source).ErrorValue());
        IdSet ids{request.asset};
        if (auto layers = AddLayers(request, registry, source.Value(), ids, cancellation); layers.HasError())
            return Return::Failure(std::move(layers).ErrorValue());
        auto definitions = AddDefinitions(request, registry, source.Value(), ids, cancellation);
        if (definitions.HasError())
            return Return::Failure(std::move(definitions).ErrorValue());
        std::vector<Assets::AssetId> selected(ids.begin(), ids.end());
        auto inputs = Assets::AssetCookInputSnapshot::CaptureSelected(request.sourceRoot, registry, selected, request.profile.assets,
                                                                      request.profile.maximumCapturedBytes, cancellation);
        if (inputs.HasError())
            return Return::Failure(std::move(inputs).ErrorValue());
        const auto *root = inputs.Value().Find(request.asset);
        if (!root || root->sourceDigest != request.fence.sourceDigest)
            return Return::Failure(MakeError(TerrainAuthoringCookErrors::Stale));
        selected.erase(std::ranges::find(selected, request.asset));
        Capture capture{std::make_shared<const Assets::AssetCookInputSnapshot>(std::move(inputs).Value()), std::move(source).Value(),
                        std::move(definitions).Value(), std::move(selected)};
        if (auto bindings = BindingCaptures(capture); bindings.HasError())
            return Return::Failure(std::move(bindings).ErrorValue());
        if (auto current = VerifySources(environment, request, capture, cancellation); current.HasError())
            return Return::Failure(std::move(current).ErrorValue());
        return Return::Success(std::move(capture));
    }

    /** @copydoc VerifySources */
    Result<void> VerifySources(const TerrainAuthoringCookEnvironment &environment, const TerrainAuthoringCookRequest &request,
                               const Capture &capture, const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(TerrainAuthoringCookErrors::Cancelled));
        if (auto current = CurrentRegistry(environment.registry->Snapshot(), *capture.inputs); current.HasError())
            return current;
        if (auto metadata = HistoricMetadata(request, capture.inputs->Registry(), cancellation); metadata.HasError())
            return metadata;
        return capture.inputs->VerifyUnchanged(cancellation);
    }
}  // namespace Horo::Application::TerrainCookDetail
