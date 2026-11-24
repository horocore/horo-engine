#include "Horo/Gameplay/GameplayFormationMovement.h"
#include "Horo/Navigation/Backends/RecastDetourCrowdProvider.h"
#include "navigation/NavigationCrowdScaleFixture.h"
#include "navigation/NavigationRuntimeQualificationFixture.h"

#include <cmath>

namespace Horo::Navigation {
    namespace {
        using namespace CrowdQualification;

        /** @brief One production movement and Character adapter chain retained across group retirement. */
        struct FormationActor final {
            Character::CharacterControllerHandle controller;
            Character::CharacterCapability grant;
            std::unique_ptr<NavigationMovementController> movement;
            std::unique_ptr<NavigationVelocityPipeline> velocity;
            NavigationCharacterAdapter bridge;
            std::shared_ptr<Gameplay::GameplayNavigationContext> gameplay;
            NavigationMovementObservation observation;
            NavigationCharacterMotion submittedMotion;
        };

        /** @brief Build a CrowdSmall-sized Character owner sharing the native Physics query fixture. */
        std::unique_ptr<Character::CharacterWorld> PrepareFormationCharacters(const Qualification::Host &host) {
            Character::CharacterWorldSettingsDescriptor limits;
            limits.capacities.maximumControllers = CrowdAgents;
            limits.capacities.maximumQueuedCommands = CrowdAgents;
            limits.work.maximumCommandsPerTick = CrowdAgents;
            auto world = Character::CharacterWorld::Prepare({host.activation.sceneGeneration.Value(), host.physics->Identity(), 1, 1},
                                                            Character::CharacterWorldSettings::Capture(limits).Value())
                             .Value();
            REQUIRE(world->RefreshPhysicsSnapshot(host.physics->Identity(), host.physics->PublishedTick().publicationRevision).HasValue());
            return world;
        }

        /** @brief Bind a registered member to an independently revocable gameplay and Character chain. */
        FormationActor PrepareFormationActor(Qualification::Host &host, CrowdScene &scene, Character::CharacterWorld &characters,
                                             const NavigationAgentRecord &agent, const Character::CharacterControllerHandle controller) {
            FormationActor actor;
            const auto slot = agent.owner.entityIndex - 1;
            actor.controller = controller;
            actor.observation = host.Now(0);
            actor.observation.position = characters.ControllerTransform(controller).Value().position;
            const NavigationPathCaller caller{Id<NavigationDynamicOwnerId>(slot + 1), Id<NavigationDynamicOwnerGeneration>(1)};
            actor.movement =
                NavigationMovementController::Create(scene.agents, host.navigation, *host.coordinator, agent.handle, caller).Value();
            actor.velocity = NavigationVelocityPipeline::Create(*actor.movement, scene.binding, agent.handle, 1).Value();
            actor.grant = characters.IssueCapability().Value();
            actor.bridge = NavigationCharacterAdapter::Create(*actor.velocity, actor.grant,
                                                              {{1, 0, CharacterInput::DesiredMotionSource::ExternalIntent, true},
                                                               actor.controller,
                                                               scene.binding,
                                                               1})
                               .Value();
            const NavigationPathSubmission policy{.caller = caller, .request = TestSupport::Request(), .capabilityRevision = 1};
            actor.gameplay = Gameplay::GameplayNavigationContext::Create({"game.formation", scene.binding, false, true}, &host.navigation,
                                                                         host.coordinator.get(), actor.movement.get(), policy)
                                 .Value();
            return actor;
        }

