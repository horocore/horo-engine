#include "Horo/PCG/PCGErrors.h"
#include "Horo/PCGScene/SceneBake.h"

#include <algorithm>
#include <limits>

namespace Horo::PCG {
    namespace {
        /** @brief Translate a persisted target-owned derivation into the original typed PCG provenance. */
        Result<PCGOwnedGeneratedOutput> ReadOwnership(const SceneSource::SceneGeneratedOwnership &v) {
            using Return = Result<PCGOwnedGeneratedOutput>;
            if (!SceneSource::IsValidSceneGeneratedOwnership(v))
                return Return::Failure(MakeError(PCGErrors::GenerationOwnershipMismatch));
            const auto &s = v.source;
            PCGGeneratedOutputProvenance p{GenerationLogicalOutputId::Create(v.output).Value(),
                                           {{{GraphId::Create(s.identity).Value(), GraphRevision::Create(s.revision).Value()},
                                             ExecutionValue::Create(s.execution).Value()},
                                            NodeId::Create(s.node).Value(),
                                            PinId::Create(s.pin).Value(),
                                            SourceSampleId::Create(s.sample).Value(),
                                            s.ordinal},
                                           GenerationLineageId::Create(v.lineage).Value(),
                                           GeneratedSetId::Create(v.set).Value(),
                                           v.revision,
                                           v.ownershipGeneration,
                                           GenerationTargetOwnerId::Create(v.owner).Value(),
                                           GenerationCellId::Create(v.scope).Value()};
            return Return::Success({p, v.content});
        }

        /** @brief Store exact copied provenance alongside canonical authored content without introducing a Scene-to-PCG dependency. */
        SceneSource::SceneGeneratedOwnership StoreOwnership(const Assets::AssetId asset, const PCGOutputDelta &delta) {
            const auto &p = delta.provenance;
            const auto &s = p.sourceOutput;
            return {{asset, s.execution.generation.graph.Value(), s.execution.generation.revision.Value(), s.execution.value.Value(),
                     s.node.Value(), s.pin.Value(), s.sample.Value(), s.ordinal},
                    p.targetOwner.Value(),
                    p.cell.Value(),
                    p.lineage.Value(),
                    p.set.Value(),
                    p.logicalOutput.Value(),
                    p.setRevision,
                    p.ownershipGeneration,
                    delta.desiredContent};
        }

        /** @brief Compare exact graph document/source/catalog opening, preserving unsaved graph state. */
        Result<void> ValidateGraph(const Editor::PCGGraphDocument &graph, const Editor::PCGDocumentSnapshot &captured,
                                   const PCGGenerationPlan &plan, const GenerationDependencyId graphSource) {
            const auto now = graph.Snapshot();
            if (now.HasError())
                return Result<void>::Failure(now.ErrorValue());
            const auto &v = now.Value();
            if (v.asset != captured.asset || v.session != captured.session || v.revision != captured.revision ||
                v.catalogRevision != captured.catalogRevision || v.state != captured.state || v.acceptedSource != captured.acceptedSource ||
                !captured.source || captured.source->Data().generation != plan.Execution().generation)
                return Result<void>::Failure(MakeError(PCGErrors::GenerationPlanStale));
            const auto dependency = CapturePCGSceneGraphInput(captured, graphSource);
            if (dependency.HasError())
                return Result<void>::Failure(dependency.ErrorValue());
            if (std::ranges::find(plan.Dependencies(), dependency.Value()) == plan.Dependencies().end())
                return Result<void>::Failure(MakeError(PCGErrors::GenerationPlanStale));
            return Result<void>::Success();
        }

        /** @brief Recognize the exact already-committed operation without granting new mutation authority. */
        Result<bool> AlreadyApplied(const PCGSceneOutputBatch &batch, const PCGSceneBakeTarget &view, const Assets::AssetId asset) {
            if (batch.Commands().empty())
                return Result<bool>::Success(false);
            for (const auto &command : batch.Commands()) {
                const auto current = std::ranges::find(view.authored.objects, command.object, &SceneSource::SceneObjectSnapshot::id);
                if (command.delta.kind == PCGOutputDeltaKind::Remove) {
                    if (current != view.authored.objects.end())
                        return Result<bool>::Success(false);
                    continue;
                }
                if (current == view.authored.objects.end() || current->generated != StoreOwnership(asset, command.delta))
                    return Result<bool>::Success(false);
                const auto content = FingerprintPCGSceneOutput(*current, 4 * 1024 * 1024);
                if (content.HasError())
                    return Result<bool>::Failure(content.ErrorValue());
                if (content.Value() != command.delta.desiredContent)
                    return Result<bool>::Success(false);
            }
            return Result<bool>::Success(true);
        }

