#pragma once

#include "Horo/Navigation/NavigationSceneActivation.h"
#include "Horo/Navigation/NavigationSceneAgentLifecycle.h"
#include "navigation/NavigationGroundedProviderFixture.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

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
                NavigationSceneAgentLifecycle::Create(registry, world, coordinator, *scene.ActiveScene(), {entity, agent, caller, 1});
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

        /** @brief Accept and issue the same exact live Follow goal; return detached acceptance and observation values. */
        std::pair<NavigationMovementAcceptance, NavigationMovementObservation> BeginFollow() {
            auto request = Move();
            request.mode = NavigationMovementMode::Follow;
            request.entity = NavigationAgentOwner{binding.scene, target.entity.index, target.entity.generation};
            auto current = Now();
            current.target = NavigationMovementTarget{*request.entity, {9, 0, 9}, 1};
            const auto accepted = lifecycle->Start(*scene.ActiveScene(), request, current);
            REQUIRE(accepted.HasValue());
            REQUIRE(lifecycle->Update(*scene.ActiveScene(), current).HasValue());
            return {accepted.Value(), current};
        }

        /** @brief Observe the actual worker terminal fence and publish one exact current query result. */
        void PublishCompletedQuery(const NavigationMovementObservation &current, const std::uint64_t tick) {
            jobs.Shutdown(ShutdownPolicy::Drain);
            const std::array callers{caller};
            REQUIRE(coordinator.Commit({activation, current.path.provenance, callers, tick}) == 1);
        }

        /** @brief Assert physical pin ownership while the deliberately held provider is still executing. */
        void RequireRetainedProvider() const {
            REQUIRE_FALSE(coordinator.IsDrained());
            REQUIRE(world.RetiredCount() == 1);
            REQUIRE(fence->destructions == 0);
        }

        /** @brief Release this fixture's real running provider, then observe terminal work before reading worker-owned result facts. */
        void FinishHeldProvider() {
            fence->Release();
            jobs.Shutdown(ShutdownPolicy::Drain);
            REQUIRE(fence->reachable);
        }

        /** @brief Reconcile a still-live dormant owner without Update/Commit, retaining both drain and live-agent assertions. */
        void ReconcileLiveRetirement() {
            REQUIRE(lifecycle->ReconcileAtSafePoint(scene.ActiveScene()).HasValue());
            REQUIRE(coordinator.IsDrained());
            REQUIRE(registry.Find(agent).HasValue());
        }

        /** @brief Drain a revoked world through the existing coordinator commit, preserving all physical-release assertions. */
        void DrainRevokedWorld(const std::uint64_t tick) {
            REQUIRE(world.Unload(binding.world).HasValue());
            jobs.Shutdown(ShutdownPolicy::Drain);
            REQUIRE(coordinator.Commit({.tick = tick}) == 0);
            REQUIRE(coordinator.IsDrained());
            REQUIRE(world.CollectRetired() == NavigationWorldLifecycleState::Empty);
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
            const NavigationAgentDescriptor descriptor{.owner = {next.scene, entity.entity.index, entity.entity.generation},
                                                       .profile = NavigationAgentProfileId::Create(2).Value(),
                                                       .filter = NavigationFilterId::Create(1).Value(),
                                                       .radiusOverride = 0.25F};
            if (replaceWorld) {
                activation = {next.scene, next.sceneGeneration, next.world, Topology(10)};
                REQUIRE(world.Stage(activation, MakeGroundedProvider(next.world, activation.topology)).HasValue());
                REQUIRE(world.CommitAtSafePoint(next.scene, next.sceneGeneration).HasValue());
                replacement = std::move(registry.PrepareScene(next, std::array{descriptor})).Value();
                replacement->Publish();
            } else {
                REQUIRE(registry.UnregisterAtSafePoint(agent).HasValue());
                REQUIRE(registry.RegisterAtSafePoint(descriptor).HasValue());
            }
            binding = next;
            agent = registry.Snapshot().Value().Agents().front().handle;
            caller.generation = NavigationDynamicOwnerGeneration::Create(caller.generation.Value() + 1).Value();
        }
    };
}  // namespace Horo::Navigation::SceneLifecycleTestSupport
