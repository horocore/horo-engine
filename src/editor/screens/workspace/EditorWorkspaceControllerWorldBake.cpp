#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

namespace Horo::Editor {
    namespace {
        /** @brief Captures exact registry spelling and navigable page origins from one immutable project publication. */
        std::vector<Application::WorldBakePageSource> CapturePageSources(const PartitionedWorldSnapshot &current,
                                                                         const Assets::AssetRegistrySnapshot &registry,
                                                                         const std::filesystem::path &root,
                                                                         const std::string &documentSource) {
            std::vector<Application::WorldBakePageSource> sources;
            for (const auto &page : current.document.pages) {
                if (const auto *record = registry.Find(page.head.sourceAsset)) {
                    const auto relative = record->sourcePath.String();
                    const auto absolute = (root / relative).lexically_normal();
                    sources.push_back({page.head, {absolute.string(), 1, 1}, relative});
                } else if (!documentSource.empty()) {
                    const auto absolute = (root / documentSource).lexically_normal();
                    sources.push_back({page.head, {absolute.string(), 1, 1}, {}});
                }
            }
            return sources;
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::InitializeWorldBake */
    void EditorWorkspaceController::InitializeWorldBake(const EditorWorkspaceDependencies &dependencies) {
        m_worldBakeOperations = dependencies.worldBake.operations;
        if (dependencies.jobs && dependencies.worldBake.operations && dependencies.worldBake.output)
            m_worldBake = std::make_unique<Application::WorldBakeApplication>(*dependencies.jobs, *dependencies.worldBake.operations,
                                                                              *dependencies.worldBake.output);
        if (dependencies.worldBake.project)
            m_worldBakeProject = *dependencies.worldBake.project;
        else {
            constexpr std::string_view core = "Horo.Editor.WorldBake.Core.v1";
            const auto digest = ComputeSha256(std::as_bytes(std::span{core.data(), core.size()}));
            m_worldBakeProject.fence = {dependencies.world.documentSessionOwner, 1, digest};
            m_worldBakeProject.settings = digest;
            m_worldBakeProject.triggers = dependencies.triggerBinding;
            m_worldBakeProject.dependencySource = dependencies.worldBake.dependencies;
        }
        m_viewModel.worldBakeAvailable = m_worldBake != nullptr;
    }

    /** @copydoc EditorWorkspaceController::SetWorldBakeProject */
    Result<void> EditorWorkspaceController::SetWorldBakeProject(Application::WorldBakeProjectCapture capture) {
        if (auto admitted = Application::ValidateWorldBakeProjectCapture(capture); admitted.HasError())
            return admitted;
        if (capture.fence.project == m_worldBakeProject.fence.project && capture.fence.revision <= m_worldBakeProject.fence.revision)
            return Result<void>::Failure(MakeError(Application::WorldBakeErrors::Stale));
        m_worldBakeProject = std::move(capture);
        UpdateWorldBake();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ReserveWorldBakeSources */
    Result<std::shared_ptr<const ProjectMutationLease>> EditorWorkspaceController::ReserveWorldBakeSources(
        const Application::WorldBakeRequest &request) {
        if (!request.sourceAuthority.files && !request.project.dependencySource)
            return Result<std::shared_ptr<const ProjectMutationLease>>::Success({});
        if (auto existing = m_worldBakeSourceReservation.lock())
            return Result<std::shared_ptr<const ProjectMutationLease>>::Success(std::move(existing));
        if (!m_mutations)
            return Result<std::shared_ptr<const ProjectMutationLease>>::Failure(MakeError(Application::WorldBakeErrors::Dependency));
        auto lease = m_mutations->TryAcquire(
            {std::filesystem::path{m_viewModel.projectRoot}, ProjectMutationOwner::Asset, "world.bake.source.capture"});
        if (lease.HasError())
            return Result<std::shared_ptr<const ProjectMutationLease>>::Failure(lease.ErrorValue());
        auto retained = std::make_shared<const ProjectMutationLease>(std::move(lease).Value());
        m_worldBakeSourceReservation = retained;
        return Result<std::shared_ptr<const ProjectMutationLease>>::Success(std::move(retained));
    }

    /** @copydoc EditorWorkspaceController::StartWorldBake */
    Result<OperationId> EditorWorkspaceController::StartWorldBake(const Application::WorldBakeMode mode,
                                                                  const WorldDocumentFence &expected) {
        if (!m_worldBake)
            return Result<OperationId>::Failure(MakeError(Application::WorldBakeErrors::Closed));
        const auto &owner = m_partitionedWorld ? *m_partitionedWorld : WorldDocuments();
        const auto current = owner.Snapshot();
        if (current.HasError())
            return Result<OperationId>::Failure(current.ErrorValue());
        if (current.Value()->fence != expected)
            return Result<OperationId>::Failure(MakeError(WorldDocumentErrors::Stale));
        const auto registry = m_mutableAssetRegistry ? m_mutableAssetRegistry->Snapshot() : m_assetRegistry;
        auto project = m_worldBakeProject;
        project.fence.registryRevision = registry.Revision().value;
        if (!m_gameplayRegistry)
            return Result<OperationId>::Failure(MakeError(Application::WorldBakeErrors::Dependency));
        if (const auto *error = m_gameplayRegistry->ReplicationMetadataError())
            return Result<OperationId>::Failure(*error);
        if (!m_gameplayRegistry->ReplicationMetadata())
            return Result<OperationId>::Failure(MakeError(Application::WorldBakeErrors::Dependency));
        project.replication = m_gameplayRegistry->ReplicationMetadata();
        project.fence.replication = project.replication->Fingerprint();
        Application::WorldBakeRequest request{.document = current.Value(),
                                              .project = std::move(project),
                                              .registry = registry,
                                              .sourceAuthority = {m_worldSourceFiles, m_workspaceSession, 1},
                                              .mode = mode};
        auto reserved = ReserveWorldBakeSources(request);
        if (reserved.HasError())
            return Result<OperationId>::Failure(reserved.ErrorValue());
        request.sourceAuthority.reservation = std::move(reserved).Value();
        if (m_localization)
            request.title = m_localization->Get("editor", mode == Application::WorldBakeMode::Bake ? "workspace.world.bake"
                                                                                                   : "workspace.world.validate");
        request.sources =
            CapturePageSources(*current.Value(), registry, std::filesystem::path{m_viewModel.projectRoot}, m_viewModel.worldDocumentSource);
        const auto accepted = m_worldBake->Submit(std::move(request));
        UpdateWorldBake();
        return accepted;
    }

    /** @copydoc EditorWorkspaceController::UpdateWorldBake */
    void EditorWorkspaceController::UpdateWorldBake() {
        if (!m_worldBake)
            return;
        const auto current = (m_partitionedWorld ? *m_partitionedWorld : WorldDocuments()).Snapshot();
        auto project = m_worldBakeProject.fence;
        project.replication =
            m_gameplayRegistry && !m_gameplayRegistry->ReplicationMetadataError() && m_gameplayRegistry->ReplicationMetadata()
                ? m_gameplayRegistry->ReplicationMetadata()->Fingerprint()
                : Sha256Digest{};
        project.registryRevision = (m_mutableAssetRegistry ? m_mutableAssetRegistry->Snapshot() : m_assetRegistry).Revision().value;
        const auto lease = current.HasValue() ? current.Value() : PartitionedWorldSnapshotLease{};
        m_worldBake->Pump(lease, project);
        m_viewModel.worldBakeOperation = m_worldBake->ActiveOperation();
        const auto output = m_worldBake->Published();
        m_viewModel.worldBakeCurrent = output && lease && output->document == lease->fence && output->project == project;
    }

    /** @copydoc EditorWorkspaceController::RequestWorldBakeCancellation */
    Result<void> EditorWorkspaceController::RequestWorldBakeCancellation(const std::optional<OperationId> expected) {
        if (!m_worldBakeOperations || !expected.has_value() || expected != m_viewModel.worldBakeOperation)
            return Result<void>::Failure(MakeError(Application::WorldBakeErrors::Stale));
        if (!m_worldBakeOperations->RequestCancel(*expected))
            return Result<void>::Failure(MakeError(Application::WorldBakeErrors::Stale));
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ProcessWorldBakeCommand */
    bool EditorWorkspaceController::ProcessWorldBakeCommand(const EditorWorkspaceViewCommandData &command) {
        using enum EditorWorkspaceViewCommand;
        if (command.command == CancelWorldBake) {
            if (auto cancelled = RequestWorldBakeCancellation(command.worldBakeOperation); cancelled.HasError())
                m_viewModel.worldDocumentError = cancelled.ErrorValue();
            return true;
        }
        if (command.command != BakeWorldCells && command.command != ValidateWorldCells)
            return false;
        if (!command.worldDocumentFence) {
            m_viewModel.worldDocumentError = MakeError(WorldDocumentErrors::Invalid);
            return true;
        }
        const auto mode = command.command == BakeWorldCells ? Application::WorldBakeMode::Bake : Application::WorldBakeMode::Validate;
        const auto accepted = StartWorldBake(mode, *command.worldDocumentFence);
        m_viewModel.worldDocumentError = accepted.HasError() ? std::optional{accepted.ErrorValue()} : std::nullopt;
        return true;
    }
}  // namespace Horo::Editor
