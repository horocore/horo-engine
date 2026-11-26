#include "Horo/AI/AiReducedPopulation.h"
#include "Horo/Assets/AssetProvider.h"
#include "AiSceneTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <thread>

namespace Horo::AI {
    namespace {
        TEST_CASE("Reduced population proposes a bounded batch without constructing full agents", "[ai][reduced-population]") {
            constexpr std::size_t count = 65'536;
            std::vector<AiReducedAgent> rows;
            rows.reserve(count);
            for (std::size_t id = 1; id <= count; ++id)
                rows.push_back({AgentId::Create(id).Value(), {25, 0, -25}});
            rows.front().positionMillimeters[0] = std::numeric_limits<std::int64_t>::max();

            auto created = AiReducedPopulation::Create({count, 2, 25}, rows);
            REQUIRE(created.HasValue());
            auto &population = created.Value();
            std::array<AiReducedAgent, 2> output{};
            CHECK(population.EvaluatePromotions({0, 0, 0}, {}).HasError());
            const auto evaluated = population.EvaluatePromotions({0, 0, 0}, output);
            REQUIRE(evaluated.HasValue());
            CHECK(evaluated.Value() == output.size());
            CHECK(output.front().agent == rows[1].agent);
            CHECK(output.back().agent == rows[2].agent);
            const auto overflow = population.EvaluatePromotions({std::numeric_limits<std::int64_t>::min(), 0, 0}, output);
            REQUIRE(overflow.HasValue());
            CHECK(overflow.Value() == 0);
            CHECK(population.EvaluatePromotions({std::numeric_limits<std::int64_t>::min(), 0, 0}, output).Value() == 0);
            CHECK(population.EvaluatePromotions({0, 0, 0}, output).Value() == output.size());
            CHECK(output.front().agent == rows[1].agent);
        }

        TEST_CASE("Reduced population transfers identity only when its transaction publishes", "[ai][reduced-population]") {
            const auto first = AgentId::Create(1).Value();
            const auto second = AgentId::Create(2).Value();
            const auto missing = AgentId::Create(3).Value();
            const std::array rows{AiReducedAgent{first, {1, 0, 0}}, AiReducedAgent{second, {2, 0, 0}}};
            auto created = AiReducedPopulation::Create({rows.size(), 2, 10}, rows);
            REQUIRE(created.HasValue());
            auto &population = created.Value();
            std::array<AiReducedAgent, 2> output{};
            const std::array invalid{first, missing};
            CHECK(population.PrepareTransition(invalid, {}).HasError());
            CHECK(population.PrepareTransition(std::span{&first, 1}, rows).HasError());
            auto stale = population.PrepareTransition(std::span{&second, 1}, {});
            REQUIRE(stale.HasValue());
            {
                auto candidate = population.PrepareTransition(std::span{&first, 1}, {});
                REQUIRE(candidate.HasValue());
                REQUIRE(candidate.Value().ValidatePublication().HasValue());
            }
            CHECK(population.EvaluatePromotions({0, 0, 0}, output).Value() == 2);
            output = {};
            CHECK(population.EvaluatePromotions({0, 0, 0}, output).Value() == 2);
            CHECK(output.front().agent == first);
            CHECK(output.back().agent == second);

            auto promote = population.PrepareTransition(std::span{&first, 1}, {});
            REQUIRE(promote.HasValue());
            REQUIRE(promote.Value().Publish().HasValue());
            CHECK(stale.Value().Publish().HasError());
            CHECK(promote.Value().Publish().HasError());
            CHECK(population.EvaluatePromotions({0, 0, 0}, output).Value() == 1);
            CHECK(output.front().agent == second);

            const AiReducedAgent demoted{first, {10, 4, -2}};
            auto swap = population.PrepareTransition(std::span{&second, 1}, std::span{&demoted, 1});
            REQUIRE(swap.HasValue());
            bool rejectedOnForeignLane = false;
            std::thread foreign([&] { rejectedOnForeignLane = swap.Value().Publish().HasError(); });
            foreign.join();
            CHECK(rejectedOnForeignLane);
            REQUIRE(swap.Value().Publish().HasValue());
            CHECK(population.EvaluatePromotions({0, 0, 0}, output).Value() == 1);
            CHECK(output.front().agent == first);
            CHECK(output.front().positionMillimeters == demoted.positionMillimeters);
        }

