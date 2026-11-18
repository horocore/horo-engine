#include "Horo/Navigation/NavigationSceneActivation.h"

#include "Horo/Navigation/NavigationCoordinator.h"

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

            /** @brief The prepared navigation root owns runtime navigation state, not a persistent-world dataset identity. */
            Runtime::SceneCanonicalDatasetProjection CanonicalDatasetProjection() const noexcept override {
                return Runtime::SceneCanonicalDatasetProjection::Absent;
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

        [[nodiscard]] Result<NavigationAgentSceneBinding> MakeBinding(const Runtime::RuntimeSceneView &scene) {
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

        /** @brief Captures the same authored component identity on Scene activation and group creation. */
        [[nodiscard]] NavigationAgentDescriptor MakeDescriptor(const NavigationAgentSceneBinding binding, const Runtime::EntityRef entity,
                                                               const Runtime::NavigationAgentComponent &component) noexcept {
            return {.owner = {binding.scene, entity.entity.index, entity.entity.generation},
                    .profile = component.profile,
                    .filter = component.filter,
                    .radiusOverride = component.radiusOverride,
                    .movementCapability = component.movementCapability,
                    .enabled = component.enabled};
        }

        /** @brief Carries an exact provider-world fence alongside a detached registry delta. */
        class NavigationStructuralCandidate final : public Runtime::SceneStructuralCandidate {
        public:
            explicit NavigationStructuralCandidate(NavigationWorldLifecycle &world, NavigationCoordinator &coordinator,
                                                   const NavigationWorldActivationDescriptor activation = {},
                                                   std::unique_ptr<NavigationAgentStructuralCandidate> agents = {}) noexcept
                : world_(world), coordinator_(coordinator), activation_(activation), agents_(std::move(agents)) {}

            [[nodiscard]] Result<void> ValidatePublication() const override {
                if (!agents_)
                    return Result<void>::Success();
                if (activation_.IsValid()) {
                    const auto active = world_.ActiveDescriptor();
                    if (active.HasError())
                        return Result<void>::Failure(active.ErrorValue());
                    if (active.Value() != activation_ || world_.State() != NavigationWorldLifecycleState::Active)
                        return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
                }
                return agents_->ValidatePublication();
            }

            void Publish() noexcept override {
                if (!agents_)
                    return;
                coordinator_.RetireAgentQueriesAtSafePoint(agents_->RetiredHandles());
                agents_->Publish();
            }

            [[nodiscard]] Result<void> AfterPublication() override {
                return Result<void>::Success();
            }

        private:
            NavigationWorldLifecycle &world_;
            NavigationCoordinator &coordinator_;
            NavigationWorldActivationDescriptor activation_;
            std::unique_ptr<NavigationAgentStructuralCandidate> agents_;
        };

        /** @brief Stages typed agent-only changes alongside Scene, AI and other prefab owners. */
        class NavigationStructuralParticipant final : public Runtime::SceneStructuralParticipant {
        public:
            NavigationStructuralParticipant(NavigationAgentRegistry &registry, NavigationWorldLifecycle &world,
                                            NavigationCoordinator &coordinator,
                                            NavigationSceneActivationParticipant::StructuralAdmission admission)
                : registry_(registry), world_(world), coordinator_(coordinator), admission_(std::move(admission)) {}

            [[nodiscard]] Runtime::SceneStructuralOwner Owner() const noexcept override {
                return Runtime::SceneStructuralOwner::Navigation;
            }

            [[nodiscard]] Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(
                const Runtime::RuntimeSceneView &scene, std::span<const Runtime::RuntimeEntityView> created,
                std::span<const Runtime::EntityRef> destroyed) override {
                using Candidate = Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>;
                try {
                    const auto binding = MakeBinding(scene);
                    if (binding.HasError())
                        return Candidate::Failure(binding.ErrorValue());
                    std::vector<NavigationAgentDescriptor> additions;
                    for (const auto &entity : created) {
                        const auto &component = entity.components->navigationAgent;
                        if (component && component->enabled)
                            additions.push_back(MakeDescriptor(binding.Value(), entity.entity, *component));
                    }
                    std::vector<NavigationAgentOwner> removals;
                    for (const auto entity : destroyed) {
                        const auto old = scene.Get(entity);
                        if (old.HasError())
                            return Candidate::Failure(old.ErrorValue());
                        const auto &component = old.Value().components->navigationAgent;
                        if (component && component->enabled)
                            removals.push_back({binding.Value().scene, entity.entity.index, entity.entity.generation});
                    }
                    if (additions.empty() && removals.empty())
                        return Candidate::Success(std::make_unique<NavigationStructuralCandidate>(world_, coordinator_));
                    return PrepareAgents(binding.Value(), additions, removals);
                } catch (const std::bad_alloc &) {
                    return Candidate::Failure(MakeError(NavigationErrors::AgentRegistryCapacityExceeded));
                }
            }

        private:
            /** @brief Checks provider availability and exact identity before applying host admission policy. */
            [[nodiscard]] Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> PrepareAgents(
                const NavigationAgentSceneBinding binding, const std::span<const NavigationAgentDescriptor> additions,
                const std::span<const NavigationAgentOwner> removals) {
                using Candidate = Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>;
                NavigationWorldActivationDescriptor activation;
                if (!additions.empty()) {
                    const auto admitted = AdmitAdditions(binding, additions);
                    if (admitted.HasError())
                        return Candidate::Failure(admitted.ErrorValue());
                    activation = admitted.Value();
                }
                auto staged = registry_.PrepareStructuralChanges(binding, additions, removals);
                if (staged.HasError())
                    return Candidate::Failure(staged.ErrorValue());
                return Candidate::Success(
                    std::make_unique<NavigationStructuralCandidate>(world_, coordinator_, activation, std::move(staged).Value()));
            }

            /** @brief Additions need a live topology; exact owner retirement remains possible while it is paused or unloaded. */
            [[nodiscard]] Result<NavigationWorldActivationDescriptor> AdmitAdditions(
                const NavigationAgentSceneBinding binding, const std::span<const NavigationAgentDescriptor> additions) const {
                using Admission = Result<NavigationWorldActivationDescriptor>;
                const auto lease = world_.Acquire(binding.world);
                if (lease.HasError())
                    return Admission::Failure(lease.ErrorValue());
                const auto activation = lease.Value().Descriptor();
                if (activation.scene != binding.scene || activation.sceneGeneration != binding.sceneGeneration)
                    return Admission::Failure(MakeError(NavigationErrors::StaleSnapshot));
                if (!admission_ || QueryNavigationCapability(lease.Value().Backend().Capabilities(),
                                                             NavigationCapability::GroundedQueries) != NavigationSupport::Available)
                    return Admission::Failure(MakeError(NavigationErrors::CapabilityUnavailable));
                for (const auto &agent : additions)
                    if (auto admitted = admission_(agent); admitted.HasError())
                        return Admission::Failure(admitted.ErrorValue());
                return Admission::Success(activation);
            }

            NavigationAgentRegistry &registry_;
            NavigationWorldLifecycle &world_;
            NavigationCoordinator &coordinator_;
            NavigationSceneActivationParticipant::StructuralAdmission admission_;
        };
    }  // namespace

    /** @copydoc NavigationSceneActivationParticipant::Prepare */
    Result<std::unique_ptr<Runtime::SceneActivationCandidate>> NavigationSceneActivationParticipant::Prepare(
        const Runtime::RuntimeSceneDefinition &definition, const Runtime::RuntimeSceneView &scene) {
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
                descriptors.push_back(MakeDescriptor(binding.Value(), *runtimeEntity, component));
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

    /** @copydoc NavigationSceneActivationParticipant::MakeStructuralParticipant */
    std::unique_ptr<Runtime::SceneStructuralParticipant> NavigationSceneActivationParticipant::MakeStructuralParticipant(
        NavigationWorldLifecycle &world, NavigationCoordinator &coordinator, StructuralAdmission admission) const {
        return std::make_unique<NavigationStructuralParticipant>(*registry_, world, coordinator, std::move(admission));
    }
}  // namespace Horo::Navigation
