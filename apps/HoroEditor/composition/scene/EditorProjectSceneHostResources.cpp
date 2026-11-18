#include "EditorProjectSceneHostOwner.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/WorldStreaming/WorldArtifactErrors.h"

#include <new>

namespace Horo::Application::Internal::EditorProjectSceneDetail {
    /** @copydoc ProjectPlaySceneHost::SelectWorldStartup */
    Result<void> ProjectPlaySceneHost::SelectWorldStartup(const std::filesystem::path &root, const EditorProjectScenePolicy &policy,
                                                          const Runtime::RuntimeSceneDefinition *definition) {
        if (!policy.world)
            return Result<void>::Success();
        if (!definition || policy.playNavigationScene ||
            (policy.xrHost && (policy.xrHost->ViewSource() != Editor::EditorPlayViewSource::SelectedXr || !policy.world->viewport)))
            return Result<void>::Failure(
                MakeError(WorldStreaming::WorldArtifactErrors::Unsupported,
                          "Mounted World Play requires a supported Scene composition and an explicit cell selection for native XR."));
        const bool startsCell = policy.world->committed.initialCell.has_value();
        const auto &viewport = policy.world->viewport;
        if (startsCell != viewport.has_value() ||
            (viewport && (!viewport->camera.IsValid() || viewport->meshes.empty() || !viewport->publication || !viewport->issueView)))
            return Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Unsupported,
                                                   "A World startup cell requires explicit canonical camera and viewport selection."));
        std::error_code error;
        if (root.empty() || !std::filesystem::equivalent(root, policy.world->projectRoot, error) || error)
            return Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale,
                                                   "A committed World startup is scoped to its selected launch project."));
        worldSelection_ = policy.world->committed;
        worldViewport_ = viewport;
        return Result<void>::Success();
    }

    /** @copydoc ProjectPlaySceneHost::WorldOrigin */
    std::optional<WorldStreaming::OriginFrameLease> ProjectPlaySceneHost::WorldOrigin() const {
        if (std::this_thread::get_id() != ownerLane_ || sceneClosed_ || !mountedWorld_ || !scenes_->ActiveScene())
            return {};
        auto lease = mountedWorld_->Origin().Lease();
        if (lease.HasError())
            return {};
        return std::move(lease).Value();
    }

    /** @copydoc ProjectPlaySceneHost::QueueInitialScene */
    Result<void> ProjectPlaySceneHost::QueueInitialScene(const Runtime::RuntimeSceneDefinition &definition,
                                                         const Editor::EditorPlayChildSceneSource *source,
                                                         const CancellationToken &cancellation) {
        if (worldSelection_ && source)
            return Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Unsupported,
                                                   "Mounted World startup does not admit a separate activated child Scene batch."));
        if (worldSelection_)
            return QueueMountedWorldStartup(definition, cancellation);
        if (!source)
            return Editor::EditorPlaySceneHost::QueueInitialScene(definition, nullptr, cancellation);
        if (cancellation.IsCancellationRequested())
            return JobCancelled();
        if (!resources_.assets_)
            return Result<void>::Failure(MakeError(InvalidProjectScene, "Child Scenes require a committed cook selection."));
        try {
            auto cooked = ReadCookedChildSceneSource(*source);
            if (cooked.HasError())
                return Result<void>::Failure(cooked.ErrorValue());

            std::vector<Runtime::RuntimeSceneDefinition> children;
            children.reserve(cooked.Value().ChildScenes().size());
            Runtime::SceneDefinitionBuilder parent{definition.Id(), definition.Revision()};
            for (const auto &entity : definition.Entities())
                parent.Add(entity);
            for (const auto &dependency : definition.AssetDependencies())
                if (auto required = parent.RequireAsset(dependency); required.HasError())
                    return required;
            for (const auto &evidence : definition.TriggerVolumeEvidence())
                if (auto required = parent.RequireTriggerVolume(evidence); required.HasError())
                    return required;
            for (const auto &reference : cooked.Value().ChildScenes()) {
                if (cancellation.IsCancellationRequested())
                    return JobCancelled();
                auto child = resources_.assets_->DecodeChildScene(reference);
                if (child.HasError())
                    return Result<void>::Failure(child.ErrorValue());
                if (auto required = parent.RequireChildScene(reference); required.HasError())
                    return required;
                children.push_back(std::move(child).Value());
            }
            auto ready = std::move(parent).Build();
            if (ready.HasError())
                return Result<void>::Failure(ready.ErrorValue());
            auto queued = scenes_->QueuePreparationWithActivatedChildren(ready.Value(), std::move(children), cancellation);
            hasActivatedChildren_ = queued.HasValue();
            return queued;
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(InvalidProjectScene, "Child Scene preparation exceeded available storage."));
        }
    }

    /** @copydoc ProjectPlaySceneHost::ReadCookedChildSceneSource */
    Result<Runtime::RuntimeSceneDefinition> ProjectPlaySceneHost::ReadCookedChildSceneSource(
        const Editor::EditorPlayChildSceneSource &source) const {
        using Output = Result<Runtime::RuntimeSceneDefinition>;
        const auto path = source.projectRelativeSource.generic_u8string();
        const auto relative = ProjectPath::Parse(std::string(path.begin(), path.end()));
        if (relative.HasError())
            return Output::Failure(relative.ErrorValue());
        const auto registry = resources_.registry_.Snapshot();
        const auto *record = registry.FindByPath(relative.Value().String());
        if (!record || record->type.Value() != "core.scene")
            return Output::Failure(MakeError(InvalidProjectScene, "Register the exact saved Play Scene as a Scene asset."));
        if (const auto *artifact = resources_.assets_->Find(record->id); !artifact || artifact->sourceDigest != source.sourceDigest)
            return Output::Failure(MakeError(InvalidProjectScene, "Re-cook the exact saved Scene before Play."));
        auto cooked = resources_.assets_->DecodeScene(record->id);
        if (cooked.HasValue() && cooked.Value().ChildScenes().empty())
            return Output::Failure(MakeError(InvalidProjectScene, "The committed Scene has no matching child references."));
        return cooked;
    }

    /** @copydoc ProjectPlaySceneHost::QueueMountedWorldStartup */
    Result<void> ProjectPlaySceneHost::QueueMountedWorldStartup(const Runtime::RuntimeSceneDefinition &definition,
                                                                const CancellationToken &cancellation) {
        if (worldViewport_ && !viewportPrimitives_)
            return Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Unsupported,
                                                   "Mounted World Play needs the active viewport's primitive-resource owner."));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        auto mounted = MountedWorld::Prepare(NativeDurableFileSystem{}, *worldSelection_, identity_.value, *scenes_, cancellation);
        if (mounted.HasError())
            return Result<void>::Failure(mounted.ErrorValue());
        if (selectedXrHost_) {
            mountedWorld_ = std::move(mounted).Value();
            auto queued = QueueMountedXrScene(definition, {cancellation, deadline});
            if (queued.HasError()) {
                worldRenderer_.reset();
                mountedWorld_.reset();
            }
            return queued;
        }
        if (const auto queued = Editor::EditorPlaySceneHost::QueueInitialScene(definition, nullptr, cancellation); queued.HasError())
            return queued;
        mountedWorld_ = std::move(mounted).Value();
        if (worldViewport_)
            worldStartup_ = MountedWorldStartup{cancellation, deadline};
        return Result<void>::Success();
    }

    /** @copydoc ProjectSceneAssetOwners::CaptureAssets */
    Result<void> ProjectSceneAssetOwners::CaptureAssets(const std::filesystem::path &root, JobSystem &jobs,
                                                        const EditorProjectScenePolicy &policy,
                                                        const Runtime::RuntimeSceneDefinition *definition) {
        if (auto captured = CaptureProjectRegistry(root); captured.HasError())
            return captured;
        if ((policy.prefabGranted || (definition && !definition->AssetDependencies().empty())) && !policy.cooked)
            return Result<void>::Failure(MakeError(InvalidProjectScene, "This Play Scene requires committed assets: "
                                                                        "supply --cooked-root and --cook-target."));
        if (policy.cooked) {
            const NativeDurableFileSystem cookFiles;
            auto captured = CommittedSceneAssets::Capture(cookFiles, *policy.cooked, registry_.Snapshot());
            if (captured.HasError())
                return Result<void>::Failure(captured.ErrorValue());
            assets_ = std::move(captured).Value();
        }
        CreateAssetLoaders(jobs, policy);
        return Result<void>::Success();
    }

    /** @copydoc ProjectSceneAssetOwners::CaptureProjectRegistry */
    Result<void> ProjectSceneAssetOwners::CaptureProjectRegistry(const std::filesystem::path &root) {
        if (root.empty())
            return Result<void>::Success();
        auto candidate = Assets::PrepareAssetRegistryCandidate(root);
        if (candidate.HasError())
            return Result<void>::Failure(candidate.ErrorValue());
        if (auto report = registry_.Publish(std::move(candidate).Value()); report.status != Assets::AssetRegistryBuildStatus::Complete)
            return Result<void>::Failure(report.diagnostics.empty() ? MakeError(InvalidProjectScene) : report.diagnostics.front().error);
        return Result<void>::Success();
    }

    /** @copydoc ProjectSceneAssetOwners::CreateAssetLoaders */
    void ProjectSceneAssetOwners::CreateAssetLoaders(JobSystem &jobs, const EditorProjectScenePolicy &policy) {
        loads_ = std::make_unique<Assets::AssetLoadService>(jobs, assets_ ? assets_->SceneProvider() : emptyProvider_);
        if (policy.uiSources) {
            uiArtifactLoads_ = std::make_unique<Assets::AssetLoadService>(jobs, assets_ ? assets_->ArtifactProvider() : emptyProvider_);
            uiLoads_ = std::make_unique<Runtime::Ui::UiRuntimeAssetLoadService>(registry_, *uiArtifactLoads_);
        }
        if (policy.prefabGranted)
            prefabLoads_ = std::make_unique<Assets::AssetLoadService>(jobs, assets_->ArtifactProvider());
    }

    /** @copydoc ProjectSceneAssetOwners::CaptureSettings */
    Result<Physics::PhysicsSceneActivationSettings> ProjectSceneAssetOwners::CaptureSettings(
        const std::filesystem::path &root, const EditorProjectScenePolicy &policy,
        const Physics::PhysicsSceneActivationAuthority &authority) {
        const auto mode = policy.physicsMode;
        auto physics = policy.physicsSettings ? Result<Physics::PhysicsWorldSettings>::Success(*policy.physicsSettings)
                                              : Physics::PhysicsWorldSettings::Capture({});
        if (physics.HasError())
            return Result<Physics::PhysicsSceneActivationSettings>::Failure(physics.ErrorValue());
        auto character = Character::CharacterWorldSettings::Capture({});
        if (character.HasError())
            return Result<Physics::PhysicsSceneActivationSettings>::Failure(character.ErrorValue());
        Physics::PhysicsWorldSimulationBinding simulation;
        if (mode == Physics::PhysicsRuntimeMode::Canonical) {
            const auto metadata = LoadProjectMetadata(root);
            if (metadata.HasError())
                return Result<Physics::PhysicsSceneActivationSettings>::Failure(metadata.ErrorValue());
            auto collision = LoadProjectCollisionSettings(root, metadata.Value().projectId);
            if (collision.HasError())
                return Result<Physics::PhysicsSceneActivationSettings>::Failure(collision.ErrorValue());
            collision_ = std::move(collision).Value().settings;
            auto captured = collision_->Capture(authority.Capture().collisionFilterGeneration);
            if (captured.HasError())
                return Result<Physics::PhysicsSceneActivationSettings>::Failure(captured.ErrorValue());
            simulation = std::move(captured).Value();
            auto surfaces = LoadProjectSurfaceSettings(root, metadata.Value().projectId);
            if (surfaces.HasError())
                return Result<Physics::PhysicsSceneActivationSettings>::Failure(std::move(surfaces).ErrorValue());
            auto semantic = surfaces.Value().settings->Capture(simulation.generation);
            if (semantic.HasError())
                return Result<Physics::PhysicsSceneActivationSettings>::Failure(std::move(semantic).ErrorValue());
            simulation.surfaces = std::move(semantic).Value();
            capturedFilterGeneration_ = simulation.generation;
        }
        return Result<Physics::PhysicsSceneActivationSettings>::Success(
            {std::move(physics).Value(), std::move(character).Value(), std::move(simulation)});
    }
}  // namespace Horo::Application::Internal::EditorProjectSceneDetail
