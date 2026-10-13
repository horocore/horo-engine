#pragma once

#include "Horo/Navigation/NavigationSceneActivation.h"
#include "Horo/Navigation/NavigationSceneAgentLifecycle.h"
#include "navigation/NavigationGroundedProviderFixture.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace Horo::Navigation::SceneLifecycleTestSupport {
    using namespace TestSupport;

    /** @brief Bounded worker gate deliberately retaining a real provider until explicit release, not cancellation. */
    struct ProviderFence final {
        std::mutex mutex;
        std::condition_variable condition;
        bool held{};
        bool entered{};
        std::atomic<unsigned> destructions{};
        std::atomic<bool> reachable{};

        bool WaitEntered() {
            std::unique_lock lock{mutex};
            return condition.wait_for(lock, std::chrono::seconds{3}, [&] {
                return entered;
            });
        }

        void Release() {
            std::lock_guard lock{mutex};
            held = false;
            condition.notify_all();
        }
    };

    /** @brief Instrumented ownership wrapper; all geometry and corridors come from the actual Recast provider. */
    class FencedProvider final : public INavigationQueryBackend {
    public:
        FencedProvider(std::unique_ptr<INavigationQueryBackend> provider, std::shared_ptr<ProviderFence> fence)
            : provider_(std::move(provider)), fence_(std::move(fence)) {}

        ~FencedProvider() override {
            provider_.reset();
            ++fence_->destructions;
        }

        NavigationProviderCapabilities Capabilities() const noexcept override {
            return provider_->Capabilities();
        }

        Result<NavigationPath> FindPath(const NavigationPathRequest &request, const CancellationToken &) const override {
            {
                std::unique_lock lock{fence_->mutex};
                fence_->entered = true;
                fence_->condition.notify_all();
                if (!fence_->condition.wait_for(lock, std::chrono::seconds{3}, [&] {
                    return !fence_->held;
                }))
                    return Result<NavigationPath>::Failure(MakeError(NavigationErrors::QueryCancelled));
            }
            // Deliberately finish valid real work even after owner revocation: publication must reject this late success.
            CancellationSource independent;
            auto result = provider_->FindPath(request, independent.Token());
            fence_->reachable =
                result.HasValue() && result.Value().status == NavigationPathStatus::Reachable && !result.Value().corridor.empty();
            return result;
        }

    private:
        std::unique_ptr<INavigationQueryBackend> provider_;
        std::shared_ptr<ProviderFence> fence_;
    };

    struct Fixture final {
        std::shared_ptr<ProviderFence> fence{std::make_shared<ProviderFence>()};
        CancellationSource cancellation;
        JobSystem jobs;
        NavigationAgentRegistry registry{std::move(NavigationAgentRegistry::Create({.maximumAgents = 2})).Value()};
        Runtime::RuntimeSceneService scene;
        NavigationWorldLifecycle world{std::move(NavigationWorldLifecycle::Create(4)).Value()};
        NavigationCoordinator coordinator{std::move(NavigationCoordinator::Create(jobs, {})).Value()};
        NavigationAgentSceneBinding binding;
        NavigationWorldActivationDescriptor activation;
        Runtime::EntityRef entity;
        Runtime::EntityRef target;
        CrowdAgentHandle agent;
        NavigationPathCaller caller{NavigationDynamicOwnerId::Create(1).Value(), NavigationDynamicOwnerGeneration::Create(1).Value()};
        std::unique_ptr<NavigationSceneAgentLifecycle> lifecycle;
        std::unique_ptr<NavigationAgentSceneCandidate> replacement;

        explicit Fixture(const unsigned workers = 0, const bool holdProvider = false) : jobs{{.workerCount = workers}} {
            fence->held = holdProvider;
            REQUIRE(scene.AddActivationParticipant(std::make_unique<NavigationSceneActivationParticipant>(registry)).HasValue());
            REQUIRE(scene.Startup(cancellation.Token()).HasValue());
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{4}, Runtime::SceneDefinitionRevision{1}};
            Runtime::RuntimeEntityDefinition definition{.object = Runtime::SceneObjectId{101}};
            definition.components.navigationAgent =
                Runtime::NavigationAgentComponent{.profile = NavigationAgentProfileId::Create(1).Value(),
                                                  .filter = NavigationFilterId::Create(1).Value()};
            builder.Add(std::move(definition));
            builder.Add(Runtime::RuntimeEntityDefinition{.object = Runtime::SceneObjectId{202}});
            REQUIRE(scene.QueuePreparation(std::move(builder).Build().Value()).HasValue());
            CommitScene();
            binding = registry.ActiveBinding().Value();
            activation = {binding.scene, binding.sceneGeneration, binding.world, Topology()};
            entity = *scene.ActiveScene()->Find(Runtime::SceneObjectId{101});
            target = *scene.ActiveScene()->Find(Runtime::SceneObjectId{202});
            agent = registry.Snapshot().Value().Agents().front().handle;
            REQUIRE(world.Stage(activation, std::make_unique<FencedProvider>(MakeGroundedProvider(binding.world), fence)).HasValue());
            REQUIRE(world.CommitAtSafePoint(binding.scene, binding.sceneGeneration).HasValue());
            auto created =
                NavigationSceneAgentLifecycle::Create(registry, world, coordinator, *scene.ActiveScene(), entity, agent, caller, 1);
            REQUIRE(created.HasValue());
            lifecycle = std::move(created).Value();
        }

        ~Fixture() {
            fence->Release();
            lifecycle.reset();
            jobs.Shutdown(ShutdownPolicy::Drain);
            static_cast<void>(coordinator.Commit({.tick = 100}));
        }

        void CommitScene() {
            const Runtime::FrameContext context{1, {}, 0.0, 0, {}, false, cancellation.Token()};
            REQUIRE(scene.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, context).HasValue());
            REQUIRE_FALSE(scene.TakeOperationError().has_value());
        }

        NavigationMovementObservation Now(const std::uint64_t tick = 0) const {
            return {.binding = binding,
                    .position = {1, 0, 1},
                    .stepSeconds = 0.1F,
                    .path = {.provenance = {NavigationSnapshotToken::Create(1).Value(), binding.world, activation.topology, 1, 1, 1, 1, 0},
                             .linkRevision = 1,
                             .goalRevision = 1,
                             .tick = tick}};
        }

        NavigationMovementRequest Move() const {
            return {.destination = {9, 0, 9}, .query = Request().requirement, .capabilityRevision = 1};
        }

        void StartQuery() {
            REQUIRE(lifecycle->Start(*scene.ActiveScene(), Move(), Now()).HasValue());
            REQUIRE(lifecycle->Update(*scene.ActiveScene(), Now()).Value().state == NavigationMovementState::Querying);
        }

        void PublishRoute() {
            StartQuery();
            REQUIRE(coordinator.Dispatch(0) == 1);
            const std::array callers{caller};
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
            std::uint32_t published = 0;
            while (!published && std::chrono::steady_clock::now() < deadline) {
                published = coordinator.Commit({activation, Now().path.provenance, callers, 1});
                std::this_thread::yield();
            }
            REQUIRE(published == 1);
        }

        void DestroyEntity() {
            Destroy(entity);
        }

        void Destroy(const Runtime::EntityRef destroyed) {
            Runtime::SceneCommandBuffer commands;
            commands.Destroy(destroyed);
            REQUIRE(scene.QueueStructuralCommands(std::move(commands)).HasValue());
            CommitScene();
            REQUIRE(scene.ActiveScene()->Get(destroyed).HasError());
        }

        void Reload(const bool replaceWorld = true) {
            const auto next = NavigationAgentSceneBinding{replaceWorld ? World(2) : binding.world, binding.scene, binding.sceneGeneration};
            if (replaceWorld) {
                activation = {next.scene, next.sceneGeneration, next.world, Topology(10)};
                REQUIRE(world.Stage(activation, MakeGroundedProvider(next.world, activation.topology)).HasValue());
                REQUIRE(world.CommitAtSafePoint(next.scene, next.sceneGeneration).HasValue());
                const NavigationAgentDescriptor descriptor{.owner = {next.scene, entity.entity.index, entity.entity.generation},
                                                           .profile = NavigationAgentProfileId::Create(2).Value(),
                                                           .filter = NavigationFilterId::Create(1).Value(),
                                                           .radiusOverride = 0.25F};
                replacement = std::move(registry.PrepareScene(next, std::array{descriptor})).Value();
                replacement->Publish();
            } else {
                REQUIRE(registry.UnregisterAtSafePoint(agent).HasValue());
                const NavigationAgentDescriptor descriptor{.owner = {next.scene, entity.entity.index, entity.entity.generation},
                                                           .profile = NavigationAgentProfileId::Create(2).Value(),
                                                           .filter = NavigationFilterId::Create(1).Value(),
                                                           .radiusOverride = 0.25F};
                REQUIRE(registry.RegisterAtSafePoint(descriptor).HasValue());
            }
            binding = next;
            agent = registry.Snapshot().Value().Agents().front().handle;
            caller.generation = NavigationDynamicOwnerGeneration::Create(caller.generation.Value() + 1).Value();
        }
    };
}  // namespace Horo::Navigation::SceneLifecycleTestSupport
