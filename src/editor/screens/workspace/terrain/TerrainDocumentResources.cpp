#include "Horo/Foundation/PathUtils.h"
#include "TerrainDocumentHost.h"
#include "TerrainDocumentHostInternal.h"

#include <algorithm>
#include <new>
#include <set>

namespace Horo::Editor {
    namespace {
        constexpr std::size_t MaximumCatalogueRecords = 65'536;
        constexpr std::size_t MaximumTypeChoices = 256;
        constexpr std::size_t MaximumLayerChoices = 64;

        /** @brief Resolve a real tracked source and validate its current identity sidecar under a caller-held project lease. */
        Result<Assets::AssetSourceCapture> CaptureTracked(const std::filesystem::path &root, const Assets::AssetRegistrySnapshot &registry,
                                                          Assets::AssetId asset, std::string_view type, std::size_t maximum) {
            using Return = Result<Assets::AssetSourceCapture>;
            const auto *record = registry.Find(asset);
            if (!record || record->type.Value() != type)
                return Return::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
            auto metadata = Assets::CaptureAssetSource(root, Foundation::Paths::Resolve(root, record->metadataPath), 256 * 1024);
            if (metadata.HasError())
                return Return::Failure(std::move(metadata).ErrorValue());
            const auto &bytes = metadata.Value().bytes;
            auto decoded = Assets::DecodeAssetIdentitySidecar(record->sourcePath.String(),
                                                              std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
            if (decoded.HasError())
                return Return::Failure(std::move(decoded).ErrorValue());
            if (decoded.Value().id != asset || decoded.Value().type != record->type)
                return Return::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            auto source = Assets::CaptureAssetSource(root, Foundation::Paths::Resolve(root, record->sourcePath), maximum);
            if (source.HasError())
                return source;
            if (!source.Value().identity.exists)
                return Return::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
            return source;
        }

        bool RegisteredType(const Assets::AssetRegistrySnapshot &registry, Assets::AssetId asset, std::string_view type) noexcept {
            const auto *record = registry.Find(asset);
            return record && record->type.Value() == type;
        }

        /** @brief Layer-set semantics reference actual registered material assets, never generated stand-ins. */
        Result<void> MaterialReferences(const Terrain::TerrainMaterialLayerSetData &layers, const Assets::AssetRegistrySnapshot &registry) {
            for (std::size_t i = 0; i < layers.layerCount; ++i)
                if (!RegisteredType(registry, Assets::AssetId::FromBytes(layers.layers[i].material.Bytes()), "core.material"))
                    return Result<void>::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
            return Result<void>::Success();
        }

        Result<TerrainLayerSetChoice> LayerChoice(const std::filesystem::path &root, const Assets::AssetRegistrySnapshot &registry,
                                                  const Assets::AssetRecord &record) {
            using Return = Result<TerrainLayerSetChoice>;
            auto source = CaptureTracked(root, registry, record.id, "core.terrain.layerset", 4096);
            if (source.HasError())
                return Return::Failure(std::move(source).ErrorValue());
            auto decoded = Terrain::DecodeTerrainAuthoringLayerSet(source.Value().bytes, record.id, TerrainHostConfiguration());
            if (decoded.HasError())
                return Return::Failure(std::move(decoded).ErrorValue());
            if (auto references = MaterialReferences(decoded.Value().layers, registry); references.HasError())
                return Return::Failure(std::move(references).ErrorValue());
            const auto &layers = decoded.Value().layers;
            TerrainLayerSetChoice choice{{record.id, layers.id, layers.revision, source.Value().identity.digest, {}},
                                         std::string(record.sourcePath.String())};
            for (std::size_t i = 0; i < layers.layerCount; ++i)
                choice.binding.order[i] = layers.layers[i].id;
            return Return::Success(std::move(choice));
        }

        Result<void> FoliageReferences(const Terrain::FoliageTypeDefinitionData &definition,
                                       const Assets::AssetRegistrySnapshot &registry) {
            if (!RegisteredType(registry, Assets::AssetId::FromBytes(definition.assets.material.Bytes()), "core.material"))
                return Result<void>::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
            for (std::size_t i = 0; i < definition.assets.meshLodCount; ++i)
                if (!RegisteredType(registry, Assets::AssetId::FromBytes(definition.assets.meshLods[i].Bytes()), "core.mesh"))
                    return Result<void>::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
            return Result<void>::Success();
        }

        Result<FoliagePaletteItem> TypeChoice(const std::filesystem::path &root, const Assets::AssetRegistrySnapshot &registry,
                                              const Assets::AssetRecord &record) {
            using Return = Result<FoliagePaletteItem>;
            auto source = CaptureTracked(root, registry, record.id, "core.foliage.type", 1024);
            if (source.HasError())
                return Return::Failure(std::move(source).ErrorValue());
            const Terrain::FoliageDefinitionCapabilitySet capabilities{
                Terrain::FoliageDefinitionCapabilityBit<Terrain::FoliageDefinitionCapability::CpuCulling>};
            const auto configuration = TerrainHostConfiguration();
            auto decoded = Terrain::DecodeFoliageTypeSource(source.Value().bytes, record.id, configuration, capabilities);
            if (decoded.HasError())
                return Return::Failure(std::move(decoded).ErrorValue());
            if (auto references = FoliageReferences(decoded.Value().definition, registry); references.HasError())
                return Return::Failure(std::move(references).ErrorValue());
            auto definition = Terrain::FoliageTypeDefinition::Create(decoded.Value().definition, configuration, capabilities);
            if (definition.HasError())
                return Return::Failure(std::move(definition).ErrorValue());
            const auto &data = decoded.Value().definition;
            return Return::Success({{record.id, data.type, data.revision, source.Value().identity.digest},
                                    std::move(definition).Value(),
                                    std::string(record.sourcePath.String())});
        }

        struct ResourceChoices final {
            std::vector<TerrainLayerSetChoice> layers;
            std::vector<FoliagePaletteItem> foliage;
        };

        /** @brief Semantic identities must name one actual source; ambiguous catalogues cannot choose first-match meaning. */
        Result<void> UniqueChoices(const ResourceChoices &choices) {
            std::set<Terrain::FoliageTypeId> types;
            for (const auto &item : choices.foliage)
                if (!types.insert(item.source.type).second)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            std::set<Terrain::TerrainLayerSetId> layers;
            for (const auto &item : choices.layers)
                if (!layers.insert(item.binding.id).second)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            return Result<void>::Success();
        }

        /** @brief Catalogue work is finite and explicit; invalid unrelated resource assets fail this refresh rather than disappear. */
        Result<ResourceChoices> ReadChoices(const std::filesystem::path &root, const Assets::AssetRegistrySnapshot &registry) {
            using Return = Result<ResourceChoices>;
            if (registry.Records().size() > MaximumCatalogueRecords)
                return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
            ResourceChoices choices;
            for (const auto &record : registry.Records()) {
                if (record.type.Value() == "core.terrain.layerset") {
                    if (choices.layers.size() == MaximumLayerChoices)
                        return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
                    auto value = LayerChoice(root, registry, record);
                    if (value.HasError())
                        return Return::Failure(std::move(value).ErrorValue());
                    choices.layers.push_back(std::move(value).Value());
                } else if (record.type.Value() == "core.foliage.type") {
                    if (choices.foliage.size() == MaximumTypeChoices)
                        return Return::Failure(MakeError(TerrainEditErrors::LimitExceeded));
                    auto value = TypeChoice(root, registry, record);
                    if (value.HasError())
                        return Return::Failure(std::move(value).ErrorValue());
                    choices.foliage.push_back(std::move(value).Value());
                }
            }
            if (auto unique = UniqueChoices(choices); unique.HasError())
                return Return::Failure(std::move(unique).ErrorValue());
            return Return::Success(std::move(choices));
        }

        Result<void> RequiredDefinitions(std::span<const Terrain::FoliageAuthoringDefinitionReference> required,
                                         const std::vector<FoliagePaletteItem> &choices) {
            for (const auto &reference : required) {
                const auto match = std::ranges::find_if(choices, [&](const auto &item) {
                    return item.source == reference;
                });
                if (match == choices.end())
                    return Result<void>::Failure(MakeError(TerrainEditErrors::StaleRevision));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc TerrainDocumentHost::LoadResources */
    Result<void> TerrainDocumentHost::LoadResources(std::span<const Terrain::FoliageAuthoringDefinitionReference> required) {
        auto choices = ReadChoices(root_, registry_);
        if (choices.HasError())
            return Result<void>::Failure(std::move(choices).ErrorValue());
        if (auto checked = RequiredDefinitions(required, choices.Value().foliage); checked.HasError())
            return checked;
        if (layers_ && std::ranges::none_of(choices.Value().layers, [&](const auto &item) {
            return item.binding == *layers_;
        }))
            return Result<void>::Failure(MakeError(TerrainEditErrors::StaleRevision));
        auto admitted = std::move(choices).Value();
        layerSets_ = std::move(admitted.layers);
        foliageCatalog_ = std::move(admitted.foliage);
        return Result<void>::Success();
    }

    /** @copydoc TerrainDocumentHost::BorrowDocument */
    const TerrainAuthoringDocument *TerrainDocumentHost::BorrowDocument() const noexcept {
        return BorrowSource() ? &document_ : nullptr;
    }

    /** @copydoc TerrainDocumentHost::BorrowLayerBinding */
    const Terrain::TerrainAuthoringLayerBinding *TerrainDocumentHost::BorrowLayerBinding() const noexcept {
        return BorrowSource() && layers_ ? &*layers_ : nullptr;
    }

    /** @copydoc TerrainDocumentHost::BorrowFoliageCatalog */
    std::span<const FoliagePaletteItem> TerrainDocumentHost::BorrowFoliageCatalog() const noexcept {
        return BorrowSource() ? std::span<const FoliagePaletteItem>(foliageCatalog_) : std::span<const FoliagePaletteItem>{};
    }

    /** @copydoc TerrainDocumentHost::BorrowLayerSets */
    std::span<const TerrainLayerSetChoice> TerrainDocumentHost::BorrowLayerSets() const noexcept {
        return BorrowSource() ? std::span<const TerrainLayerSetChoice>(layerSets_) : std::span<const TerrainLayerSetChoice>{};
    }

    /** @copydoc TerrainDocumentHost::RefreshResources */
    Result<void> TerrainDocumentHost::RefreshResources(const Assets::AssetRegistrySnapshot &registry,
                                                       const ProjectMutationCoordinator &mutations) {
        if (auto admitted = Admit(document_.Fence()); admitted.HasError())
            return admitted;
        const TerrainHostBusy busy(busy_);
        try {
            auto lease = mutations.TryAcquire({root_, ProjectMutationOwner::Save, "terrain.resources.refresh"});
            if (lease.HasError())
                return Result<void>::Failure(std::move(lease).ErrorValue());
            auto current = CaptureCurrent();
            if (current.HasError())
                return Result<void>::Failure(std::move(current).ErrorValue());
            auto choices = ReadChoices(root_, registry);
            if (choices.HasError())
                return Result<void>::Failure(std::move(choices).ErrorValue());
            if (auto required = RequiredDefinitions(current.Value().foliage.definitions, choices.Value().foliage); required.HasError())
                return required;
            if (layers_ && std::ranges::none_of(choices.Value().layers, [&](const auto &item) {
                return item.binding == *layers_;
            }))
                return Result<void>::Failure(MakeError(TerrainEditErrors::StaleRevision));
            auto admitted = std::move(choices).Value();
            registry_ = registry;
            layerSets_ = std::move(admitted.layers);
            foliageCatalog_ = std::move(admitted.foliage);
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(TerrainEditErrors::LimitExceeded));
        } catch (...) {
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        }
    }

    /** @copydoc TerrainDocumentHost::ValidateResources */
    Result<void> TerrainDocumentHost::ValidateResources() const {
        if (layers_) {
            if (auto valid = ValidateLayer(*layers_); valid.HasError())
                return valid;
        }
        std::set<Terrain::FoliageTypeId> used;
        for (const auto &[id, placement] : document_.Placements())
            used.insert(placement.type);
        for (const auto type : used) {
            const auto match = std::ranges::find_if(foliageCatalog_, [&](const auto &item) {
                return item.source.type == type;
            });
            if (match == foliageCatalog_.end())
                return Result<void>::Failure(MakeError(TerrainEditErrors::StaleRevision));
            if (auto valid = ValidateDefinition(match->source); valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }

    /** @copydoc TerrainDocumentHost::ValidateLayer */
    Result<void> TerrainDocumentHost::ValidateLayer(const Terrain::TerrainAuthoringLayerBinding &binding) const {
        if (const auto match = std::ranges::find_if(layerSets_,
                                                    [&](const auto &item) {
            return item.binding == binding;
        });
            match == layerSets_.end())
            return Result<void>::Failure(MakeError(TerrainEditErrors::StaleRevision));
        auto source = CaptureTracked(root_, registry_, binding.asset, "core.terrain.layerset", 4096);
        if (source.HasError())
            return Result<void>::Failure(std::move(source).ErrorValue());
        if (source.Value().identity.digest != binding.digest)
            return Result<void>::Failure(MakeError(TerrainEditErrors::StaleRevision));
        return Result<void>::Success();
    }

    /** @copydoc TerrainDocumentHost::ValidateDefinition */
    Result<void> TerrainDocumentHost::ValidateDefinition(const Terrain::FoliageAuthoringDefinitionReference &definition) const {
        if (const auto match = std::ranges::find_if(foliageCatalog_,
                                                    [&](const auto &item) {
            return item.source == definition;
        });
            match == foliageCatalog_.end())
            return Result<void>::Failure(MakeError(TerrainEditErrors::StaleRevision));
        auto source = CaptureTracked(root_, registry_, definition.asset, "core.foliage.type", 1024);
        if (source.HasError())
            return Result<void>::Failure(std::move(source).ErrorValue());
        if (source.Value().identity.digest != definition.sourceDigest)
            return Result<void>::Failure(MakeError(TerrainEditErrors::StaleRevision));
        return Result<void>::Success();
    }

    /** @copydoc TerrainDocumentHost::CaptureCurrent */
    Result<Terrain::TerrainAuthoringSource> TerrainDocumentHost::CaptureCurrent() const {
        using Return = Result<Terrain::TerrainAuthoringSource>;
        Terrain::TerrainAuthoringSource value{document_.Source(), layers_, {}};
        std::set<Terrain::FoliageTypeId> used;
        value.foliage.placements.reserve(document_.Placements().size());
        for (const auto &[id, placement] : document_.Placements()) {
            value.foliage.placements.push_back(placement);
            used.insert(placement.type);
        }
        value.foliage.definitions.reserve(used.size());
        for (const auto type : used) {
            const auto match = std::ranges::find_if(foliageCatalog_, [&](const auto &item) {
                return item.source.type == type;
            });
            if (match == foliageCatalog_.end())
                return Return::Failure(MakeError(TerrainEditErrors::StaleRevision));
            value.foliage.definitions.push_back(match->source);
        }
        return Return::Success(std::move(value));
    }

    /** @copydoc TerrainDocumentHost::EncodeCurrent */
    Result<std::vector<std::byte>> TerrainDocumentHost::EncodeCurrent() const {
        if (!containerSource_)
            return EncodeTerrainHeights(document_.Source());
        auto source = CaptureCurrent();
        if (source.HasError())
            return Result<std::vector<std::byte>>::Failure(std::move(source).ErrorValue());
        return Terrain::EncodeTerrainAuthoringSource(source.Value());
    }
}  // namespace Horo::Editor