        /** @brief Queue an exact Character command and keep its feedback registration pending during retirement. */
        void SubmitFormationMotion(FormationActor &actor, const NavigationCrowdSnapshot &capture, const std::size_t index,
                                   INavigationCrowdBackend &solver) {
            actor.observation.path.tick = capture.CaptureTick();
            const auto work = actor.velocity->Prepare(actor.observation, capture,
                                                      {.agentIndex = index,
                                                       .maximumSpeedMetersPerSecond = MaximumSpeed,
                                                       .maximumAccelerationMetersPerSecondSquared = 20,
                                                       .stepSeconds = FixedStep});
            REQUIRE(work.HasValue());
            REQUIRE(work.Value().preferred.state == NavigationMovementState::Querying);
            const auto outcome = solver.Solve(capture, work.Value().request);
            REQUIRE(outcome.HasValue());
            const auto publication = actor.velocity->Commit(work.Value(), outcome.Value(), capture, actor.observation);
            REQUIRE(publication.HasValue());
            actor.submittedMotion = actor.bridge.Prepare(publication.Value(), actor.observation, 1).Value();
            const auto submitted = actor.bridge.Submit(actor.submittedMotion, actor.observation);
            REQUIRE(submitted.HasValue());
            REQUIRE(submitted.Value().admission.status == Character::CharacterCommandAdmissionStatus::Deferred);
            CHECK(submitted.Value().admission.pendingCommands == index + 1);
        }

        /** @brief Create the full non-overlapping floor roster before publishing the Character owner. */
        std::array<Character::CharacterControllerHandle, CrowdAgents> ActivateFormationControllers(const Qualification::Host &host,
                                                                                                   Character::CharacterWorld &characters) {
            std::array<Character::CharacterControllerHandle, CrowdAgents> controllers;
            for (std::size_t index = 0; index < controllers.size(); ++index) {
                auto descriptor = host.Controller();
                descriptor.characterWorld = characters.Descriptor().identity;
                descriptor.collisionRootPosition = {1.0F + static_cast<float>(index % 8), Qualification::RootHeight,
                                                    1.0F + static_cast<float>(index / 8)};
                controllers[index] = characters.CreateController(descriptor).Value();
            }
            REQUIRE(characters.Activate().HasValue());
            return controllers;
        }

        /** @brief Holds the live actors and the detached values for admitting one formation revision. */
        struct FormationRoster final {
            std::vector<FormationActor> actors;
            std::array<Gameplay::GameplayFormationMovementBinding, CrowdAgents> bindings;
            std::array<NavigationCrowdMotionSample, CrowdAgents> samples;
            Gameplay::GameplayFormationResolution formation;
        };

        /** @brief Spawn Characters and capture each exact agent, grant, root and formation goal. */
        FormationRoster PrepareFormationRoster(Qualification::Host &host, CrowdScene &scene, Character::CharacterWorld &characters,
                                               const NavigationAgentSnapshot &agents) {
            const auto controllers = ActivateFormationControllers(host, characters);
            Character::CharacterPhysicsQueryAdapter physicsQueries(*host.physics);
            const auto &world = characters.Descriptor();
            const Character::CharacterPhysicsQueryExpectations queries{world.sceneGeneration,        world.identity,
                                                                       world.physicsWorld,           world.collisionFilterGeneration,
                                                                       world.originGeneration,       0,
                                                                       world.physicsSnapshotRevision};
            FormationRoster roster{.formation = {.group = Gameplay::GameplayFormationId::Create(1).Value(),
                                                 .revision = 1,
                                                 .source = host.activation,
                                                 .availability = Gameplay::GameplayFormationAvailability::Complete}};
            roster.actors.reserve(CrowdAgents);
            roster.formation.goals.reserve(CrowdAgents);
            for (std::size_t index = 0; index < controllers.size(); ++index) {
                const auto &controller = controllers[index];
                const auto &agent = agents.Agents()[index];
                REQUIRE(characters.SpawnController(controller, physicsQueries.Context(queries)).HasValue());
                const auto &actor = roster.actors.emplace_back(PrepareFormationActor(host, scene, characters, agent, controller));
                REQUIRE(actor.gameplay->Refresh(actor.observation).HasValue());
                roster.bindings[index] = {agent.handle, actor.gameplay.get()};
                roster.samples[index] = {.handle = agent.handle, .position = actor.observation.position};
                const auto target = actor.observation.position + Math::Vec3{0.75F, 0, 0};
                roster.formation.goals.push_back({static_cast<std::uint32_t>(index), agent.handle, {target.x, 0, target.z}, target});
            }
            return roster;
        }