        /** @brief Revalidate an existing immutable plan through its one canonical admission boundary. */
        Result<void> ValidatePlan(const PCGGenerationPlan &plan, const PCGSceneBakeTarget &target, const PCGSceneBakeContext &context) {
            PCGGenerationPlanCandidate candidate{.plan = plan.Id(),
                                                 .execution = plan.Execution(),
                                                 .seed = plan.Seed(),
                                                 .lineage = plan.Lineage(),
                                                 .set = plan.Set(),
                                                 .setRevision = plan.SetRevision(),
                                                 .cell = plan.Cell(),
                                                 .targetOwner = plan.Target().owner,
                                                 .requiredCapabilities = plan.RequiredCapabilities(),
                                                 .validation = plan.Target(),
                                                 .dependencies = {plan.Dependencies().begin(), plan.Dependencies().end()},
                                                 .outputs = {plan.Outputs().begin(), plan.Outputs().end()}};
            PCGGenerationPlanContext admission{context.target.cancellation.IsCancellationRequested()
                                                   ? PCGGenerationPlanAdmission::CancellationRequested
                                                   : PCGGenerationPlanAdmission::Accepting,
                                               target.receipt,
                                               target.generated,
                                               {}};
            const auto checked = CreatePCGGenerationPlan(std::move(candidate), admission);
            if (checked.HasError())
                return Result<void>::Failure(checked.ErrorValue());
            if (!plan.RequiredCapabilities().Contains(PCGCapability::OfflineBake))
                return Result<void>::Failure(MakeError(PCGErrors::UnsupportedCapability));
            return Result<void>::Success();
        }

        /** @brief Require fresh exact asset evidence for every mesh retained by a desired command. */
        Result<void> ValidateAssets(const PCGSceneOutputBatch &batch, const PCGSceneBakeContext &context) {
            for (const auto &command : batch.Commands()) {
                if (!command.mesh)
                    continue;
                const auto &expected = *command.mesh;
                if (std::ranges::none_of(context.currentMeshAssets, [&expected](const auto &v) {
                    return v.asset == expected.asset && v.dependency == expected.dependency;
                }))
                    return Result<void>::Failure(MakeError(PCGErrors::GenerationPlanStale));
            }
            return Result<void>::Success();
        }

        /** @brief Prepare complete target-owned replacements after graph/plan/asset admission. */
        Result<std::vector<Editor::SceneObjectReplacement>> CaptureChanges(const PCGSceneOutputBatch &batch, const PCGSceneBakeTarget &view,
                                                                           const Assets::AssetId graphAsset) {
            std::vector<Editor::SceneObjectReplacement> changes;
            changes.reserve(batch.Commands().size());
            for (const auto &command : batch.Commands()) {
                Editor::SceneObjectReplacement change{command.object,
                                                      command.delta.priorContent,
                                                      {},
                                                      command.desired,
                                                      {command.delta.resources.residentBytes, command.delta.resources.preparationBytes,
                                                       command.delta.resources.retirementBytes}};
                const auto before = std::ranges::find(view.authored.objects, command.object, &SceneSource::SceneObjectSnapshot::id);
                if (command.delta.kind != PCGOutputDeltaKind::Create) {
                    if (before == view.authored.objects.end() || !before->generated || before->generated->source.asset != graphAsset)
                        return Result<std::vector<Editor::SceneObjectReplacement>>::Failure(
                            MakeError(PCGErrors::GenerationOwnershipMismatch));
                    change.expectedOwnership = before->generated;
                }
                if (change.desired)
                    change.desired->generated = StoreOwnership(graphAsset, command.delta);
                changes.push_back(std::move(change));
            }
            return Result<std::vector<Editor::SceneObjectReplacement>>::Success(std::move(changes));
        }
    }  // namespace

    /** @copydoc CapturePCGSceneGraphInput */
    Result<PCGGenerationDependency> CapturePCGSceneGraphInput(const Editor::PCGDocumentSnapshot &graph,
                                                              const GenerationDependencyId dependency) {
        if (!dependency.IsValid() || !graph.source || graph.state == Sha256Digest{} ||
            graph.revision == std::numeric_limits<std::uint64_t>::max())
            return Result<PCGGenerationDependency>::Failure(MakeError(PCGErrors::GenerationPlanInvalid));
        return Result<PCGGenerationDependency>::Success({dependency, graph.revision + 1, graph.state});
    }

