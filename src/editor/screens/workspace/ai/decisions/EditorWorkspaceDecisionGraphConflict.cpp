#include "DecisionSaveJournal.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Foundation/PathUtils.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "graph/DecisionGraphCanvasModel.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <ranges>
#include <utility>

namespace Horo::Editor {
    namespace DecisionExternalErrors {
        const ErrorCodeDescriptor Overlap{
            .domain = ErrorDomainId{"horo.editor"},
            .code = ErrorCode{"editor.decision_graph.external_overlap"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "External Decision changes overlap the local draft.",
            .retryable = false,
            .userActionable = true,
        };
    }  // namespace DecisionExternalErrors

    struct CapturedDecisionExternal final {
        Assets::AssetCookInputSnapshot inputs;
        std::filesystem::path sourcePath;
        DecisionExternalReview review;
        DecisionGraphDocumentRoot root;
    };

    namespace {

        Result<CapturedDecisionExternal> CaptureDecisionExternal(const std::filesystem::path &root,
                                                                 const Assets::AssetRegistrySnapshot &registry,
                                                                 const DecisionGraphWorkspaceDocument &document) {
            using Captured = Result<CapturedDecisionExternal>;
            const auto limit = AI::DecisionCodecLimits{}.maximumBytes;
            auto inputs = Assets::AssetCookInputSnapshot::CaptureSelected(root, registry, std::span{&document.asset, 1},
                                                                          {.maximumSourceBytes = limit, .maximumAssets = 1}, limit);
            if (inputs.HasError())
                return Captured::Failure(inputs.ErrorValue());
            const auto &source = inputs.Value().Sources().front();
            if (source.record.sourcePath.String() != document.identity.key.source.Value() ||
                source.record.type.Value() != AI::DecisionAssetType(document.family))
                return Captured::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            auto graph = Application::DecodeRegisteredDecisionGraphSource(source);
            if (graph.HasError())
                return Captured::Failure(graph.ErrorValue());
            if (graph.Value().asset != document.graph->asset)
                return Captured::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            const auto canonical = AI::DecisionPlanCodec::EncodeSource(std::span{&graph.Value(), 1}, graph.Value().asset);
            if (canonical.HasError())
                return Captured::Failure(canonical.ErrorValue());
            auto path = Foundation::Paths::Resolve(root, source.record.sourcePath);
            auto layout = CaptureDecisionLayout(root, path, document.asset, graph.Value().asset);
            if (layout.HasError())
                return Captured::Failure(layout.ErrorValue());
            const DecisionExternalReview review{.source = {true, source.bytes.size(), source.sourceDigest},
                                                .layout = layout.Value().accepted,
                                                .revision = document.revision};
            return Captured::Success({std::move(inputs).Value(),
                                      std::move(path),
                                      review,
                                      {std::make_shared<const AI::DecisionGraphSource>(std::move(graph).Value()),
                                       std::make_shared<const DecisionGraphLayout>(std::move(layout).Value().layout),
                                       ComputeSha256(std::as_bytes(std::span{canonical.Value()}))}});
        }

        Result<void> VerifyDecisionExternal(const std::filesystem::path &root, const CapturedDecisionExternal &external) {
            if (const auto verified = external.inputs.VerifyUnchanged(); verified.HasError())
                return verified;
            const auto layout = Assets::CaptureAssetSource(root, DecisionLayoutPath(external.sourcePath), MaximumDecisionLayoutBytes);
            if (layout.HasError())
                return Result<void>::Failure(layout.ErrorValue());
            if (layout.Value().identity != external.review.layout)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            return Result<void>::Success();
        }

        template <typename Value> bool Overlaps(const Value &base, const Value &local, const Value &remote) {
            return local != base && remote != base && local != remote;
        }

        using DecisionSource = AI::DecisionGraphSource;
        using Machine = AI::StateMachineAssetDescriptor;

        Result<std::vector<std::uint8_t>> CanonicalDecision(const DecisionSource &source) {
            return AI::DecisionPlanCodec::EncodeSource(std::span{&source, 1}, source.asset);
        }

        DecisionSource MachineHeader(const DecisionSource &source, const Machine &machine) {
            return {source.asset,
                    source.schemaVersion,
                    source.blackboard,
                    source.blackboardVersion,
                    source.location,
                    source.executionTask,
                    Machine{.initial = machine.initial, .maximumTransitionsPerStep = machine.maximumTransitionsPerStep}};
        }

        template <typename Record> struct MachineRecordVersion final {
            const Record *record{};
            std::vector<std::uint8_t> canonical;
        };