        /** @brief Observe group cancellation and withdraw the actor's exact Character and movement authority. */
        void RequireRetiredFormationActor(FormationActor &actor) {
            actor.observation.path.tick = 2;
            const auto cancelled = actor.movement->Update(actor.observation);
            REQUIRE(cancelled.HasValue());
            CHECK(cancelled.Value().state == NavigationMovementState::Cancelled);
            actor.bridge.Shutdown();
            CHECK(actor.grant.Validate().HasError());
            CHECK(actor.bridge.ObserveFeedback().HasError());
            CHECK(actor.bridge.Submit(actor.submittedMotion, actor.observation).HasError());
            actor.gameplay->Revoke();
            CHECK_FALSE(actor.gameplay->IsUsable());
            actor.velocity->Retire();
            actor.movement->BeginShutdown();
            actor.movement->CollectRetiredAtSafePoint();
        }

        /** @brief Close the host, then collect actor retirement handles released by worker join. */
        void RequireFormationDrained(Qualification::Host &host, CrowdScene &scene, Character::CharacterWorld &characters,
                                     const std::span<const FormationActor> actors) {
            characters.Shutdown();
            CHECK(characters.ActiveControllerCount() == 0);
            CHECK(characters.TickStatistics().pendingCommands == 0);
            scene.Close();
            host.Close();
            REQUIRE(host.coordinator->IsDrained());
            for (std::size_t index = 0; index < actors.size(); ++index) {
                CAPTURE(index);
                actors[index].movement->CollectRetiredAtSafePoint();
                CHECK(actors[index].movement->IsDrained());
            }
            REQUIRE(host.navigation.CollectRetired() == NavigationWorldLifecycleState::Closed);
            CHECK(host.navigation.RetiredCount() == 0);
        }

        /** @brief Owns a canonical Physics and Character world for one complete CrowdSmall formation. */
        class CrowdCharacters final {
        public:
            explicit CrowdCharacters(const CrowdScene &scene, const NavigationAgentSnapshot &agents)
                : sceneGeneration_(scene.activation.sceneGeneration.Value()),
                  runtime_(Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical).Value()),
                  physics_(runtime_->PrepareWorld(Qualification::PhysicsSettings()).Value()) {
                REQUIRE(physics_->Activate(runtime_->IssueWorldIdentity().Value()).HasValue());
                Character::CharacterWorldSettingsDescriptor limits;
                limits.capacities.maximumControllers = CrowdAgents;
                limits.capacities.maximumQueuedCommands = CrowdAgents;
                limits.work.maximumCommandsPerTick = CrowdAgents;
                character_ = Character::CharacterWorld::Prepare({sceneGeneration_, physics_->Identity(), 1, 1},
                                                                Character::CharacterWorldSettings::Capture(limits).Value())
                                 .Value();
                auto descriptor = Qualification::ReferenceController(*character_);
                Qualification::AddReferenceShape(*physics_, descriptor, Physics::PhysicsStaticPlaneShape{{0, 1, 0}, 0}, {});
                AdvancePhysics();
                for (std::size_t index = 0; index < controllers_.size(); ++index) {
                    descriptor.collisionRootPosition = Sample(agents.Agents()[index], Layout::Formation, 1).position;
                    descriptor.collisionRootPosition.y = Qualification::RootHeight;
                    controllers_[index] = character_->CreateController(descriptor).Value();
                }
                REQUIRE(character_->Activate().HasValue());
                Character::CharacterPhysicsQueryAdapter queries(*physics_);
                const auto spawn = queries.Context(Qualification::ReferenceExpectations(*character_, 0));
                for (const auto &controller : controllers_)
                    REQUIRE(character_->SpawnController(controller, spawn).HasValue());
                grant_ = character_->IssueCapability().Value();
            }

