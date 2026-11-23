#include "Horo/Gameplay/GameplayFormationMovement.h"
#include "Horo/Navigation/Backends/DeterministicCrowdProvider.h"
#include "Horo/Navigation/Backends/RecastDetourCrowdProvider.h"
#include "Horo/Navigation/NavigationCrowdBatch.h"
#include "Horo/Navigation/NavigationCrowdDiagnostics.h"
#include "navigation/NavigationRuntimeQualificationFixture.h"
#include "navigation/NavigationTestAssertions.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Navigation {
    namespace {
        using TestSupport::Activation;
        using TestSupport::Id;
        using TestSupport::RequireError;
        using Clock = std::chrono::steady_clock;

        // ADR-109 CrowdSmall and AvoidanceBalanced. Other bounds belong to this reference corpus.
        constexpr std::size_t CrowdAgents = 64;
        constexpr std::uint32_t FactsPerAgent = 16;
        constexpr std::uint64_t Steps = 32;
        constexpr float FixedStep = 1.0F / 60.0F;
        constexpr float MaximumSpeed = 2.0F;
        constexpr float Radius = 0.25F;

        enum class Layout {
            Crossings,
            Bottleneck,
            Formation,
            MixedPriorities
        };

        constexpr std::array Layouts{std::pair{Layout::Crossings, std::string_view{"crossings"}},
                                     std::pair{Layout::Bottleneck, std::string_view{"bottleneck"}},
                                     std::pair{Layout::Formation, std::string_view{"formation"}},
                                     std::pair{Layout::MixedPriorities, std::string_view{"mixed_priorities"}}};

        /** @brief Real logical registry and dynamic overlay retained for one isolated workload. */
        struct CrowdScene final {
            const NavigationWorldActivationDescriptor activation{Activation()};
            const NavigationAgentSceneBinding binding{activation.world, activation.scene, activation.sceneGeneration};
            NavigationAgentRegistry agents{NavigationAgentRegistry::Create({.maximumAgents = CrowdAgents}).Value()};
            NavigationDynamicRegistry dynamic{NavigationDynamicRegistry::Create().Value()};
            std::unique_ptr<NavigationAgentSceneCandidate> candidate;
            const NavigationAgentProfileId profile{Id<NavigationAgentProfileId>(1)};
            std::optional<CrowdAgentHandle> teleported;

            CrowdScene() {
                std::array<NavigationAgentDescriptor, CrowdAgents> descriptors;
                for (std::size_t slot = 0; slot < descriptors.size(); ++slot) {
                    descriptors[slot] = {.owner = {activation.scene, static_cast<std::uint32_t>(slot + 1), 1},
                                         .profile = profile,
                                         .filter = Id<NavigationFilterId>(1),
                                         .radiusOverride = Radius};
                }
                candidate = agents.PrepareScene(binding, descriptors).Value();
                candidate->Publish();
            }

            /** @brief Two physical-width obstacle faces bound the entrance to the central lane. */
            void CommitOverlay(const Layout layout) {
                if (layout == Layout::Bottleneck) {
                    for (const auto id : {1U, 2U}) {
                        const float side = id == 1 ? -1.0F : 1.0F;
                        REQUIRE(dynamic
                                    .StageRegisterObstacle({
                                        .id = Id<NavigationObstacleId>(id),
                                        .provenance = {activation.world, activation.scene, activation.sceneGeneration,
                                                       Id<NavigationDynamicOwnerId>(id), Id<NavigationDynamicOwnerGeneration>(1),
                                                       Id<NavigationDynamicSourceRevision>(1)},
                                        .shape = NavigationDynamicBoxShape{{0, 0, side * 3.4F}, {0.25F, 1, 1.15F}},
                                        .layers = {1},
                                        .updateTick = 1,
                                    })
                                    .HasValue());
                    }
                }
                REQUIRE(dynamic.CommitAtSafePoint(activation, 1).HasValue());
            }

            /** @brief Invalidates every registry identity after the solver jobs have drained. */
            void Close() {
                candidate->Shutdown();
                dynamic.BeginShutdown();
                CHECK(agents.ActiveBinding().HasError());
                CHECK(dynamic.IsShutdown());
            }
        };

        /** @brief Samples an authored fixed-tick arrangement without assuming a Character displacement result. */
        NavigationCrowdMotionSample Sample(const NavigationAgentRecord &agent, const Layout layout, const std::uint64_t tick) {
            using enum Layout;
            const auto slot = agent.owner.entityIndex - 1;
            const auto row = static_cast<float>(slot / 8) - 3.5F;
            const auto column = slot % 8;
            const auto travel = static_cast<float>(tick - 1) * FixedStep;
            if (layout == Formation)
                return {.handle = agent.handle, .position = {static_cast<float>(column) * 1.2F + travel, 0, row * 1.2F}};

            const float side = column < 4 ? -1.0F : 1.0F;
            const float spacing = layout == Bottleneck ? 0.8F : 2.4F;
            const float rightOfWay = side < 0 ? 0.9F : 0.1F;
            return {.handle = agent.handle,
                    .position = {side * (1.05F + static_cast<float>(column % 4) * 1.2F - travel), 0, row * spacing},
                    .avoidance = {.priority = layout == MixedPriorities ? rightOfWay : 0.5F}};
        }

        /** @brief Admits exact motion samples under the workload's bounded CrowdSmall profile. */
        NavigationCrowdSnapshot BuildCapture(const CrowdScene &scene, const NavigationAgentSnapshot &agents,
                                             std::span<const NavigationCrowdMotionSample> motions, const std::uint64_t tick,
                                             const NavigationCrowdQualification *qualified = nullptr) {
            const std::array profiles{NavigationCrowdProfileFacts{.profile = scene.profile,
                                                                  .radiusMeters = Radius,
                                                                  .neighborRadiusMeters = 1.8F,
                                                                  .maximumNeighbors = FactsPerAgent,
                                                                  .maximumBoundarySegments = FactsPerAgent}};
            NavigationCrowdSnapshotLimits limits{.maximumAgents = CrowdAgents,
                                                 .maximumBoundarySegments = FactsPerAgent,
                                                 .maximumCellEntries = CrowdAgents * 9,
                                                 .maximumNeighborFacts = CrowdAgents * FactsPerAgent,
                                                 .maximumBoundaryFacts = CrowdAgents * FactsPerAgent,
                                                 .maximumPairChecks = CrowdAgents * (CrowdAgents - 1),
                                                 .maximumBoundaryChecks = CrowdAgents * FactsPerAgent};
            if (qualified) {
                limits.mode = AvoidanceExecutionMode::DeterministicQualified;
                limits.deterministicStamp = {scene.activation.topology, qualified->topologyContent, qualified->profile};
            }
            auto result = BuildNavigationCrowdSnapshot(agents, scene.dynamic.Snapshot().Value(), motions, profiles, limits, tick);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        /** @brief Captures all live agents under the same bounded profile used at solver admission. */
        NavigationCrowdSnapshot Capture(const CrowdScene &scene, const Layout layout, const std::uint64_t tick, const bool reverse,
                                        const NavigationCrowdQualification *qualified = nullptr) {
            const auto agents = scene.agents.Snapshot().Value();
            std::vector<NavigationCrowdMotionSample> motions;
            motions.reserve(agents.Agents().size());
            for (const auto &agent : agents.Agents()) {
                auto motion = Sample(agent, layout, tick);
                if (scene.teleported == agent.handle)
                    motion.position.z += 24.0F;
                motions.push_back(motion);
            }
            if (reverse)
                std::ranges::reverse(motions);
            return BuildCapture(scene, agents, motions, tick, qualified);
        }

        /** @brief Requests one 60 Hz bounded solve per captured agent. */
        std::vector<NavigationAvoidanceRequest> Requests(const NavigationCrowdSnapshot &capture, const Layout layout) {
            std::vector<NavigationAvoidanceRequest> requests;
            requests.reserve(capture.Agents().size());
            for (const auto &agent : capture.Agents()) {
                const float direction = layout == Layout::Formation || agent.position.x < 0 ? 1.0F : -1.0F;
                requests.push_back({.agentIndex = requests.size(),
                                    .preferredVelocity = {direction * MaximumSpeed, 0, 0},
                                    .maximumSpeedMetersPerSecond = MaximumSpeed,
                                    .maximumAccelerationMetersPerSecondSquared = 20,
                                    .stepSeconds = FixedStep});
            }
            return requests;
        }

        /** @brief Checks the shared publishable outcome contract for the full population. */
        std::size_t CheckOutcomes(const NavigationCrowdSnapshot &capture, const std::span<const NavigationCrowdBatchResult> results) {
            REQUIRE(results.size() == CrowdAgents);
            const auto metrics = SummarizeNavigationCrowd(capture);
            CHECK(metrics.activeAgents == CrowdAgents);
            CHECK(metrics.neighborFacts <= CrowdAgents * FactsPerAgent);
            CHECK(capture.BoundarySegments().size() <= FactsPerAgent);
            CHECK(metrics.neighborsOmitted == 0);
            CHECK(metrics.boundariesOmitted == 0);
            std::size_t advancing{};
            for (std::size_t index = 0; index < results.size(); ++index) {
                const auto &[agent, outcome] = results[index];
                const auto velocity = outcome.desiredVelocity;
                CHECK(agent == capture.CellAgentIndices()[index]);
                CHECK(outcome.captureTick == capture.CaptureTick());
                CHECK(outcome.binding == capture.Binding());
                CHECK(velocity.y == 0);
                if (outcome.disposition == NavigationAvoidanceDisposition::CollisionSafeStop)
                    CHECK(velocity == Math::Vec3{});
                else if (velocity.x * velocity.x + velocity.z * velocity.z > 0)
                    ++advancing;
            }
            return advancing;
        }

        /** @brief Records exact ordered outcomes while leaving timing outside the replay digest. */
        void HashOutcomes(const std::span<const NavigationCrowdBatchResult> results, Sha256Builder &digest) {
            for (const auto &[agent, outcome] : results) {
                const auto velocity = outcome.desiredVelocity;
                const std::array words{static_cast<std::uint32_t>(agent), static_cast<std::uint32_t>(outcome.stopReason),
                                       std::bit_cast<std::uint32_t>(velocity.x), std::bit_cast<std::uint32_t>(velocity.z)};
                REQUIRE(digest.Update(std::as_bytes(std::span{words})));
            }
        }

        /** @brief Replay the same layout in the native qualified owner with independently ordered inputs. */
        Sha256Digest RunQualifiedReplay(const Layout layout, const bool reverse) {
            CrowdScene scene;
            scene.CommitOverlay(layout);
            constexpr std::string_view identity = "NAV-006.12 CrowdSmall reference replay";
            const auto fixture = ComputeSha256(std::as_bytes(std::span{identity.data(), identity.size()}));
            const NavigationCrowdQualification
                qualification{DeterministicCrowdProviderFingerprint(), fixture, fixture, fixture, fixture, FixedStep, CrowdAgents};
            auto provider = CreateDeterministicCrowdBackend(qualification).Value();
            Sha256Builder digest;
            std::size_t advanced{};
            for (std::uint64_t tick = 1; tick <= Steps; ++tick) {
                const auto capture = Capture(scene, layout, tick, reverse, &qualification);
                auto requests = Requests(capture, layout);
                if (reverse)
                    std::ranges::reverse(requests);
                const auto results =
                    NavigationCrowdBatch::RunQualified(capture, requests, *provider, qualification, scene.activation.topology);
                REQUIRE(results.HasValue());
                advanced += CheckOutcomes(capture, results.Value());
                HashOutcomes(results.Value(), digest);
            }
            CHECK(advanced > 0);
            scene.Close();
            return digest.Finalize();
        }

        /** @brief Joins one test-owned batch at a bounded fence while leaving the production collection path nonblocking. */
        std::vector<NavigationCrowdBatchResult> Collect(NavigationCrowdBatch &batch, const NavigationCrowdSnapshot &capture,
                                                        std::uint64_t &elapsed) {
            const auto deadline = Clock::now() + std::chrono::seconds{10};
            do {
                auto result = batch.TryTake(capture, capture.CaptureTick(), &elapsed);
                REQUIRE(result.HasValue());
                if (result.Value())
                    return *std::move(result).Value();
                std::this_thread::yield();
            } while (Clock::now() < deadline);
            FAIL("CrowdSmall batch failed to reach the test retirement fence");
            return {};
        }

        /** @brief Measures a single real Detour worker per tick, then drains all work. */
        void RunBestEffort(const Layout layout, const std::string_view name) {
            CrowdScene scene;
            scene.CommitOverlay(layout);
            const std::array<std::shared_ptr<INavigationCrowdBackend>, 1> providers{
                CreateRecastDetourCrowdBackend({FactsPerAgent, FactsPerAgent}).Value()};
            JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 1, .maxRetainedTerminalJobs = Steps});
            std::array<std::uint64_t, Steps> elapsed{};
            std::size_t advanced{};
            std::size_t maximumOwnerBytes{};
            for (std::uint64_t tick = 1; tick <= Steps; ++tick) {
                auto [capture, batch] = [&] {
                    Tests::AllocationProbe::ScopedMeasurement owner;
                    auto snapshot = Capture(scene, layout, tick, false);
                    auto submitted = NavigationCrowdBatch::Submit(jobs, snapshot, Requests(snapshot, layout), providers);
                    maximumOwnerBytes = std::max(maximumOwnerBytes, owner.Snapshot().requestedBytes);
                    return std::pair{std::move(snapshot), std::move(submitted)};
                }();
                REQUIRE(batch.HasValue());
                advanced += CheckOutcomes(capture, Collect(*batch.Value(), capture, elapsed[tick - 1]));
            }
            CHECK(advanced > 0);
            jobs.Shutdown(ShutdownPolicy::Drain);
            const auto journal = jobs.SnapshotIfChanged(0);
            REQUIRE(journal);
            REQUIRE(journal->jobs.size() == Steps);
            CHECK(std::ranges::all_of(journal->jobs, [](const auto &job) {
                return job.state == JobState::Succeeded;
            }));
            CHECK(providers.front().use_count() == 1);
            scene.Close();
            std::ranges::sort(elapsed);
            std::cout << "NAV-006.12 profile=CrowdSmall layout=" << name << " agents=" << CrowdAgents << " ticks=" << Steps
                      << " build=" << HORO_QUALIFICATION_BUILD_TYPE << " platform=" << HORO_QUALIFICATION_PLATFORM
                      << " compiler=" << HORO_QUALIFICATION_COMPILER << " solve_p50_ns=" << elapsed[elapsed.size() / 2]
                      << " solve_p95_ns=" << elapsed[(elapsed.size() - 1) * 95 / 100]
                      << " capture_submit_max_owner_cpp_requested_bytes=" << maximumOwnerBytes << '\n';
        }

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
    }  // namespace

    TEST_CASE("CrowdSmall layouts exercise both bounded Detour work and deterministic replay", "[integration][navigation][crowd][scale]") {
        for (const auto &[layout, name] : Layouts) {
            CAPTURE(name);
            CHECK(RunQualifiedReplay(layout, false) == RunQualifiedReplay(layout, true));
            RunBestEffort(layout, name);
        }
    }

    TEST_CASE("CrowdSmall overload withdraws old work across teleports and removal", "[integration][navigation][crowd][scale]") {
        const auto layout = Layout::Crossings;
        CrowdScene scene;
        scene.CommitOverlay(layout);
        auto provider = std::shared_ptr<INavigationCrowdBackend>(CreateRecastDetourCrowdBackend().Value());
        const std::array providers{provider};
        JobSystem jobs({.workerCount = 0, .maxQueuedJobs = 1, .maxRetainedTerminalJobs = 2});
        const auto original = Capture(scene, layout, 1, false);
        const auto requests = Requests(original, layout);
        auto submitted = NavigationCrowdBatch::Submit(jobs, original, requests, providers);
        REQUIRE(submitted.HasValue());
        auto oldWork = std::move(submitted).Value();
        const auto rejected = NavigationCrowdBatch::Submit(jobs, original, requests, providers);
        REQUIRE(rejected.HasError());
        CHECK(jobs.AdmissionSnapshot().rejected[static_cast<std::size_t>(JobPriority::Normal)] == 1);

        const auto &first = original.Agents().front();
        const auto removed = original.Agents().back().handle;
        scene.teleported = first.handle;
        REQUIRE(scene.agents.UnregisterAtSafePoint(removed).HasValue());
        const auto changed = Capture(scene, layout, 1, false);
        REQUIRE(changed.Agents().size() == CrowdAgents - 1);
        CHECK(changed.Agents().front().position.z > first.position.z + 20);
        REQUIRE(scene.agents.Find(removed).HasError());
        RequireError(oldWork->TryTake(changed, 1), NavigationErrors::StaleSnapshot);
        RequireError(oldWork->TryTake(original, 1), NavigationErrors::AdmissionRejected);
        oldWork.reset();
        jobs.Shutdown(ShutdownPolicy::Cancel);
        const auto journal = jobs.SnapshotIfChanged(0);
        REQUIRE(journal);
        REQUIRE(journal->jobs.size() == 1);
        CHECK(journal->jobs.front().state == JobState::Cancelled);
        CHECK(provider.use_count() == 2);
        scene.Close();
    }

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
}  // namespace Horo::Navigation