        template <typename Record> using MachineRecordIndex = std::map<AI::DecisionNodeId, std::array<MachineRecordVersion<Record>, 3>>;

        template <typename Record>
        Result<void> IndexMachineRecords(const std::vector<Record> &records, std::size_t side, std::vector<Record> Machine::*member,
                                         DecisionSource &reference, MachineRecordIndex<Record> &byId) {
            auto &single = std::get<Machine>(reference.body).*member;
            for (const auto &record : records) {
                auto &entry = byId[record.id][side];
                if (entry.record)
                    return Result<void>::Failure(MakeError(DecisionExternalErrors::Overlap));
                single.assign(1, record);
                auto bytes = CanonicalDecision(reference);
                if (bytes.HasError())
                    return Result<void>::Failure(bytes.ErrorValue());
                entry = {&record, std::move(bytes).Value()};
            }
            return Result<void>::Success();
        }

        template <typename Record>
        Result<std::vector<Record>> MergeMachineRecords(const std::array<const std::vector<Record> *, 3> &versions,
                                                        std::vector<Record> Machine::*member, DecisionSource reference) {
            using Merged = Result<std::vector<Record>>;
            MachineRecordIndex<Record> byId;
            for (std::size_t side = 0; side < versions.size(); ++side) {
                if (auto indexed = IndexMachineRecords(*versions[side], side, member, reference, byId); indexed.HasError())
                    return Merged::Failure(indexed.ErrorValue());
            }
            std::vector<Record> merged;
            for (const auto &[base, local, remote] : byId | std::views::values) {
                if (Overlaps(base.canonical, local.canonical, remote.canonical))
                    return Merged::Failure(MakeError(DecisionExternalErrors::Overlap));
                const auto &selected = local.canonical == base.canonical ? remote : local;
                if (selected.record)
                    merged.push_back(*selected.record);
            }
            return Merged::Success(std::move(merged));
        }

        Result<DecisionSource> MergeDecisionMachines(const DecisionSource &base, const DecisionSource &local,
                                                     const DecisionSource &remote) {
            using Merged = Result<DecisionSource>;
            const auto *baseMachine = std::get_if<Machine>(&base.body);
            const auto *localMachine = std::get_if<Machine>(&local.body);
            const auto *remoteMachine = std::get_if<Machine>(&remote.body);
            if (!baseMachine || !localMachine || !remoteMachine)
                return Merged::Failure(MakeError(DecisionExternalErrors::Overlap));

            auto baseHeader = MachineHeader(base, *baseMachine);
            auto localHeader = MachineHeader(local, *localMachine);
            auto remoteHeader = MachineHeader(remote, *remoteMachine);
            const auto previous = CanonicalDecision(baseHeader);
            const auto draft = CanonicalDecision(localHeader);
            const auto disk = CanonicalDecision(remoteHeader);
            if (previous.HasError() || draft.HasError() || disk.HasError() || Overlaps(previous.Value(), draft.Value(), disk.Value()))
                return Merged::Failure(MakeError(DecisionExternalErrors::Overlap));

            auto states =
                MergeMachineRecords({&baseMachine->states, &localMachine->states, &remoteMachine->states}, &Machine::states, baseHeader);
            if (states.HasError())
                return Merged::Failure(states.ErrorValue());
            auto transitions = MergeMachineRecords({&baseMachine->transitions, &localMachine->transitions, &remoteMachine->transitions},
                                                   &Machine::transitions, baseHeader);
            if (transitions.HasError())
                return Merged::Failure(transitions.ErrorValue());

            auto merged = draft.Value() == previous.Value() ? std::move(remoteHeader) : std::move(localHeader);
            auto &body = std::get<Machine>(merged.body);
            body.states = std::move(states).Value();
            body.transitions = std::move(transitions).Value();
            return Merged::Success(std::move(merged));
        }

