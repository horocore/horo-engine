#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiStyleWorkspaceInternal.h"

#include <algorithm>

namespace Horo::Application::UiStyleWorkspaceInternal {
    namespace {
        using namespace Runtime::Ui;

        /** @brief Provider bytes must match the exact manifest commitment and caller's captured identity/type/target. */
        Result<Assets::AssetCookArtifact> VerifyLoaded(const Assets::AssetLoadResult &loaded, const Assets::AssetCookManifestEntry &entry,
                                                       const AssetCookTargetId &target, const Assets::AssetRegistryRevision revision) {
            if (loaded.id != entry.assetId || loaded.sourceRegistryRevision != revision ||
                ComputeSha256(std::as_bytes(std::span{loaded.bytes})) != entry.artifactHash)
                return Result<Assets::AssetCookArtifact>::Failure(MakeError(UiErrors::StyleSourceStale));
            auto artifact = Assets::DecodeCookedArtifact(loaded.bytes);
            if (artifact.HasError())
                return artifact;
            if (artifact.Value().id != entry.assetId || artifact.Value().type != entry.assetType || artifact.Value().target != target)
                return Result<Assets::AssetCookArtifact>::Failure(MakeError(UiErrors::AssetTypeMismatch));
            return artifact;
        }

        /** @brief Every namespace payload must equal the semantic source captured for this operation, including revision/defaults. */
        Result<UiStyleAssetDocument> DecodeStyle(const Assets::AssetCookArtifact &artifact, const Capture &capture,
                                                 const UiStyleSchema &schema) {
            auto decoded = DecodeCookedUiStyleAsset(artifact.payload, artifact.id, schema);
            if (decoded.HasError())
                return decoded;
            const auto original = std::ranges::find_if(capture.documents, [&](const auto &document) {
                return document.style.id.Bytes() == artifact.id.Bytes();
            });
            if (original == capture.documents.end())
                return Result<UiStyleAssetDocument>::Failure(MakeError(UiErrors::StyleIdentityConflict));
            const auto canonical = CookUiStyleAsset(*original, schema);
            if (canonical.HasError())
                return Result<UiStyleAssetDocument>::Failure(canonical.ErrorValue());
            if (canonical.Value() != artifact.payload)
                return Result<UiStyleAssetDocument>::Failure(MakeError(UiErrors::StyleSourceStale));
            return decoded;
        }

        /** @brief Independently owned provider envelopes and decoded artifact values. */
        struct Loaded final {
            std::vector<Assets::AssetLoadResult> bytes;
            std::vector<Assets::AssetCookArtifact> artifacts;
        };

        /** @brief Verify the complete manifest, then load every exact artifact through the actual bounded provider service. */
        Result<Loaded> LoadInventory(JobSystem &jobs, const UiStyleWorkspaceConfig &config, const Capture &capture,
                                     const Assets::AssetCookGeneration &generation, const CancellationToken &cancellation) {
            auto inventory = Assets::ReadCookGenerationContents(*config.cook.publicationFiles, generation, config.maximumCapturedBytes,
                                                                config.cook.limits);
            if (inventory.HasError())
                return Result<Loaded>::Failure(inventory.ErrorValue());
            if (inventory.Value().entries.size() != capture.selected.Records().size())
                return Result<Loaded>::Failure(MakeError(UiErrors::StyleSourceStale));
            auto contents = std::move(inventory).Value();
            std::vector<std::vector<std::uint8_t>>{}.swap(contents.artifacts);
            Assets::FilesystemAssetProvider provider{generation.generationRoot, {config.cook.limits.maximumArtifactBytes}};
            Assets::AssetLoadService loads{jobs, provider, 1};
            Loaded loaded;
            for (const auto &entry : contents.entries) {
                const auto *record = capture.selected.Find(entry.assetId);
                if (!record || record->type != entry.assetType)
                    return Result<Loaded>::Failure(MakeError(UiErrors::AssetTypeMismatch));
                auto started = loads.LoadAsync(capture.inputs->Registry(), entry.assetId, cancellation);
                if (started.HasError())
                    return Result<Loaded>::Failure(started.ErrorValue());
                auto handle = std::move(started).Value();
                if (const auto joined = handle.Wait(); joined.HasError())
                    return Result<Loaded>::Failure(joined.ErrorValue());
                auto bytes = handle.TakeResult();
                if (bytes.HasError())
                    return Result<Loaded>::Failure(bytes.ErrorValue());
                auto artifact = VerifyLoaded(bytes.Value(), entry, generation.target, capture.inputs->Registry().Revision());
                if (artifact.HasError())
                    return Result<Loaded>::Failure(artifact.ErrorValue());
                const auto *source = capture.inputs->Find(entry.assetId);
                if (!source || artifact.Value().sourceDigest != source->sourceDigest)
                    return Result<Loaded>::Failure(MakeError(UiErrors::StyleSourceStale));
                loaded.bytes.push_back(std::move(bytes).Value());
                loaded.artifacts.push_back(std::move(artifact).Value());
            }
            return Result<Loaded>::Success(std::move(loaded));
        }