    /** @copydoc CapturePCGSceneBakeTarget */
    Result<PCGSceneBakeTarget> CapturePCGSceneBakeTarget(const Editor::SceneDocumentTransactions &target,
                                                         const PCGSceneBakeBinding &binding, const Editor::SceneTransactionLimits limits) {
        using Return = Result<PCGSceneBakeTarget>;
        auto snapshot = target.Snapshot(limits);
        if (snapshot.HasError())
            return Return::Failure(snapshot.ErrorValue());
        if (!binding.owner.IsValid() || !binding.graphSource.IsValid() || !binding.scope.lineage.IsValid() ||
            !binding.scope.set.IsValid() || !binding.scope.cell.IsValid() ||
            binding.capabilityGeneration != snapshot.Value().session.generation ||
            !binding.capabilities.Contains(PCGCapability::SceneOutput) || !binding.capabilities.Contains(PCGCapability::OfflineBake) ||
            snapshot.Value().revision == std::numeric_limits<std::uint64_t>::max())
            return Return::Failure(MakeError(PCGErrors::UnsupportedCapability));
        const auto revision = snapshot.Value().revision + 1;
        PCGSceneBakeTarget result{std::move(snapshot).Value(),
                                  {GenerationValidationReceiptId::Create(revision).Value(), binding.owner, revision,
                                   binding.capabilityGeneration, binding.capabilities},
                                  {},
                                  {},
                                  {}};
        for (const auto &object : result.authored.objects) {
            result.occupied.push_back(object.id);
            if (!object.generated)
                continue;
            const auto owned = ReadOwnership(*object.generated);
            if (owned.HasError())
                return Return::Failure(owned.ErrorValue());
            if (owned.Value().provenance.targetOwner != binding.owner || owned.Value().provenance.lineage != binding.scope.lineage ||
                owned.Value().provenance.set != binding.scope.set || owned.Value().provenance.cell != binding.scope.cell)
                continue;
            result.generated.push_back(owned.Value());
            result.mappings.push_back({owned.Value().provenance.logicalOutput, object.id});
        }
        std::ranges::sort(result.occupied, {}, &SceneSource::SceneObjectId::value);
        std::ranges::sort(result.generated, {}, [](const auto &v) {
            return v.provenance.logicalOutput.Value();
        });
        std::ranges::sort(result.mappings, {}, [](const auto &v) {
            return v.logical.Value();
        });
        return Return::Success(std::move(result));
    }

    /** @copydoc BakePCGSceneOutputs */
    Result<PCGSceneBakeResult> BakePCGSceneOutputs(const Editor::PCGGraphDocument &graph, const Editor::PCGDocumentSnapshot &capturedGraph,
                                                   Editor::SceneDocumentTransactions &target, const PCGSceneOutputBatch &batch,
                                                   const PCGSceneBakeContext &context) {
        using Return = Result<PCGSceneBakeResult>;
        if (!context.limits.maximumChanges || batch.Commands().size() > context.limits.maximumChanges ||
            batch.ChargedBytes() > context.limits.maximumPreparedBytes / 4)
            return Return::Failure(MakeError(Editor::SceneTransactionError(Editor::SceneTransactionFailure::Capacity)));
        if (auto valid = ValidateGraph(graph, capturedGraph, batch.Plan(), context.binding.graphSource); valid.HasError())
            return Return::Failure(valid.ErrorValue());
        if (auto assets = ValidateAssets(batch, context); assets.HasError())
            return Return::Failure(assets.ErrorValue());
        const auto current = CapturePCGSceneBakeTarget(target, context.binding, context.limits);
        if (current.HasError())
            return Return::Failure(current.ErrorValue());
        const auto &view = current.Value();
        if (context.target.session != view.authored.session || context.target.revision != view.authored.revision ||
            context.target.state != view.authored.state)
            return Return::Failure(MakeError(Editor::SceneTransactionError(Editor::SceneTransactionFailure::Stale)));
        if (!context.target.canEdit)
            return Return::Failure(MakeError(Editor::SceneTransactionError(Editor::SceneTransactionFailure::Unauthorized)));
        if (context.target.cancellation.IsCancellationRequested())
            return Return::Failure(MakeError(Editor::SceneTransactionError(Editor::SceneTransactionFailure::Cancelled)));
        if (batch.Plan().Target().owner != context.binding.owner ||
            batch.Plan().Target().capabilityGeneration != context.binding.capabilityGeneration ||
            batch.Plan().Lineage() != context.binding.scope.lineage || batch.Plan().Set() != context.binding.scope.set ||
            batch.Plan().Cell() != context.binding.scope.cell)
            return Return::Failure(MakeError(PCGErrors::GenerationOwnershipMismatch));
        const auto applied = AlreadyApplied(batch, view, capturedGraph.asset);
        if (applied.HasError())
            return Return::Failure(applied.ErrorValue());
        if (applied.Value())
            return Return::Success({{view.authored.revision, view.authored.state, false},
                                    capturedGraph.session,
                                    capturedGraph.revision,
                                    capturedGraph.state,
                                    capturedGraph.dirty});
        if (auto plan = ValidatePlan(batch.Plan(), view, context); plan.HasError())
            return Return::Failure(plan.ErrorValue());
        auto changes = CaptureChanges(batch, view, capturedGraph.asset);
        if (changes.HasError())
            return Return::Failure(changes.ErrorValue());
        if (auto valid = ValidateGraph(graph, capturedGraph, batch.Plan(), context.binding.graphSource); valid.HasError())
            return Return::Failure(valid.ErrorValue());
        auto committed = target.Execute(context.target, changes.Value(), context.limits);
        if (committed.HasError())
            return Return::Failure(committed.ErrorValue());
        return Return::Success(
            {committed.Value(), capturedGraph.session, capturedGraph.revision, capturedGraph.state, capturedGraph.dirty});
    }
}  // namespace Horo::PCG
