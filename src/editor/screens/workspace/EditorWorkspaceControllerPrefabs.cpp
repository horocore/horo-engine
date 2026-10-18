#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Utf8.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "editor/document/RuntimeSceneConversion.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Editor {
    namespace {
        constexpr std::size_t MaximumHierarchyRows = 65536;

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
