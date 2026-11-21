#include "EnvironmentQueryWorkspaceSource.h"
#include "Horo/AI/EnvironmentQueryGenerators.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"
#include "editor/document/AuthoringSourceMerge.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <limits>
#include <tuple>

namespace Horo::Editor {
    namespace {
        /** @brief Compares the complete authored stage, including unavailable contribution data. */
        bool SameStage(const AI::QueryAssetStage &left, const AI::QueryAssetStage &right) {
            const auto fields = [](const AI::QueryAssetStage &stage) {
                return std::tie(stage.kind, stage.executionOrder, stage.displayName, stage.generator, stage.test, stage.descriptorVersion,
                                stage.properties, stage.unknownTypeId, stage.opaquePayload);
            };
            return fields(left) == fields(right);
        }

        /** @brief Merges independent result, display and stage edits without changing their typed identities. */
        Result<AI::EnvironmentQueryAsset> MergeQueries(const AI::EnvironmentQueryAsset &base, const AI::EnvironmentQueryAsset &local,
                                                       const AI::EnvironmentQueryAsset &disk) {
            const auto label = SelectAuthoringMerge(base.DisplayName(), local.DisplayName(), disk.DisplayName());
            const auto schema = SelectAuthoringMerge(base.ResultSchema(), local.ResultSchema(), disk.ResultSchema());
            const auto stages = MergeAuthoringRecords(base.Stages(), local.Stages(), disk.Stages(), &AI::QueryAssetStage::id, SameStage);
            if (!label || !schema || !stages)
                return Result<AI::EnvironmentQueryAsset>::Failure(
                    MakeError(Assets::AssetSourceErrors::Conflict, "Query changes overlap in the display, result contract or stage."));
            AI::QueryAssetSource merged{.id = base.Id(), .displayName = label->get(), .result = schema->get()};
            merged.stages.reserve(stages->size());
            for (const auto &entry : *stages)
                merged.stages.push_back(*entry.second);
            return AI::EnvironmentQueryAsset::Capture(merged);
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::ResolveEnvironmentQueryExternal */
    Result<void> EditorWorkspaceController::ResolveEnvironmentQueryExternal(EnvironmentQueryWorkspaceDocument &document,
                                                                            EnvironmentQueryDraft &draft,
                                                                            const EnvironmentQueryEditRequest &request) {
        using namespace EnvironmentQueryWorkspaceSource;
        const auto conflict = [] {
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        };
        if (!document.dirty || !document.conflict || request.fence.revision != document.revision ||
            document.revision == std::numeric_limits<std::uint64_t>::max())
            return conflict();
        const auto root = ProjectRoot();
        const auto registry = m_mutableAssetRegistry ? m_mutableAssetRegistry->Snapshot() : m_assetRegistry;
        if (!RegisteredQueryDocument(document, registry))
            return conflict();
        const auto captured = CaptureQuerySource(root, registry, document.asset);
        if (captured.HasError())
            return Result<void>::Failure(captured.ErrorValue());
        const auto &source = captured.Value().Sources().front();
        const auto sidecar = CaptureQuerySidecar(root, source.record);
        if (sidecar.HasError() || !sidecar.Value().exists)
            return conflict();
        auto external = AI::EnvironmentQuerySourceCodec::Decode(source.bytes);
        if (external.HasError())
            return Result<void>::Failure(external.ErrorValue());
        if (external.Value().Id() != draft.savedSource->Id() || external.Value().ResultSchema().id != draft.savedSource->ResultSchema().id)
            return conflict();

        const auto stage = [&](AI::EnvironmentQueryAsset asset) -> Result<EnvironmentQueryState> {
            auto settings = AI::ReadBuiltinPointQuerySettings(asset, *m_querySchema);
            if (settings.HasError())
                return Result<EnvironmentQueryState>::Failure(settings.ErrorValue());
            const auto bytes = AI::EnvironmentQuerySourceCodec::Encode(asset);
            if (bytes.HasError())
                return Result<EnvironmentQueryState>::Failure(bytes.ErrorValue());
            return Result<EnvironmentQueryState>::Success({std::make_shared<const AI::EnvironmentQueryAsset>(std::move(asset)),
                                                           std::move(settings).Value(),
                                                           ComputeSha256(std::as_bytes(std::span{bytes.Value()})), bytes.Value().size()});
        };
        auto baseline = stage(std::move(external).Value());
        if (baseline.HasError())
            return Result<void>::Failure(baseline.ErrorValue());
        auto next = draft.current;
        if (request.action == EnvironmentQueryEditAction::MergeExternal) {
            auto merged = MergeQueries(*draft.savedSource, *draft.current.source, *baseline.Value().source);
            if (merged.HasError())
                return Result<void>::Failure(merged.ErrorValue());
            auto staged = stage(std::move(merged).Value());
            if (staged.HasError())
                return Result<void>::Failure(staged.ErrorValue());
            next = std::move(staged).Value();
        }

        const auto latest = CaptureQuerySidecar(root, source.record);
        if (captured.Value().VerifyUnchanged().HasError() || latest.HasError() || latest.Value() != sidecar.Value())
            return conflict();
        draft.savedSource = baseline.Value().source;
        draft.saved = baseline.Value().digest;
        draft.current = std::move(next);
        draft.history.Clear();
        draft.acceptedBytes = source.bytes.size();
        draft.sidecar = sidecar.Value().digest;
        document.source = source.sourceDigest;
        document.conflict = false;
        ++document.revision;
        ProjectEnvironmentQueryDraft(document, draft);
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