        Result<DecisionGraphDocumentRoot> MergeDecisionRoots(const DecisionGraphDocumentRoot &local,
                                                             const DecisionGraphDocumentRoot &remote, Sha256Digest baseDigest,
                                                             const DecisionSource &baseGraph, const DecisionGraphLayout &baseLayout) {
            using Merged = Result<DecisionGraphDocumentRoot>;
            auto layout = MergeDecisionLayouts(baseLayout, *local.layout, *remote.layout);
            if (layout.HasError()) {
                const auto &error = layout.ErrorValue();
                return Merged::Failure(error.code.Value() == Assets::AssetSourceErrors::Conflict.code.Value()
                                           ? MakeError(DecisionExternalErrors::Overlap)
                                           : error);
            }
            auto placement = std::make_shared<const DecisionGraphLayout>(std::move(layout).Value());
            if (!Overlaps(baseDigest, local.digest, remote.digest)) {
                const auto &semantic = local.digest == baseDigest ? remote : local;
                return Merged::Success({semantic.graph, std::move(placement), semantic.digest});
            }
            auto combined = MergeDecisionMachines(baseGraph, *local.graph, *remote.graph);
            if (combined.HasError())
                return Merged::Failure(combined.ErrorValue());
            const auto canonical = CanonicalDecision(combined.Value());
            if (canonical.HasError())
                return Merged::Failure(canonical.ErrorValue());
            return Merged::Success({std::make_shared<const DecisionSource>(std::move(combined).Value()), std::move(placement),
                                    ComputeSha256(std::as_bytes(std::span{canonical.Value()}))});
        }

        bool CanWriteExternalDecisionLayout(const AI::DecisionGraphSource &graph) {
            const auto *machine = std::get_if<AI::StateMachineAssetDescriptor>(&graph.body);
            return !machine || CanEditStateMachineTopology(*machine);
        }
    }  // namespace

    void EditorWorkspaceController::ResolveExternalDecisionGraph(const DecisionGraphDocumentFence fence,
                                                                 const DecisionExternalAction action) {
        const auto &documents = m_viewModel.decisionGraphDocuments;
        const auto document = std::ranges::find_if(documents, [fence](const auto &entry) {
            return entry.identity.instance == fence.instance && entry.revision == fence.revision;
        });
        if (document == documents.end() || !document->conflict || !document->dirty || document->interruptedSave ||
            m_viewModel.workspacePanelHost.ActiveDocument() != fence.instance)
            return;
        if (!document->externalReview)
            ProcessDecisionExternalChange({fence, DecisionExternalAction::Review});
        if (document->externalReview)
            ProcessDecisionExternalChange({fence, action, document->externalReview});
    }

    void EditorWorkspaceController::ProcessDecisionExternalChange(const DecisionExternalRequest &request) {
        const auto [instance, revision] = request.fence;
        if (m_viewModel.workspacePanelHost.ActiveDocument() != instance || !m_mutations || !m_durableFiles)
            return;
        auto &documents = m_viewModel.decisionGraphDocuments;
        const auto document = std::ranges::find_if(documents, [instance, revision](const auto &entry) {
            return entry.identity.instance == instance && entry.revision == revision && entry.conflict;
        });
        const auto draft = m_decisionGraphDrafts.find(instance);
        if (document == documents.end() || draft == m_decisionGraphDrafts.end())
            return;
        try {
            const auto result = ResolveDecisionExternalChange(*document, draft->second, request);
            if (result.HasError()) {
                document->diagnostic = result.ErrorValue();
                PublishDecisionGraphDiagnostics(*document->diagnostic);
            } else {
                document->diagnostic.reset();
            }
        } catch (const std::bad_alloc &) {
            document->diagnostic = MakeError(Assets::AssetSourceErrors::Limit);
        }
    }

    Result<void> EditorWorkspaceController::ResolveDecisionExternalChange(DecisionGraphWorkspaceDocument &document,
                                                                          DecisionGraphDraft &draft,
                                                                          const DecisionExternalRequest &request) {
        using Resolved = Result<void>;
        using enum DecisionExternalAction;
        if (HasPendingBlackboardReferenceEdit(document.identity.instance))
            return Resolved::Failure(MakeError(BlackboardDocumentErrors::ReferenceResolutionRequired));
        const auto root = ProjectRoot();
        const auto registry = m_mutableAssetRegistry ? m_mutableAssetRegistry->Snapshot() : m_assetRegistry;
        auto external = CaptureDecisionExternal(root, registry, document);
        if (external.HasError()) {
            document.externalReview.reset();
            return Resolved::Failure(external.ErrorValue());
        }
        auto captured = std::move(external).Value();
        auto &review = captured.review;
        review.graphChanged = review.source != Assets::AssetSourceIdentity{true, draft.acceptedBytes, document.source};
        review.layoutChanged = review.layout != draft.acceptedLayout;
        review.interruptedSave = document.interruptedSave;
        if (request.action != Review && (request.action == AcceptDiskPair) != document.interruptedSave.has_value())
            return Resolved::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        if (request.action == Review && review.interruptedSave) {
            const auto current = ReadDecisionSaveJournalIdentity(root, document.asset);
            if (current.HasError() || current.Value() != review.interruptedSave) {
                document.externalReview.reset();
                return Resolved::Failure(current.HasError() ? current.ErrorValue() : MakeError(Application::DecisionCookErrors::Stale));
            }
        }
        if (request.action == Review) {
            document.externalReview = review;
            return Resolved::Success();
        }
        if (document.revision == std::numeric_limits<std::uint64_t>::max() || request.reviewed != document.externalReview ||
            request.reviewed != review) {
            document.externalReview.reset();
            return Resolved::Failure(MakeError(Application::DecisionCookErrors::Stale));
        }
        if (request.action == AcceptDiskPair)
            return AcceptInterruptedDecisionSave(document, captured);
        return ApplyDecisionExternalChange(document, draft, request.action, captured);
    }

