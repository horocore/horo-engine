#include "Horo/Navigation/NavigationSceneActivation.h"

#include <new>
#include <utility>
#include <vector>

namespace Horo::Navigation {
    namespace {
        class NavigationSceneCandidate final : public Runtime::SceneActivationCandidate {
        public:
            explicit NavigationSceneCandidate(std::unique_ptr<NavigationAgentSceneCandidate> candidate) noexcept
                : candidate_(std::move(candidate)) {}

            [[nodiscard]] Result<void> ValidatePublication() const override {
                return candidate_->ValidatePublication();
            }

            void Publish() noexcept override {
                candidate_->Publish();
            }

            void Shutdown() noexcept override {
                candidate_->Shutdown();
            }

        private:
            std::unique_ptr<NavigationAgentSceneCandidate> candidate_;
        };

        [[nodiscard]] Result<NavigationAgentSceneBinding> MakeBinding(const Runtime::RuntimeSceneView scene) {
            const Runtime::SceneRuntimeId runtimeId = scene.RuntimeId();
            if (!runtimeId.IsValid())
                return Result<NavigationAgentSceneBinding>::Failure(MakeError(NavigationErrors::AgentRegistryStale));
            const auto world = NavigationWorldId::Create(runtimeId.value);
            const auto sceneId = NavigationSceneRuntimeId::Create(runtimeId.value);
            const auto generation = NavigationSceneGeneration::Create(runtimeId.value);
            if (world.HasError())
                return Result<NavigationAgentSceneBinding>::Failure(world.ErrorValue());
            if (sceneId.HasError())
                return Result<NavigationAgentSceneBinding>::Failure(sceneId.ErrorValue());
            if (generation.HasError())
                return Result<NavigationAgentSceneBinding>::Failure(generation.ErrorValue());
            return Result<NavigationAgentSceneBinding>::Success(
                NavigationAgentSceneBinding{.world = world.Value(), .scene = sceneId.Value(), .sceneGeneration = generation.Value()});
        }
    }  // namespace

    /** @copydoc NavigationSceneActivationParticipant::Prepare */
    Result<std::unique_ptr<Runtime::SceneActivationCandidate>> NavigationSceneActivationParticipant::Prepare(
        const Runtime::RuntimeSceneDefinition &definition, const Runtime::RuntimeSceneView scene) {
        if (registry_ == nullptr || !scene.IsCurrent())
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(MakeError(NavigationErrors::AgentRegistryStale));

        const auto binding = MakeBinding(scene);
        if (binding.HasError())
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(binding.ErrorValue());

        try {
            std::vector<NavigationAgentDescriptor> descriptors;
            descriptors.reserve(definition.Entities().size());
            for (const Runtime::RuntimeEntityDefinition &entity : definition.Entities()) {
                if (!entity.components.navigationAgent || !entity.components.navigationAgent->enabled)
                    continue;
                const auto runtimeEntity = scene.Find(entity.object);
                if (!runtimeEntity)
                    return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(
                        MakeError(NavigationErrors::AgentRegistryStale));
                const Runtime::NavigationAgentComponent &component = *entity.components.navigationAgent;
                descriptors.push_back(NavigationAgentDescriptor{
                    .owner = NavigationAgentOwner{.scene = binding.Value().scene,
                                                  .entityIndex = runtimeEntity->entity.index,
                                                  .entityGeneration = runtimeEntity->entity.generation},
                    .profile = component.profile,
                    .filter = component.filter,
                    .radiusOverride = component.radiusOverride,
                    .movementCapability = component.movementCapability,
                    .enabled = component.enabled,
                });
            }
            auto candidate = registry_->PrepareScene(binding.Value(), descriptors);
            if (candidate.HasError())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(candidate.ErrorValue());
            auto adapter = std::make_unique<NavigationSceneCandidate>(std::move(candidate).Value());
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(std::move(adapter));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(
                MakeError(NavigationErrors::AgentRegistryCapacityExceeded));
        }
    }
}  // namespace Horo::Navigation