        /** @brief Bind actual resource envelopes to the single selected-root schema, retaining no temporary span. */
        Result<UiStyleSchema> LoadedSchema(const Capture &capture, const Loaded &loaded) {
            std::vector<Assets::AssetCookCandidateArtifactView> resources;
            for (std::size_t i = 0; i < loaded.bytes.size(); ++i)
                if (std::ranges::find(capture.resources, loaded.bytes[i].id) != capture.resources.end())
                    resources.push_back(
                        {{loaded.bytes[i].id, loaded.artifacts[i].type, ComputeSha256(std::as_bytes(std::span{loaded.bytes[i].bytes}))},
                         loaded.bytes[i].bytes});
            return Schema(capture, resources);
        }

        /** @brief Revalidate every namespace against both its captured source and the exact loaded resource policy. */
        Result<std::vector<UiStyleAssetDocument>> LoadedNamespaces(const Capture &capture, const Loaded &loaded,
                                                                   const UiStyleSchema &schema) {
            std::vector<UiStyleAssetDocument> documents;
            for (const auto &artifact : loaded.artifacts)
                if (artifact.type.Value() == UiStyleAssetTypeName) {
                    auto decoded = DecodeStyle(artifact, capture, schema);
                    if (decoded.HasError())
                        return Result<std::vector<UiStyleAssetDocument>>::Failure(decoded.ErrorValue());
                    documents.push_back(std::move(decoded).Value());
                }
            return Result<std::vector<UiStyleAssetDocument>>::Success(std::move(documents));
        }
    }  // namespace

    /** @copydoc LoadCandidate */
    Result<UiStyleWorkspaceCandidate> LoadCandidate(JobSystem &jobs, const UiStyleWorkspaceConfig &config, const Capture &capture,
                                                    const Assets::AssetCookGeneration &generation, const Assets::AssetId workspace,
                                                    const std::uint64_t request, const RuntimeStyleAssetId theme,
                                                    const RuntimeStyleGeneration styleGeneration, const CancellationToken &cancellation) {
        using R = Result<UiStyleWorkspaceCandidate>;
        auto inventory = LoadInventory(jobs, config, capture, generation, cancellation);
        if (inventory.HasError())
            return R::Failure(inventory.ErrorValue());
        auto loaded = std::move(inventory).Value();
        auto schema = LoadedSchema(capture, loaded);
        if (schema.HasError())
            return R::Failure(schema.ErrorValue());
        auto documents = LoadedNamespaces(capture, loaded, schema.Value());
        if (documents.HasError())
            return R::Failure(documents.ErrorValue());
        auto registry = PrepareUiStyleRegistry(documents.Value(), schema.Value(), styleGeneration);
        if (registry.HasError())
            return R::Failure(registry.ErrorValue());
        if (!registry.Value().HasAsset(theme))
            return R::Failure(MakeError(UiErrors::StyleReferenceInvalid));
        if (const auto unchanged = capture.inputs->VerifyUnchanged(cancellation); unchanged.HasError())
            return R::Failure(unchanged.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return R::Failure(MakeError(UiErrors::AssetLoadCancelled));
        return R::Success({workspace,
                           request,
                           capture.inputs->Registry().Revision(),
                           generation,
                           std::move(schema).Value(),
                           std::make_shared<const RuntimeStyleRegistry>(std::move(registry).Value()),
                           theme,
                           std::move(loaded.bytes),
                           {}});
    }
}  // namespace Horo::Application::UiStyleWorkspaceInternal