    Result<void> EditorWorkspaceController::AcceptInterruptedDecisionSave(DecisionGraphWorkspaceDocument &document,
                                                                          const CapturedDecisionExternal &captured) {
        using Accepted = Result<void>;
        const auto root = ProjectRoot();
        const auto &review = captured.review;
        const auto project = m_mutations->TryAcquire({root, ProjectMutationOwner::Asset, "decision-source-accept-external"});
        if (project.HasError())
            return Accepted::Failure(project.ErrorValue());
        auto candidate = document;
        if (auto validated = ValidateDecisionDraftRoot(*captured.root.graph, candidate); validated.HasError())
            return validated;
        if (const auto verified = VerifyDecisionExternal(root, captured); verified.HasError()) {
            document.externalReview.reset();
            return verified;
        }
        const DecisionSaveJournalKey key{document.asset, document.identity.key.source.Value(), captured.sourcePath};
        const auto accepted = AcceptDecisionSaveJournal(root, key, *review.interruptedSave, review.source, review.layout, *m_durableFiles);
        if (accepted.HasError()) {
            document.externalReview.reset();
            return accepted;
        }
        document.interruptedSave.reset();
        document.externalReview.reset();
        document.conflict = review.graphChanged || review.layoutChanged;
        ++document.revision;
        return Accepted::Success();
    }

    Result<void> EditorWorkspaceController::ApplyDecisionExternalChange(DecisionGraphWorkspaceDocument &document, DecisionGraphDraft &draft,
                                                                        DecisionExternalAction action,
                                                                        const CapturedDecisionExternal &captured) {
        using Applied = Result<void>;
        using enum DecisionExternalAction;
        const auto &external = captured.root;
        const auto &review = captured.review;
        DecisionGraphDocumentRoot next{document.graph, document.layout, draft.currentDigest};
        if (action == Reload) {
            next = external;
        } else if (action == Merge) {
            auto merged = MergeDecisionRoots(next, external, draft.savedDigest, *draft.savedGraph, *draft.savedLayout);
            if (merged.HasError())
                return Applied::Failure(merged.ErrorValue());
            next = std::move(merged).Value();
        }
        const bool layoutWritable = CanWriteExternalDecisionLayout(*next.graph);
        auto candidate = document;
        if (auto validated = ValidateDecisionDraftRoot(*next.graph, candidate); validated.HasError())
            return validated;
        const auto canvas = ProjectDecisionCanvas(*next.graph, {document.identity.instance, document.revision}, document.editable);
        if (action != Reload &&
            (PruneDecisionLayout(*next.layout, canvas) != *next.layout || (!layoutWritable && *next.layout != *external.layout)))
            return Applied::Failure(MakeError(DecisionExternalErrors::Overlap));
        if (auto verified = VerifyDecisionExternal(ProjectRoot(), captured); verified.HasError()) {
            document.externalReview.reset();
            return verified;
        }
        if (action == Reload) {
            const auto discarded = DiscardOwnedDecisionRecovery(document, draft);
            if (discarded.HasError())
                return discarded;
        }
        if (InBlackboardReferenceEdit(document.identity.instance))
            RetireBlackboardReferenceEdit();
        draft.history = DecisionGraphDocumentHistory{};
        draft.acceptedBytes = review.source.bytes;
        draft.savedDigest = external.digest;
        draft.savedGraph = external.graph;
        draft.currentDigest = next.digest;
        draft.savedLayout = external.layout;
        draft.acceptedLayout = review.layout;
        document.source = review.source.digest;
        document.graph = std::move(next.graph);
        document.layout = std::move(next.layout);
        document.layoutWritable = layoutWritable;
        document.conflict = false;
        document.externalReview.reset();
        document.validation.reset();
        document.subtreeReview.reset();
        document.queryReview.reset();
        ++document.revision;
        ProjectDecisionGraphDraft(document, draft);
        return Applied::Success();
    }
}  // namespace Horo::Editor