            /** @brief Creates solver input from actual committed Character roots and velocities. */
            std::array<NavigationCrowdMotionSample, CrowdAgents> Motions(const NavigationAgentSnapshot &agents) const {
                std::array<NavigationCrowdMotionSample, CrowdAgents> motions{};
                for (std::size_t index = 0; index < motions.size(); ++index) {
                    const auto position = character_->ControllerTransform(controllers_[index]);
                    REQUIRE(position.HasValue());
                    const auto movement = character_->ControllerLocomotionSnapshot(controllers_[index]);
                    motions[index] = {.handle = agents.Agents()[index].handle,
                                      .position = position.Value().position,
                                      .velocity =
                                          movement.HasValue() ? movement.Value().movement.achievedVelocityMetersPerSecond : Math::Vec3{}};
                }
                return motions;
            }

            /** @brief Submits every native solve output and observes the actual command-consumption boundary. */
            void Advance(const std::uint64_t tick, const std::span<const NavigationCrowdBatchResult> results) {
                REQUIRE(results.size() == controllers_.size());
                for (const auto &[index, outcome] : results)
                    Queue(index, tick, outcome.desiredVelocity);
                AdvancePhysics();
                Character::CharacterPhysicsQueryAdapter queries(*physics_);
                std::size_t callbacks{};
                REQUIRE(character_
                            ->AdvanceFixedTick({.tick = tick,
                                                .sceneGeneration = sceneGeneration_,
                                                .fixedDelta = Quantum,
                                                .observer = Observer(callbacks),
                                                .query = queries.Context(Qualification::ReferenceExpectations(*character_, tick))})
                            .HasValue());
                CHECK(callbacks == controllers_.size());
                CHECK(character_->PublishedTick().appliedCommands == controllers_.size());
            }

            /** @brief Retires pending commands without invoking an owner-phase listener. */
            void Close() {
                const auto last = character_->PublishedTick();
                for (std::size_t index = 0; index < controllers_.size(); ++index)
                    Queue(index, last.completedTick + 1, {MaximumSpeed, 0, 0});
                grant_.Revoke();
                character_->Shutdown();
                std::size_t callbacks{};
                CHECK(character_
                          ->AdvanceFixedTick({.tick = last.completedTick + 1,
                                              .sceneGeneration = sceneGeneration_,
                                              .fixedDelta = Quantum,
                                              .observer = Observer(callbacks)})
                          .HasError());
                CHECK(callbacks == 0);
                CHECK(character_->ActiveControllerCount() == 0);
                CHECK(grant_.Validate().HasError());
                physics_->Shutdown();
                CHECK(physics_->State() == Physics::PhysicsWorldState::Destroyed);
            }

        private:
            static constexpr auto Quantum = Duration::FromNanoseconds(16'666'667);

            static Character::CharacterTickObserver Observer(std::size_t &callbacks) noexcept {
                return {.context = &callbacks, .movement = [](void *state, const Character::CharacterMovementRequest &) noexcept {
                    ++*static_cast<std::size_t *>(state);
                }};
            }

            void AdvancePhysics() {
                REQUIRE(
                    physics_
                        ->AdvanceFixedTick({.simulationTick = ++physicsTick_, .sceneGeneration = sceneGeneration_, .fixedDelta = Quantum})
                        .HasValue());
                REQUIRE(character_->RefreshPhysicsSnapshot(physics_->Identity(), physics_->PublishedTick().publicationRevision).HasValue());
            }

            void Queue(const std::size_t index, const std::uint64_t tick, const Math::Vec3 velocity) {
                const auto admitted = grant_.QueueMovementCommand(
                    {.controller = controllers_[index], .tick = tick, .sequence = tick, .desiredVelocityMetersPerSecond = velocity});
                REQUIRE(admitted.HasValue());
                REQUIRE(admitted.Value().status == Character::CharacterCommandAdmissionStatus::Deferred);
                CHECK(admitted.Value().pendingCommands <= CrowdAgents);
            }