        TEST_CASE("Scene structural publication transfers a reduced identity with its canonical AI", "[ai][reduced-population][scene]") {
            auto runtime = std::move(AiSceneRuntime::Create()).Value();
            const auto fixture = TestSupport::MakeFixture(10, 20, 30, 0);
            const auto identity = fixture.agent.agent.agent;
            const std::array manifest{AiReducedAgent{identity, {1000, 0, 0}}};
            auto created = AiReducedPopulation::Create({1, 1, 5000}, manifest);
            REQUIRE(created.HasValue());
            auto population = std::make_shared<AiReducedPopulation>(std::move(created).Value());
            bool allowDemotion = false;
            JobSystem jobs{{1, 4}};
            Assets::AssetRegistry registry;
            REQUIRE(registry.Publish(std::vector<Assets::AssetRecord>{}).status == Assets::AssetRegistryBuildStatus::Complete);
            Assets::MemoryAssetProvider assets;
            Assets::AssetLoadService loads{jobs, assets};
            Runtime::RuntimeSceneService scenes{registry, loads, {}, {30}};
            const std::array descriptors{fixture.descriptor};
            REQUIRE(scenes.AddActivationParticipant(std::make_unique<AiSceneActivationParticipant>(runtime, descriptors)).HasValue());
            auto structural = runtime.MakeReducedPopulationStructuralParticipant(
                descriptors, population, [&](const Runtime::RuntimeSceneView &scene, const Runtime::EntityRef entity) {
                    using Output = Result<Math::WorldCoordinate64>;
                    if (!allowDemotion)
                        return Output::Failure(MakeError(AIErrors::CapabilityUnavailable));
                    const auto view = scene.Get(entity);
                    REQUIRE(view.HasValue());
                    REQUIRE(view.Value().localTransform);
                    return Output::Success(
                        Math::WorldCoordinate64::FromMillimeters(static_cast<std::int64_t>(view.Value().localTransform->translation.x * 1000),
                                                                 0, 0));
                });
            REQUIRE(structural.HasValue());
            REQUIRE(scenes.AddStructuralParticipant(std::move(structural).Value()).HasValue());
            const CancellationToken cancellation;
            REQUIRE(scenes.Startup(cancellation).HasValue());
            const auto definition = Runtime::SceneDefinitionBuilder{{30}, {1}}.Build();
            REQUIRE(definition.HasValue());
            REQUIRE(scenes.QueuePreparation(definition.Value()).HasValue());
            std::uint64_t frame{};
            const auto advance = [&] {
                REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges,
                                       {.frameNumber = ++frame, .cancellation = cancellation}).HasValue());
            };
            advance();
            const auto admission = [&] {
                const auto view = scenes.ActiveScene();
                REQUIRE(view);
                return Runtime::SceneStructuralAdmission{view->RuntimeId(), registry.Snapshot().Revision(), cancellation};
            };
            const auto commit = [&](Runtime::SceneCommandBuffer commands) {
                auto queued = scenes.QueueTrackedStructuralCommands(std::move(commands));
                REQUIRE(queued.HasValue());
                advance();
                REQUIRE(queued.Value()->Complete());
                return std::move(queued).Value();
            };
            const auto promote = [&] {
                Runtime::SceneCommandBuffer commands;
                Runtime::RuntimeEntityGroupEntry added{.info = {.authoredObject = Runtime::SceneObjectId{9},
                                                                .components = {.aiAgent = fixture.agent.agent,
                                                                               .aiController = fixture.agent.controller}}};
                added.info.localTransform.translation.x = 2;
                const auto queued = commands.CreateGroup({std::move(added)}, {}, admission());
                if (queued.HasError())
                    UNSCOPED_INFO(queued.ErrorValue().message);
                REQUIRE(queued.HasValue());
                return commit(std::move(commands));
            };
            std::array<AiReducedAgent, 1> nearby{};
            CHECK(population->EvaluatePromotions({0, 0, 0}, nearby).Value() == 1);
            REQUIRE(promote()->ResultValue());
            CHECK(population->EvaluatePromotions({0, 0, 0}, nearby).Value() == 0);
            const auto scene = scenes.ActiveScene();
            REQUIRE(scene);
            const auto entity = scene->Find({9});
            REQUIRE(entity);
            const auto capability = runtime.CapabilityAtSafePoint();
            const auto canonical = capability.ResolveAgent(identity);
            REQUIRE(canonical.HasValue());
            const auto demote = [&] {
                Runtime::SceneCommandBuffer commands;
                REQUIRE(commands.DestroyGroup({*entity}, admission()).HasValue());
                return commit(std::move(commands));
            };
            REQUIRE(demote()->Failure());
            CHECK(capability.ResolveAgent(identity).Value() == canonical.Value());
            CHECK(population->EvaluatePromotions({0, 0, 0}, nearby).Value() == 0);
            allowDemotion = true;
            REQUIRE(demote()->ResultValue());
            CHECK(capability.ResolveAgent(identity).HasError());
            CHECK(population->EvaluatePromotions({0, 0, 0}, nearby).Value() == 1);
            CHECK(nearby.front().agent == identity);
            CHECK(nearby.front().positionMillimeters[0] == 2000);

            REQUIRE(promote()->ResultValue());
            const auto replacement = capability.ResolveAgent(identity);
            REQUIRE(replacement.HasValue());
            CHECK(replacement.Value() != canonical.Value());
            CHECK(population->EvaluatePromotions({0, 0, 0}, nearby).Value() == 0);
            scenes.Shutdown();
        }
    }  // namespace
}  // namespace Horo::AI
