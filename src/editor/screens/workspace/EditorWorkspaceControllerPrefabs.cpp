#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Utf8.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "editor/document/RuntimeSceneConversion.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/EditorWorkspaceControllerContentBrowserInternal.h"

#include <algorithm>
#include <limits>
#include <new>

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

        /** @brief Builds canonical source/member evidence while a newly captured host lease remains owned by the caller. */
        Result<SceneSource::ScenePrefabProjection> ResolveHierarchy(const SceneDocumentSnapshot &document,
                                                                    const Assets::AssetRegistrySnapshot &registry,
                                                                    const PrefabHierarchySourceCapture &capture,
                                                                    const std::optional<PrefabHierarchySources> &retained,
                                                                    const Prefab::PrefabLimitProfile &limits,
                                                                    const CancellationToken &cancellation,
                                                                    std::optional<PreparedPrefabHierarchySources> &prepared) {
            if (document.prefabInstances.empty())
                return Result<SceneSource::ScenePrefabProjection>::Success({});
            if (retained && HasRoots(*retained, document))
                return BuildScenePrefabProjection(document, retained->resolver, limits);
            if (!capture)
                return Result<SceneSource::ScenePrefabProjection>::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
            std::vector<Assets::AssetId> roots;
            roots.reserve(document.prefabInstances.size());
            for (const auto &placement : document.prefabInstances)
                roots.push_back(placement.sourcePrefab.Asset());
            auto loaded = capture(registry, roots, limits, cancellation);
            if (loaded.HasError())
                return Result<SceneSource::ScenePrefabProjection>::Failure(loaded.ErrorValue());
            prepared.emplace(std::move(loaded).Value());
            return BuildScenePrefabProjection(document, prepared->sources.resolver, limits);
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
    }  // namespace

    /** @copydoc EditorWorkspaceController::CapturePrefabCreationIntent */
    Result<PrefabCreationIntent> EditorWorkspaceController::CapturePrefabCreationIntent(const SceneObjectId root) const {
        const auto object = std::ranges::find(m_document.Objects(), root, &SceneObjectSnapshot::id);
        if (object == m_document.Objects().end() || m_viewModel.sceneExternalConflict || m_viewModel.recoveryAvailable ||
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
        const auto admission = CapturePrefabCreationIntent(intent.root);
        if (admission.HasError() || admission.Value().documentRevision != intent.documentRevision ||
            admission.Value().replacementEpoch != intent.replacementEpoch) {
            outcome.error =
                admission.HasError() ? admission.ErrorValue() : MakeError(SceneTransactionError(SceneTransactionFailure::Stale));
            return outcome;
        }
        auto boundPort = SceneDocumentTransactions::Bind(m_document, m_history, {m_collisionSceneOwner, intent.replacementEpoch + 1});
        const auto metadata = Application::LoadProjectMetadata(m_viewModel.projectRoot);
        if (boundPort.HasError() || metadata.HasError()) {
            outcome.error = boundPort.HasError() ? boundPort.ErrorValue() : metadata.ErrorValue();
            return outcome;
        }
        auto port = std::move(boundPort).Value();
        const auto captured = port.Snapshot();
        if (captured.HasError()) {
            outcome.error = captured.ErrorValue();
            return outcome;
        }
        const auto &snapshot = captured.Value();
        const SceneTransactionContext context{snapshot.session, snapshot.revision, snapshot.state, true};
        auto sourceFence = CreationSceneSourceFence(m_defaultScenePath, m_sceneFingerprint);
        if (sourceFence.HasError()) {
            outcome.error = sourceFence.ErrorValue();
            return outcome;
        }
        return CreatePrefabFromSelection(port,
                                         {context, intent.root, m_viewModel.projectRoot, intent.destination,
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
        if (m_viewModel.prefabCreationOutcome && m_durableFiles) {
            for (const auto &receipt : m_viewModel.prefabCreationOutcome->files) {
                if (!receipt.recoveryError)
                    continue;
                const auto current = m_durableFiles->InspectEntry(receipt.path);
                if (current.HasError() || current.Value())
                    return true;  // Only verified manual removal permits a new attempt to replace retained recovery evidence.
            }
        }
        const auto &intent = *command.prefabCreation;
        // Prepare presentation ownership before creating files; the immutable outcome keeps every failed-cleanup receipt.
        auto outcome = std::make_shared<PrefabCreationOutcome>();
        auto observerFailure =
            MakeError(SceneTransactionError(SceneTransactionFailure::Capacity),
                      Localized("workspace.prefab.create.post_commit", "Prefab created; workspace presentation needs refresh."));
        m_viewModel.prefabPostCommitError.reset();
        *outcome = ExecutePrefabCreation(intent);
        m_viewModel.prefabCreationOutcome = outcome;
        if (!outcome->committed)
            return true;
        // These canonical selection changes allocate no storage. Derived views may be rebuilt independently afterward.
        const bool selectionChanged = m_selection.ClearForTransaction();
        m_viewModel.prefabSelection = PrefabHierarchyTarget{outcome->instance, std::nullopt};
        try {
            if (selectionChanged)
                m_selection.NotifyTransactionClear();
            m_viewport.ClearTransformPreview();
            m_viewport.ClearLightPreview();
            m_dataBus.Publish(SceneDocumentChangedEvent{m_document.Revision(),
                                                        m_document.State(),
                                                        DocumentChangeKind::PrefabInstanceCreated,
                                                        m_document.IsDirty(),
                                                        {}});
            if (auto rebuilt = RebuildProjectAssetRegistry(m_viewModel.projectRoot); rebuilt.HasError()) {
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
        return true;
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
        auto limits = Prefab::PrefabLimitProfile::Create({});
        if (limits.HasError())
            return;
        std::optional<PreparedPrefabHierarchySources> prepared;
        auto resolved = ResolveHierarchy(document, m_assetRegistry, m_prefabSourceCapture, m_prefabSources, limits.Value(),
                                         m_prefabCancellation.Token(), prepared);
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
        HandleDocumentCommandResult(ApplyMutation(m_documentCommands, request), "Prefab hierarchy");
        return true;
    }
}  // namespace Horo::Editor