            const std::uint64_t sceneGeneration_;
            std::unique_ptr<Physics::PhysicsRuntime> runtime_;
            std::unique_ptr<Physics::PhysicsWorld> physics_;
            std::unique_ptr<Character::CharacterWorld> character_;
            std::array<Character::CharacterControllerHandle, CrowdAgents> controllers_{};
            Character::CharacterCapability grant_;
            std::uint64_t physicsTick_{};
        };
    }  // namespace

    TEST_CASE("CrowdSmall retirement revokes every formation move and pending Character command",
              "[integration][navigation][crowd][scale]") {
        using namespace Gameplay;
        Qualification::Host host;
        CrowdScene scene;
        scene.CommitOverlay(Layout::Formation);
        const auto agents = scene.agents.Snapshot().Value();
        REQUIRE(agents.Agents().size() == CrowdAgents);
        auto characters = PrepareFormationCharacters(host);
        auto roster = PrepareFormationRoster(host, scene, *characters, agents);
        GameplayFormationMovement movement{roster.formation.group};
        const auto admitted = movement.Apply(roster.formation, 1, host.navigation, roster.bindings);
        REQUIRE(admitted.HasValue());
        REQUIRE(admitted.Value().started.size() == CrowdAgents);
        REQUIRE(admitted.Value().availability == GameplayFormationAvailability::Complete);
        const auto capture = BuildCapture(scene, agents, roster.samples, 1);
        for (std::size_t index = 0; index < roster.actors.size(); ++index)
            SubmitFormationMotion(roster.actors[index], capture, index, *host.crowd);
        REQUIRE(characters->TickStatistics().pendingCommands == CrowdAgents);
        REQUIRE(host.coordinator->Dispatch(1) > 0);

        movement.Retire();
        auto next = roster.formation;
        next.revision = 2;
        REQUIRE(movement.Apply(next, 2, host.navigation, roster.bindings).HasError());
        for (auto &actor : roster.actors)
            RequireRetiredFormationActor(actor);
        RequireFormationDrained(host, scene, *characters, roster.actors);
    }

    TEST_CASE("CrowdSmall formation advances and retires 64 native Character controllers", "[integration][navigation][crowd][scale]") {
        CrowdScene scene;
        scene.CommitOverlay(Layout::Formation);
        const auto agents = scene.agents.Snapshot().Value();
        CrowdCharacters characters(scene, agents);
        const auto initial = characters.Motions(agents);
        const std::array<std::shared_ptr<INavigationCrowdBackend>, 1> providers{
            CreateRecastDetourCrowdBackend({FactsPerAgent, FactsPerAgent}).Value()};
        JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 1});
        for (std::uint64_t tick = 1; tick <= Steps; ++tick) {
            const auto capture = BuildCapture(scene, agents, characters.Motions(agents), tick);
            auto batch = NavigationCrowdBatch::Submit(jobs, capture, Requests(capture, Layout::Formation), providers);
            REQUIRE(batch.HasValue());
            std::uint64_t elapsed{};
            characters.Advance(tick, Collect(*batch.Value(), capture, elapsed));
        }
        const auto final = characters.Motions(agents);
        for (std::size_t index = 0; index < final.size(); ++index) {
            CHECK(final[index].position.x > initial[index].position.x + 0.05F);
            CHECK(std::abs(final[index].position.y - Qualification::RootHeight) < 0.001F);
            CHECK(std::abs(final[index].position.z - initial[index].position.z) < 0.3F);
        }
        jobs.Shutdown(ShutdownPolicy::Drain);
        CHECK(providers.front().use_count() == 1);
        characters.Close();
        scene.Close();
    }
}  // namespace Horo::Navigation
