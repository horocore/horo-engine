#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiStyleWorkspaceInternal.h"

#include <algorithm>

namespace Horo::Application::UiStyleWorkspaceInternal {
    namespace {
        using namespace Runtime::Ui;

        struct Sources final {
            std::vector<Assets::AssetId> ids;
            std::vector<UiStyleAssetDocument> documents;
            std::vector<std::pair<Assets::AssetId, Sha256Digest>> digests;
        };

        /** @brief Read only the explicitly selected dependency graph; an unrelated malformed asset is never admitted. */
        Result<Sources> ReadClosure(const UiStyleWorkspaceConfig &config, const Assets::AssetRegistrySnapshot &assets,
                                    const Assets::AssetId root, const CancellationToken &cancellation) {
            Sources sources{{root}, {}, {}};
            std::uint64_t remaining = config.maximumCapturedBytes;
            for (std::size_t i = 0; i < sources.ids.size(); ++i) {
                if (sources.ids.size() > MaximumUiStyleTokens || sources.documents.size() > MaximumUiStyleAssets)
                    return Result<Sources>::Failure(MakeError(UiErrors::CapacityExceeded));
                const auto *record = assets.Find(sources.ids[i]);
                if (!record)
                    return Result<Sources>::Failure(MakeError(UiErrors::AssetMissing));
                if (record->type.Value() != UiStyleAssetTypeName)
                    continue;
                if (sources.documents.size() == MaximumUiStyleAssets)
                    return Result<Sources>::Failure(MakeError(UiErrors::CapacityExceeded));
                auto input = Assets::AssetCookInputSnapshot::CaptureSelected(config.cook.sourceRoot, assets, std::span{&record->id, 1},
                                                                             config.cook.limits, config.maximumCapturedBytes, cancellation);
                if (input.HasError())
                    return Result<Sources>::Failure(input.ErrorValue());
                const auto &source = input.Value().Sources().front();
                if (source.bytes.size() > remaining)
                    return Result<Sources>::Failure(MakeError(UiErrors::CapacityExceeded));
                remaining -= source.bytes.size();
                auto decoded = DeserializeUiStyleAsset({reinterpret_cast<const char *>(source.bytes.data()), source.bytes.size()});
                if (decoded.HasError())
                    return Result<Sources>::Failure(decoded.ErrorValue());
                if (decoded.Value().style.id.Bytes() != record->id.Bytes())
                    return Result<Sources>::Failure(MakeError(UiErrors::StyleIdentityConflict));
                for (const auto &dependency : decoded.Value().dependencies) {
                    const auto *required = assets.Find(dependency.asset);
                    if (!required) {
                        if (dependency.required)
                            return Result<Sources>::Failure(MakeError(UiErrors::AssetMissing));
                        continue;
                    }
                    if (required->type != dependency.expectedType)
                        return Result<Sources>::Failure(MakeError(UiErrors::AssetTypeMismatch));
                    if (std::ranges::find(sources.ids, required->id) == sources.ids.end())
                        sources.ids.push_back(required->id);
                }
                sources.digests.emplace_back(record->id, source.sourceDigest);
                sources.documents.push_back(std::move(decoded).Value());
            }
            std::ranges::sort(sources.ids);
            return Result<Sources>::Success(std::move(sources));
        }

    }  // namespace

    /** @copydoc CaptureSources */
    Result<Capture> CaptureSources(const UiStyleWorkspaceConfig &config, const Assets::AssetRegistrySnapshot &assets,
                                   const RuntimeStyleAssetId root, const CancellationToken &cancellation) {
        const auto *selected = assets.Find(Assets::AssetId::FromBytes(root.Bytes()));
        if (!selected || selected->type.Value() != UiStyleAssetTypeName)
            return Result<Capture>::Failure(MakeError(UiErrors::StyleReferenceInvalid));
        auto sources = ReadClosure(config, assets, selected->id, cancellation);
        if (sources.HasError())
            return Result<Capture>::Failure(sources.ErrorValue());
        auto complete = Assets::AssetCookInputSnapshot::CaptureSelected(config.cook.sourceRoot, assets, sources.Value().ids,
                                                                        config.cook.limits, config.maximumCapturedBytes, cancellation);
        if (complete.HasError())
            return Result<Capture>::Failure(complete.ErrorValue());
        for (const auto &[id, digest] : sources.Value().digests) {
            const auto *input = complete.Value().Find(id);
            if (!input || input->sourceDigest != digest)
                return Result<Capture>::Failure(MakeError(UiErrors::StyleSourceStale));
        }
        Capture capture;
        std::vector<Assets::AssetRecord> records;
        capture.inputs = std::make_shared<const Assets::AssetCookInputSnapshot>(std::move(complete).Value());
        for (const auto id : sources.Value().ids) {
            records.push_back(*assets.Find(id));
            if (assets.Find(id)->type.Value() != UiStyleAssetTypeName)
                capture.resources.push_back(id);
        }
        capture.documents = std::move(sources).Value().documents;
        const auto rootDocument = std::ranges::find_if(capture.documents, [&](const auto &document) {
            return document.style.id == root;
        });
        if (rootDocument == capture.documents.end())
            return Result<Capture>::Failure(MakeError(UiErrors::StyleReferenceInvalid));
        capture.defaults = rootDocument->defaults;
        Assets::AssetRegistry selectedRegistry;
        if (selectedRegistry.Publish(std::move(records)).status != Assets::AssetRegistryBuildStatus::Complete)
            return Result<Capture>::Failure(MakeError(UiErrors::DependencyInvalid));
        capture.selected = selectedRegistry.Snapshot();
        return Result<Capture>::Success(std::move(capture));
    }

    /** @copydoc Schema */
    Result<UiStyleSchema> Schema(const Capture &capture, const std::span<const Assets::AssetCookCandidateArtifactView> resources) {
        if (resources.size() != capture.resources.size())
            return Result<UiStyleSchema>::Failure(MakeError(UiErrors::AssetMissing));
        std::vector<UiStyleResourceEvidence> evidence;
        for (const auto &resource : resources) {
            const auto *record = capture.selected.Find(resource.identity.id);
            if (!record || record->type != resource.identity.type ||
                std::ranges::find(capture.resources, record->id) == capture.resources.end())
                return Result<UiStyleSchema>::Failure(MakeError(UiErrors::AssetTypeMismatch));
            const auto artifact = Assets::DecodeCookedArtifact(resource.envelope);
            if (artifact.HasError())
                return Result<UiStyleSchema>::Failure(artifact.ErrorValue());
            if (artifact.Value().id != record->id || artifact.Value().type != record->type)
                return Result<UiStyleSchema>::Failure(MakeError(UiErrors::AssetTypeMismatch));
            evidence.push_back({{record->id, record->type, true}, ComputeSha256(std::as_bytes(resource.envelope))});
        }
        return CreateCoreUiStyleSchema(capture.defaults, evidence);
    }
}  // namespace Horo::Application::UiStyleWorkspaceInternal
