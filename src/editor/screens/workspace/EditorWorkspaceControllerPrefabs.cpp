#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Utf8.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "editor/document/RuntimeSceneConversion.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/EditorWorkspaceControllerContentBrowserInternal.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        constexpr std::size_t MaximumHierarchyRows = 65536;

        /** @brief Captures the last accepted disk base; an untitled Scene has neither a path nor a persisted fingerprint. */
        Result<std::optional<PrefabCreationSourceFence>> CreationSceneSourceFence(const std::optional<std::filesystem::path> &path,
                                                                                  const std::optional<SceneFileFingerprint> &fingerprint) {
            using Return = Result<std::optional<PrefabCreationSourceFence>>;
            if (!path && !fingerprint)
                return Return::Success(std::nullopt);
            if (!path || !fingerprint || !fingerprint->exists)
                return Return::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            const auto digest = ParseSha256(fingerprint->checksum);
            if (digest.HasError())
                return Return::Failure(digest.ErrorValue());
            return Return::Success(
                PrefabCreationSourceFence{*path, {true, static_cast<std::size_t>(fingerprint->byteSize), digest.Value()}});
        }

        /** @brief Retains every authored placement when its source publication is unavailable. */
        SceneSource::ScenePrefabProjection BrokenProjection(const SceneDocumentSnapshot &document, const Error &failure,
                                                            const PrefabHierarchyProjection *retained) {
            SceneSource::ScenePrefabProjection result;
            result.instances.reserve(document.prefabInstances.size());
            for (const auto &placement : document.prefabInstances) {
                SceneSource::ScenePrefabInstanceProjection entry{.authored = placement};
                if (retained && retained->sources) {
                    const auto prior =
                        std::ranges::find(retained->sources->instances, placement, &SceneSource::ScenePrefabInstanceProjection::authored);
                    if (prior != retained->sources->instances.end())
                        entry = *prior;
                }
                entry.stale = true;
                entry.failure = failure;
                result.instances.push_back(std::move(entry));
            }
            return result;
        }

        /** @brief Requires an explicit capture when a requested root is absent from the retained closure. */
        bool HasRoots(const PrefabHierarchySources &sources, const SceneDocumentSnapshot &document) {
            return std::ranges::all_of(document.prefabInstances, [&sources](const auto &placement) {
                return sources.inputs.Find(placement.sourcePrefab.Asset()) != nullptr;
            });
        }

        /** @brief Keep the immutable schema and its admission limits paired for hierarchy resolution. */
        struct HierarchySchemaContext final {
            const Prefab::PrefabLimitProfile &limits;
            const Prefab::PrefabValueSchemaSnapshot *schemas;
        };

        /** @brief Builds canonical source/member evidence while a newly captured host lease remains owned by the caller. */
        Result<SceneSource::ScenePrefabProjection> ResolveHierarchy(const SceneDocumentSnapshot &document,
                                                                    const Assets::AssetRegistrySnapshot &registry,
                                                                    const PrefabHierarchySourceCapture &capture,
                                                                    const std::optional<PrefabHierarchySources> &retained,
                                                                    const HierarchySchemaContext &context,
                                                                    const CancellationToken &cancellation,
                                                                    std::optional<PreparedPrefabHierarchySources> &prepared) {
            if (document.prefabInstances.empty())
                return Result<SceneSource::ScenePrefabProjection>::Success({});
            if (retained && HasRoots(*retained, document))
                return BuildScenePrefabProjection(document, retained->resolver, context.limits, context.schemas);
            if (!capture)
                return Result<SceneSource::ScenePrefabProjection>::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
            std::vector<Assets::AssetId> roots;
            roots.reserve(document.prefabInstances.size());
            for (const auto &placement : document.prefabInstances)
                roots.push_back(placement.sourcePrefab.Asset());
            auto loaded = capture(registry, roots, context.limits, cancellation);
            if (loaded.HasError())
                return Result<SceneSource::ScenePrefabProjection>::Failure(loaded.ErrorValue());
            prepared.emplace(std::move(loaded).Value());
            return BuildScenePrefabProjection(document, prepared->sources.resolver, context.limits, context.schemas);
        }

        /** @brief Delegates only placement-owned mutations to the existing canonical history executor. */
        Result<SceneCommandResult> ApplyMutation(SceneDocumentCommandExecutor &commands, const PrefabHierarchyCommand &request) {
            switch (request.action) {
                using enum PrefabHierarchyAction;
                case DuplicateInstance:
                    return commands.Execute(DuplicateScenePrefabInstanceCommand{request.target.instance});
                case DeleteInstance:
                    return commands.Execute(DeleteScenePrefabInstanceCommand{request.target.instance});
                case ReparentInstance:
                    return commands.Execute(ReparentScenePrefabInstanceCommand{request.target.instance, request.destinationParent});
                default:
                    return Result<SceneCommandResult>::Failure(MakeError(Prefab::PrefabErrors::AdmissionRejected));
            }
        }

        /** @brief Translates a previously validated presentation choice to canonical Scene-owned addresses. */
        std::optional<ScenePrefabRevertTarget> OverrideSelectionTarget(const PrefabHierarchyRevert &revert,
                                                                       const PrefabHierarchyEntry &entry) {
            switch (revert.scope) {
                using enum PrefabHierarchyRevertScope;
                case Value:
                    return *entry.valueDiff[revert.row].revertValue;
                case Component:
                    return *entry.valueDiff[revert.row].revertComponent;
                case Object:
                    return entry.target.member->object;
                case Instance:
                    return AllScenePrefabOverrides{};
            }
            return std::nullopt;
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::CapturePrefabCreationIntent */
    Result<PrefabCreationIntent> EditorWorkspaceController::CapturePrefabCreationIntent(const SceneObjectId root) const {
        const auto object = std::ranges::find(m_document.Objects(), root, &SceneObjectSnapshot::id);
        const auto activeDocument = m_viewModel.workspacePanelHost.ActiveDocument();
        if (const bool prefabFocused = m_activePrefabDocument || std::ranges::any_of(m_prefabDocuments,
                                                                                     [activeDocument](const auto &document) {
            return document->Instance() == activeDocument;
        });
            prefabFocused || object == m_document.Objects().end() || m_viewModel.sceneExternalConflict || m_viewModel.recoveryAvailable ||
            m_playSession.IsActive() || !m_durableFiles || !m_mutations || !m_mutableAssetRegistry || !m_prefabSourceCapture ||
            !m_collisionSceneOwner.IsValid() || m_defaultScenePath.has_value() != m_sceneFingerprint.has_value())
            return Result<PrefabCreationIntent>::Failure(MakeError(SceneTransactionError(SceneTransactionFailure::Unauthorized)));
        if (const auto state = ResolveSceneObjectEditorState(m_document.Objects(), root);
            !state || state->effectivelyLocked || object->generated)
            return Result<PrefabCreationIntent>::Failure(MakeError(SceneTransactionError(SceneTransactionFailure::Unauthorized)));
        return Result<PrefabCreationIntent>::Success({root, m_document.Revision().value, m_document.ReplacementEpoch(), {}});
    }

    /** @copydoc EditorWorkspaceController::ExecutePrefabCreation */
    PrefabCreationOutcome EditorWorkspaceController::ExecutePrefabCreation(const PrefabCreationIntent &intent) {
        PrefabCreationOutcome outcome;
        if (const auto admission = CapturePrefabCreationIntent(intent.root);
            admission.HasError() || admission.Value().documentRevision != intent.documentRevision ||
            admission.Value().replacementEpoch != intent.replacementEpoch) {
            outcome.error =
                admission.HasError() ? admission.ErrorValue() : MakeError(SceneTransactionError(SceneTransactionFailure::Stale));
            return outcome;
        }
        const auto context = CapturePrefabSceneContext();
        const auto metadata = Application::LoadProjectMetadata(ProjectRoot());
        if (context.HasError() || metadata.HasError()) {
            outcome.error = context.HasError() ? context.ErrorValue() : metadata.ErrorValue();
            return outcome;
        }
        auto sourceFence = CreationSceneSourceFence(m_defaultScenePath, m_sceneFingerprint);
        if (sourceFence.HasError()) {
            outcome.error = sourceFence.ErrorValue();
            return outcome;
        }
        return CreatePrefabFromSelection(m_prefabSceneTransaction,
                                         {context.Value(), intent.root, ProjectRoot(), intent.destination,
                                          GenerateRandomAssetId(m_assetRegistry), metadata.Value().horoVersion.value, m_assetRegistry,
                                          m_gameplayRegistry ? &m_gameplayRegistry->Registry() : nullptr, std::move(sourceFence).Value()},
                                         *m_mutations, *m_durableFiles);
    }

    /** @copydoc EditorWorkspaceController::ProcessCreatePrefabCommand */
    bool EditorWorkspaceController::ProcessCreatePrefabCommand(const EditorWorkspaceViewCommandData &command) {
        if (command.command != EditorWorkspaceViewCommand::CreatePrefabFromSelection)
            return false;
        if (!command.prefabCreation)
            return true;
        if (!CanPublishPrefabPair())
            return true;
        const auto &intent = *command.prefabCreation;
        // Prepare presentation ownership before creating files; the immutable outcome keeps every failed-cleanup receipt.
        auto outcome = std::make_shared<PrefabCreationOutcome>();
        std::optional<Error> observerFailure{
            MakeError(SceneTransactionError(SceneTransactionFailure::Capacity),
                      Localized("workspace.prefab.create.post_commit", "Prefab created; workspace presentation needs refresh."))};
        m_viewModel.prefabPostCommitError.reset();
        *outcome = ExecutePrefabCreation(intent);
        m_viewModel.prefabCreationOutcome = outcome;
        if (!outcome->committed)
            return true;
        // These canonical selection changes allocate no storage. Derived views may be rebuilt independently afterward.
        const bool selectionChanged = m_selection.ClearForTransaction();
        m_viewModel.prefabSelection = PrefabHierarchyTarget{outcome->instance, std::nullopt};
        NotifyPrefabCreation(selectionChanged, observerFailure);
        return true;
    }

    /** @copydoc EditorWorkspaceController::CanPublishPrefabPair */
    bool EditorWorkspaceController::CanPublishPrefabPair() const {
        if (!m_viewModel.prefabCreationOutcome || !m_durableFiles)
            return true;
        for (const auto &receipt : m_viewModel.prefabCreationOutcome->files) {
            if (!receipt.recoveryError)
                continue;
            const auto current = m_durableFiles->InspectEntry(receipt.path);
            if (current.HasError() || current.Value())
                return false;
        }
        return true;
    }

    /** @copydoc EditorWorkspaceController::SavePrefabAs */
    Result<void> EditorWorkspaceController::SavePrefabAs(const PrefabDocumentSnapshot &draft, const std::filesystem::path &destination) {
        if (!draft.dirty || !m_mutations || !m_durableFiles || !m_mutableAssetRegistry || m_playSession.IsActive())
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        if (!CanPublishPrefabPair())
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));

        auto receipt = std::make_shared<PrefabCreationOutcome>();
        std::optional<Error> presentationFailure{MakeError(Prefab::PrefabErrors::DependencyUnavailable)};
        m_viewModel.prefabPostCommitError.reset();
        *receipt = CreatePrefabFork(*draft.source, ProjectRoot(), destination, GenerateRandomAssetId(m_assetRegistry), m_assetRegistry,
                                    *m_mutations, *m_durableFiles);
        m_viewModel.prefabCreationOutcome = receipt;
        if (!receipt->committed)
            return Result<void>::Failure(receipt->error.value_or(MakeError(Prefab::PrefabErrors::DocumentInvalid)));
        try {
            const auto rebuilt = RebuildProjectAssetRegistry(ProjectRoot());
            if (rebuilt.HasError())
                m_viewModel.prefabPostCommitError = rebuilt.ErrorValue();
            else {
                RefreshAssets(m_mutableAssetRegistry->Snapshot());
                if (!m_assetRegistry.Find(receipt->asset))
                    m_viewModel.prefabPostCommitError = std::move(presentationFailure);
                else {
                    OpenSourceFile({.path = destination, .origin = SourceOpenOrigin::Command, .mode = SourceOpenMode::EmbeddedOnly});
                    if (std::ranges::none_of(m_prefabDocuments, [&](const auto &entry) {
                        return entry->HasPresentation() && entry->Snapshot().source->Data().assetId == receipt->asset;
                    }))
                        m_viewModel.prefabPostCommitError = std::move(presentationFailure);
                }
            }
        } catch (...) {
            m_viewModel.prefabPostCommitError = std::move(presentationFailure);
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::NotifyPrefabCreation */
    void EditorWorkspaceController::NotifyPrefabCreation(const bool selectionChanged, std::optional<Error> &observerFailure) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<std::optional<Error>>);
        static_assert(std::is_nothrow_move_assignable_v<std::optional<Error>>);
        try {
            if (selectionChanged)
                m_selection.NotifyTransactionClear();
            m_viewport.ClearTransformPreview();
            m_viewport.ClearLightPreview();
            m_dataBus.Publish(SceneDocumentChangedEvent{m_document.Revision(), m_document.State(),
                                                        DocumentChangeKind::PrefabInstanceCreated, m_document.IsDirty(),
                                                        std::vector<SceneObjectId>(0)});
            if (auto rebuilt = RebuildProjectAssetRegistry(ProjectRoot()); rebuilt.HasError()) {
                m_viewModel.prefabPostCommitError = rebuilt.ErrorValue();
            } else {
                m_assetRegistry = m_mutableAssetRegistry->Snapshot();
                if (rebuilt.Value().status != Assets::AssetRegistryBuildStatus::Complete)
                    m_viewModel.prefabPostCommitError = MakeError(Prefab::PrefabErrors::DependencyUnavailable);
            }
            RefreshSceneProjections();
        } catch (...) {
            // Every authoritative owner already committed; observer allocation/failure never removes created assets.
            m_viewModel.prefabPostCommitError = std::move(observerFailure);
        }
    }

    /** @copydoc EditorWorkspaceController::RetirePrefabHierarchy */
    void EditorWorkspaceController::RetirePrefabHierarchy() noexcept {
        m_viewModel.prefabSelection.reset();
        m_viewModel.prefabHierarchy.reset();
        m_prefabSources.reset();
        m_viewModel.prefabHierarchyError.reset();
        m_viewModel.prefabHierarchyUnavailable = false;
    }

    /** @copydoc EditorWorkspaceController::RefreshPrefabHierarchy */
    void EditorWorkspaceController::RefreshPrefabHierarchy(const SceneDocumentSnapshot &document) {
        m_viewModel.prefabApplyAvailable = m_defaultScenePath && m_sceneFingerprint && m_sceneFingerprint->exists && m_mutations &&
                                           m_durableFiles && m_collisionSceneOwner.IsValid() &&
                                           std::ranges::none_of(m_prefabApplyRecovery, [](const auto &item) {
            return item.has_value();
        });
        if (m_viewModel.prefabHierarchy && m_prefabObservedDocumentEpoch != m_document.ReplacementEpoch())
            RetirePrefabHierarchy();
        if (!m_workspaceSession.IsValid() && document.prefabInstances.empty()) {
            RetirePrefabHierarchy();
            return;
        }
        if (m_prefabPublication == std::numeric_limits<std::uint64_t>::max()) {
            m_viewModel.prefabHierarchyUnavailable = true;
            return;
        }
        const auto nextPublication = m_prefabPublication + 1;
        try {
            PrepareAndPublishPrefabHierarchy(document, nextPublication);
        } catch (const std::bad_alloc &) {
            m_viewModel.prefabHierarchyUnavailable = true;
            LOG_ERROR("editor.prefab", "Hierarchy preparation exhausted bounded storage; prior projection retained.");
        }
    }

    /** @copydoc EditorWorkspaceController::PrepareAndPublishPrefabHierarchy */
    void EditorWorkspaceController::PrepareAndPublishPrefabHierarchy(const SceneDocumentSnapshot &document,
                                                                     const std::uint64_t nextPublication) {
        auto limits = PrefabLimits();
        if (limits.HasError())
            return;
        if (EnsureNavigationPrefabSchemas(limits.Value()).HasError())
            return;
        std::optional<PreparedPrefabHierarchySources> prepared;
        auto resolved = ResolveHierarchy(document, m_assetRegistry, m_prefabSourceCapture, m_prefabSources,
                                         {limits.Value(), &*m_navigationPrefabSchemas}, m_prefabCancellation.Token(), prepared);
        auto sources = std::make_shared<const SceneSource::ScenePrefabProjection>(
            resolved.HasValue() ? std::move(resolved).Value()
                                : BrokenProjection(document, resolved.ErrorValue(), m_viewModel.prefabHierarchy.get()));
        auto hierarchy = BuildPrefabHierarchyProjection(m_workspaceSession, nextPublication, document.revision.value, std::move(sources),
                                                        MaximumHierarchyRows, document.objects, limits.Value());
        if (hierarchy.HasError()) {
            m_viewModel.prefabHierarchyUnavailable = true;
            m_viewModel.prefabHierarchyError = hierarchy.ErrorValue();
            LOG_ERROR("editor.prefab", "Hierarchy preparation failed: %s", hierarchy.ErrorValue().message.c_str());
            return;
        }
        auto publication = std::make_shared<const PrefabHierarchyProjection>(std::move(hierarchy).Value());
        if (prepared)
            m_prefabSources.emplace(std::move(prepared->sources));
        m_prefabPublication = nextPublication;
        m_prefabObservedDocumentEpoch = m_document.ReplacementEpoch();
        m_viewModel.prefabHierarchy = std::move(publication);
        m_viewModel.prefabHierarchyError.reset();
        m_viewModel.prefabHierarchyUnavailable = false;
        if (m_viewModel.prefabSelection && !m_viewModel.prefabHierarchy->Find(*m_viewModel.prefabSelection))
            m_viewModel.prefabSelection.reset();
    }

    /** @copydoc EditorWorkspaceController::RevertPrefabHierarchyOverrides */
    Result<SceneCommandResult> EditorWorkspaceController::RevertPrefabHierarchyOverrides(const PrefabHierarchyCommand &request,
                                                                                         const PrefabHierarchyEntry &entry) {
        using Return = Result<SceneCommandResult>;
        auto selection = OverrideSelectionTarget(*request.revert, entry);
        if (!selection)
            return Return::Failure(MakeError(Prefab::PrefabErrors::AddressInvalid));
        if (!m_prefabSources || !m_prefabSourceCapture || !m_navigationPrefabSchemas ||
            m_assetRegistry.Revision() != m_prefabSources->resolver.RegistryRevision())
            return Return::Failure(MakeError(Prefab::PrefabErrors::OverrideSourceStale));
        const auto instances = m_document.PrefabInstances();
        const auto instance = std::ranges::find(instances, request.target.instance, &ScenePrefabInstance::instanceId);
        if (instance == instances.end())
            return Return::Failure(MakeError(Prefab::PrefabErrors::OverrideOrphan));
        const auto limits = PrefabLimits();
        if (limits.HasError())
            return Return::Failure(limits.ErrorValue());
        const std::array roots{instance->sourcePrefab.Asset()};
        auto previous = m_prefabSources->resolver.CaptureResolutionRevision(roots.front(), limits.Value());
        if (previous.HasError())
            return Return::Failure(previous.ErrorValue());
        const auto cancellation = m_prefabCancellation.Token();
        const auto fresh = m_prefabSourceCapture(m_assetRegistry, roots, limits.Value(), cancellation);
        if (fresh.HasError())
            return Return::Failure(fresh.ErrorValue());
        if (auto current = fresh.Value().sources.inputs.VerifyUnchanged(cancellation); current.HasError())
            return Return::Failure(current.ErrorValue());
        SceneDocumentPrefabValueCommands owner{m_document, m_history};
        return owner.Execute(RevertScenePrefabOverridesCommand{request.target.instance,
                                                               {request.documentRevision},
                                                               std::move(previous).Value(),
                                                               m_navigationPrefabSchemas->Revision(),
                                                               std::move(*selection),
                                                               m_prefabObservedDocumentEpoch},
                             fresh.Value().sources.resolver, &*m_navigationPrefabSchemas, limits.Value(), cancellation);
    }

    /** @copydoc EditorWorkspaceController::CapturePrefabSceneContext */
    Result<SceneTransactionContext> EditorWorkspaceController::CapturePrefabSceneContext() {
        using Return = Result<SceneTransactionContext>;
        const SceneTransactionSession session{m_collisionSceneOwner, m_document.ReplacementEpoch() + 1};
        if (m_prefabSceneTransaction.CheckCurrent().HasError()) {
            auto bound = SceneDocumentTransactions::Bind(m_document, m_history, session);
            if (bound.HasError())
                return Return::Failure(std::move(bound).ErrorValue());
            m_prefabSceneTransaction = std::move(bound).Value();
        }
        return Return::Success({session, m_document.Revision().value, m_document.State().value, true, m_prefabCancellation.Token()});
    }

    /** @copydoc EditorWorkspaceController::ApplyPrefabHierarchyOverrides */
    Result<void> EditorWorkspaceController::ApplyPrefabHierarchyOverrides(const PrefabHierarchyCommand &request,
                                                                          const PrefabHierarchyEntry &entry) {
        if (!m_viewModel.prefabApplyAvailable || m_viewModel.recoveryAvailable || !m_prefabSources || !m_navigationPrefabSchemas)
            return Result<void>::Failure(MakeError(SceneTransactionError(SceneTransactionFailure::Unauthorized)));
        const auto selection = OverrideSelectionTarget(*request.revert, entry);
        const auto *root = m_viewModel.prefabHierarchy->Find({request.target.instance, std::nullopt});
        if (!selection || !root)
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::AddressInvalid));
        const auto source = std::ranges::find_if(m_prefabDocuments, [asset = root->source](const auto &document) {
            return document->HasPresentation() && document->Snapshot().source->Data().assetId == asset;
        });
        if (source == m_prefabDocuments.end())
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
        const auto limits = PrefabLimits();
        if (limits.HasError())
            return Result<void>::Failure(limits.ErrorValue());
        auto expected = m_prefabSources->resolver.CaptureResolutionRevision(root->source, limits.Value());
        if (expected.HasError())
            return Result<void>::Failure(std::move(expected).ErrorValue());
        auto disk = CreationSceneSourceFence(m_defaultScenePath, m_sceneFingerprint);
        if (disk.HasError() || !disk.Value())
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        const auto context = CapturePrefabSceneContext();
        if (context.HasError())
            return Result<void>::Failure(context.ErrorValue());
        const auto schema = std::make_shared<const Prefab::PrefabValueSchemaSnapshot>(*m_navigationPrefabSchemas);
        return CompletePrefabApply(PrefabApplyTransaction::Execute(m_prefabSceneTransaction, **source,
                                                                   {context.Value(),
                                                                    {request.target.instance,
                                                                     {request.documentRevision},
                                                                     std::move(expected).Value(),
                                                                     schema->Revision(),
                                                                     *selection,
                                                                     m_prefabObservedDocumentEpoch},
                                                                    (*source)->Snapshot().fence,
                                                                    disk.Value()->path,
                                                                    disk.Value()->accepted},
                                                                   m_assetRegistry, *m_prefabSources, schema, limits.Value(), *m_mutations,
                                                                   *m_durableFiles),
                                   DocumentChangeKind::PrefabInstanceValuesChanged);
    }

    /** @copydoc EditorWorkspaceController::ReconcilePrefabApplyHistory */
    void EditorWorkspaceController::ReconcilePrefabApplyHistory() {
        if (m_prefabSceneTransaction.CheckCurrent().HasError() && std::ranges::none_of(m_prefabDocuments, [](const auto &document) {
            return document->HasApplyHistory();
        }))
            return;
        const auto context = CapturePrefabSceneContext();
        if (context.HasError())
            return;
        auto maintenance = context.Value();
        maintenance.cancellation = {};
        PrefabApplyTransaction::ReconcileHistory(m_prefabSceneTransaction, maintenance, m_prefabDocuments);
        std::erase_if(m_prefabDocuments, [](const auto &document) {
            return !document->HasPresentation() && !document->HasApplyHistory();
        });
        m_viewModel.canUndo = m_history.CanUndo();
        m_viewModel.canRedo = m_history.CanRedo();
    }

    /** @copydoc EditorWorkspaceController::ReplayPrefabApply */
    Result<bool> EditorWorkspaceController::ReplayPrefabApply(const bool redo, PrefabDocumentSession *source) {
        using Return = Result<bool>;
        const auto requested = source ? source->NextApplyGroup(redo) : std::nullopt;
        if (source && !requested)
            return Return::Success(false);
        if (!m_collisionSceneOwner.IsValid())
            return source ? Return::Failure(MakeError(Prefab::PrefabErrors::OverrideSourceStale)) : Return::Success(false);
        const auto context = CapturePrefabSceneContext();
        if (context.HasError())
            return Return::Failure(context.ErrorValue());
        const auto group = PrefabApplyTransaction::NextSceneGroup(m_prefabSceneTransaction, context.Value(), redo);
        if (group.HasError())
            return Return::Failure(group.ErrorValue());
        if (!group.Value() && !source)
            return Return::Success(false);
        if (!group.Value() || (source && requested != group.Value()))
            return Return::Failure(MakeError(Prefab::PrefabErrors::OverrideSourceStale));
        if (!source) {
            const auto match = std::ranges::find_if(m_prefabDocuments, [&](const auto &candidate) {
                return candidate->NextApplyGroup(redo) == group.Value();
            });
            if (match == m_prefabDocuments.end())
                return Return::Failure(MakeError(Prefab::PrefabErrors::OverrideSourceStale));
            source = match->get();
        }
        if (!m_viewModel.prefabApplyAvailable || m_viewModel.sceneExternalConflict || m_viewModel.recoveryAvailable ||
            m_playSession.IsActive())
            return Return::Failure(MakeError(SceneTransactionError(SceneTransactionFailure::Unauthorized)));
        if (auto drained = DrainPrefabRecovery(); drained.HasError())
            return Return::Failure(drained.ErrorValue());
        if (auto inspected = PrefabApplyTransaction::InspectSource(*source); inspected.HasError())
            return Return::Failure(std::move(inspected).ErrorValue());
        if (source->Snapshot().externalConflict) {
            ReconcilePrefabApplyHistory();
            return Return::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        }
        const auto disk = CreationSceneSourceFence(m_defaultScenePath, m_sceneFingerprint);
        if (disk.HasError() || !disk.Value())
            return Return::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        auto completion = CompletePrefabApply(PrefabApplyTransaction::Replay(m_prefabSceneTransaction, *source,
                                                                             {context.Value(), source->Snapshot().fence, disk.Value()->path,
                                                                              disk.Value()->accepted, *group.Value(), redo},
                                                                             *m_mutations, *m_durableFiles),
                                              redo ? DocumentChangeKind::Redone : DocumentChangeKind::Undone);
        if (completion.HasError())
            return Return::Failure(std::move(completion).ErrorValue());
        return Return::Success(true);
    }

    /** @copydoc EditorWorkspaceController::CompletePrefabApply */
    Result<void> EditorWorkspaceController::CompletePrefabApply(PrefabApplyOutcome outcome, const DocumentChangeKind kind) {
        if (!outcome.committed) {
            if (std::ranges::any_of(outcome.recovery, [](const auto &item) {
                return item.has_value();
            })) {
                m_prefabApplyRecovery = std::move(outcome.recovery);
                m_viewModel.prefabApplyAvailable = false;
                m_viewModel.sceneExternalConflict = true;
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed));
            }
            return Result<void>::Failure(std::move(*outcome.error));
        }
        m_prefabSources.reset();
        m_viewModel.prefabOverrideError.reset();
        const auto &identity = outcome.sceneIdentity;
        const auto &committed = *outcome.committed;
        try {
            m_viewport.ClearTransformPreview();
            m_viewport.ClearLightPreview();
            static_cast<void>(CommitSceneSave({committed.revision}, {committed.state},
                                              {identity.exists, identity.bytes, std::move(outcome.sceneChecksum)}, {}, kind));
            m_selection.Reconcile();
            ScheduleContentBrowserPreviews();
        } catch (...) {
            LOG_ERROR("editor.prefab", "Paired Prefab publication committed, but workspace refresh failed.");
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ProcessPrefabHierarchyCommand */
    bool EditorWorkspaceController::ProcessPrefabHierarchyCommand(const EditorWorkspaceViewCommandData &command) {
        if (command.command != EditorWorkspaceViewCommand::PrefabHierarchy)
            return false;
        if (!command.prefabHierarchy || !m_viewModel.prefabHierarchy || m_viewModel.prefabHierarchyUnavailable)
            return true;
        const auto &request = *command.prefabHierarchy;
        if (request.publication != m_prefabPublication || m_prefabObservedDocumentEpoch != m_document.ReplacementEpoch() ||
            request.documentRevision != m_document.Revision().value)
            return true;
        auto admitted = ValidatePrefabHierarchyCommand(request, *m_viewModel.prefabHierarchy);
        if (admitted.HasError()) {
            LOG_WARN("editor.prefab", "Hierarchy action rejected: %s", admitted.ErrorValue().message.c_str());
            return true;
        }
        if (request.action == PrefabHierarchyAction::Select || request.action == PrefabHierarchyAction::LocateInstance) {
            m_selection.Clear();
            m_viewModel.prefabSelection = request.action == PrefabHierarchyAction::LocateInstance
                                              ? PrefabHierarchyTarget{request.target.instance, std::nullopt}
                                              : request.target;
            RefreshSelectionProjection();
            return true;
        }
        if (request.action == PrefabHierarchyAction::OpenSource) {
            if (const auto *record = m_assetRegistry.Find(admitted.Value()->source)) {
                const auto &encoded = record->sourcePath.String();
                if (!IsValidUtf8ScalarSequence(encoded) || encoded.find('\0') != std::string::npos) {
                    m_viewModel.contentBrowserOperationError = "workspace.source_open.unsafe";
                    return true;
                }
                const std::filesystem::path path{std::u8string{encoded.begin(), encoded.end()}};
                OpenSourceFile({.path = path, .origin = SourceOpenOrigin::Command});
            }
            return true;
        }
        if (m_viewModel.sceneExternalConflict || m_playSession.IsActive())
            return true;
        if (request.action == PrefabHierarchyAction::ApplyOverrides) {
            auto drained = DrainPrefabRecovery();
            auto applied = drained.HasError() ? std::move(drained) : ApplyPrefabHierarchyOverrides(request, *admitted.Value());
            if (applied.HasError())
                m_viewModel.prefabOverrideError = std::move(applied).ErrorValue();
            return true;
        }
        if (request.action == PrefabHierarchyAction::RevertOverrides) {
            HandleDocumentCommandResult(RevertPrefabHierarchyOverrides(request, *admitted.Value()), "Revert prefab overrides");
            return true;
        }
        HandleDocumentCommandResult(ApplyMutation(m_documentCommands, request), "Prefab hierarchy");
        return true;
    }
}  // namespace Horo::Editor
