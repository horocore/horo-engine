#include "EditorProjectSceneHostOwner.h"

namespace Horo::Application::Internal::EditorProjectSceneDetail {
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
