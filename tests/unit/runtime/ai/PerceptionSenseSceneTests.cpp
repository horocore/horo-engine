#include "Horo/AI/AIScenePerceptionSource.h"
#include "Horo/AI/PerceptionSenseScene.h"
#include "Horo/Gameplay/LuaPerceptionSense.h"
#include "PerceptionSenseTestSupport.h"

#include <functional>
#include <stdexcept>
#include <thread>

namespace Horo::AI {
    namespace {
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;

        struct State final {
            std::atomic_bool admission{true};
            std::function<void()> callback;
            std::size_t calls{};
            bool sensed{true};
        };

        /** @brief Test provider receives only facts and records callback entry for pre-admission checks. */
        class Sense final : public IPerceptionSenseProvider {
        public:
            explicit Sense(State &state) : state_(state) {}

            Result<std::size_t> Evaluate(const PerceptionSenseInput &input, std::span<PerceptionSenseSample> output,
                                         AiWorkBudget &budget) override {
                ++state_.calls;
                if (state_.callback)
                    state_.callback();
                for (std::size_t index = 0; index < input.candidates.size(); ++index) {
                    if (!budget.Consume())
                        return Result<std::size_t>::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
                    output[index] = {static_cast<std::uint32_t>(index), state_.sensed};
                }
                return Result<std::size_t>::Success(input.candidates.size());
            }

        private:
            State &state_;
        };

        struct Fixture final : PerceptionSenseTestSupport::InputFixture {
            std::shared_ptr<State> state{std::make_shared<State>()};
            std::unique_ptr<Runtime::RuntimeScene> scene;
            Runtime::EntityRef listener;
            std::array<PerceptionSpatialSource, 2> sources;
            PerceptionSpatialListener spatialListener;
            PerceptionSpatialBroadphase spatial;
            std::shared_ptr<const PerceptionSenseGeneration> generation;
            std::unique_ptr<PerceptionSenseInstance> instance;
            std::optional<AIPerceptionMemory> memory;
            PerceptionSpatialQuery query;

            Fixture() {
                Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}};
                for (std::uint64_t index = 1; index <= 3; ++index) {
                    Runtime::RuntimeEntityDefinition entity;
                    entity.object = Runtime::SceneObjectId{index};
                    builder.Add(entity);
                }
                const auto definition = std::move(builder).Build();
                REQUIRE(definition.HasValue());
                auto created = Runtime::RuntimeScene::Create(definition.Value(), Runtime::SceneRuntimeId{77});
                REQUIRE(created.HasValue());
                scene = std::move(created).Value();
                const auto view = scene->View();
                listener = *view.Find(Runtime::SceneObjectId{1});
                binding.listener = ProjectPerceptionSource(listener);
                const auto sense = MakeIdentity<SenseTypeId>(20);
                const PerceptionDescriptorOrigin origin{PerceptionDescriptorSourceKind::Native, MakeIdentity<PerceptionProviderId>(22), 1};
                PerceptionSenseRegistration registration{{{sense, origin, "scene sense", capabilities},
                                                          {MakeIdentity<StimulusTypeId>(21), origin, "scene sample", 1, {}},
                                                          sizeof(Sense),
                                                          4,
                                                          2},
                                                         {state.get(), [](void *raw) -> IPerceptionSenseProvider * {
                    return new Sense{*static_cast<State *>(raw)};
                }, [](void *, IPerceptionSenseProvider *provider) noexcept {
                    delete provider;
                }}};
                auto captured = PerceptionSenseGeneration::Capture(std::span{&registration, 1},
                                                                   std::shared_ptr<const std::atomic_bool>{state, &state->admission});
                REQUIRE(captured.HasValue());
                generation = captured.Value();
                instance = Instance(generation);
                auto admitted = AIPerceptionMemory::Create(77, binding.agent);
                REQUIRE(admitted.HasValue());
                memory.emplace(std::move(admitted).Value());
                spatialListener.entity = listener;
                spatialListener.senses[0] = sense;
                spatialListener.senseCount = 1;
                for (std::size_t index = 0; index < sources.size(); ++index) {
                    auto &source = sources[index];
                    source.entity = *view.Find(Runtime::SceneObjectId{index + 2});
                    source.position = candidates[index].position;
                    source.layers = 1;
                    source.senses[0] = sense;
                    source.senseCount = 1;
                }
                Publish(1);
                query = {.listener = listener, .sense = sense, .radiusMillimeters = 10'000, .visibleLayers = 1, .maximumCandidates = 2};
            }

            void Publish(std::uint64_t revision) {
                REQUIRE(spatial.Publish(scene->View(), revision, std::span{&spatialListener, 1}, sources).HasValue());
            }

            Result<PerceptionSenseSceneResult> Run(std::uint64_t tick, AiWorkBudget &budget) {
                return RunPerceptionSenseAtSensePoll(scene->View(), spatial, *instance, query, tick, *memory, budget, budget);
            }
        };

        TEST_CASE("Custom spatial senses stage captured facts and retain omitted memory under truncation", "[unit][ai][sense-scene]") {
            Fixture fixture;
            AiWorkBudget budget{100};
            const auto first = fixture.Run(1, budget);
            REQUIRE(first.HasValue());
            CHECK(first.Value().samples == 2);
            CHECK(budget.Remaining() == 93);
            auto snapshot = fixture.memory->Snapshot(1, PerceptionSceneLiveness(*fixture.scene));
            REQUIRE(snapshot.HasValue());
            CHECK(snapshot.Value().count == 2);
            CHECK(snapshot.Value().entries[0].lastKnownPosition == fixture.sources[0].position);
            fixture.state->sensed = false;
            fixture.query.maximumCandidates = 1;
            const auto second = fixture.Run(2, budget);
            REQUIRE(second.HasValue());
            CHECK(second.Value().truncated);
            snapshot = fixture.memory->Snapshot(2, PerceptionSceneLiveness(*fixture.scene));
            REQUIRE(snapshot.HasValue());
            CHECK_FALSE(snapshot.Value().entries[0].isCurrentlySensed);
            CHECK(snapshot.Value().entries[1].isCurrentlySensed);
        }

        TEST_CASE("Custom spatial senses reserve complete work before invoking providers", "[unit][ai][sense-scene]") {
            Fixture fixture;
            AiWorkBudget budget{6};
            ExpectError(fixture.Run(1, budget), AIErrors::SchedulerBudgetExhausted);
            CHECK(budget.Remaining() == 6);
            CHECK(fixture.state->calls == 0);
            CHECK(fixture.memory->StoredCount() == 0);
        }

        TEST_CASE("Custom spatial senses fence dead foreign and nested admission before gathering", "[unit][ai][sense-scene]") {
            Fixture fixture;
            AiWorkBudget budget{100};
            SECTION("native retirement") {
                fixture.state->admission.store(false, std::memory_order_release);
                ExpectError(fixture.Run(1, budget), AIErrors::RuntimeUnavailable);
                CHECK(fixture.state->calls == 0);
            }
            SECTION("cancellation") {
                fixture.cancellation.RequestCancellation();
                ExpectError(fixture.Run(1, budget), AIErrors::RuntimeUnavailable);
                CHECK(fixture.state->calls == 0);
            }
            SECTION("foreign owner") {
                bool rejected{};
                const auto view = fixture.scene->View();
                std::thread worker([&] {
                    const auto result = RunPerceptionSenseAtSensePoll(view, fixture.spatial, *fixture.instance, fixture.query, 1,
                                                                      *fixture.memory, budget, budget);
                    rejected = result.HasError() && result.ErrorValue().code.Value() == AIErrors::RuntimeUnavailable.code.Value();
                });
                worker.join();
                CHECK(rejected);
                CHECK(fixture.state->calls == 0);
            }
            SECTION("nested callback") {
                fixture.state->callback = [&] {
                    AiWorkBudget nested{100};
                    ExpectError(fixture.Run(2, nested), AIErrors::RuntimeUnavailable);
                    CHECK(nested.Remaining() == 100);
                };
                REQUIRE(fixture.Run(1, budget).HasValue());
                CHECK(fixture.state->calls == 1);
                CHECK(budget.Remaining() == 93);
                return;
            }
            CHECK(budget.Remaining() == 100);
            CHECK(fixture.memory->StoredCount() == 0);
        }

        TEST_CASE("Custom spatial senses reject callback-side publication and Scene changes atomically", "[unit][ai][sense-scene]") {
            Fixture fixture;
            SECTION("publication replacement") {
                fixture.state->callback = [&] {
                    fixture.Publish(2);
                };
            }
            SECTION("source despawn") {
                fixture.state->callback = [&] {
                    Runtime::SceneCommandBuffer commands;
                    commands.Destroy(fixture.sources[1].entity);
                    REQUIRE(fixture.scene->Commit(commands).HasValue());
                };
            }
            AiWorkBudget budget{100};
            ExpectError(fixture.Run(1, budget), AIErrors::PerceptionSpatialStale);
            CHECK(fixture.state->calls == 1);
            CHECK(fixture.memory->StoredCount() == 0);
        }

        TEST_CASE("Custom spatial sense callback failure leaves previous memory intact", "[unit][ai][sense-scene]") {
            Fixture fixture;
            AiWorkBudget budget{100};
            REQUIRE(fixture.Run(1, budget).HasValue());
            fixture.state->callback = [] {
                throw std::runtime_error("sense callback failed");
            };
            ExpectError(fixture.Run(2, budget), AIErrors::PerceptionProviderFailed);
            const auto snapshot = fixture.memory->Snapshot(2, PerceptionSceneLiveness(*fixture.scene));
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().count == 2);
            CHECK(snapshot.Value().entries[0].lastSensedTick == 1);
            CHECK(snapshot.Value().entries[1].lastSensedTick == 1);
        }

        TEST_CASE("Lua custom senses stage actual Scene memory and stop at program retirement", "[unit][ai][sense-scene][sense-lua]") {
            Fixture fixture;
            auto contract = fixture.generation->Contracts()[0];
            contract.sense.origin.kind = PerceptionDescriptorSourceKind::Script;
            contract.stimulus.origin.kind = PerceptionDescriptorSourceKind::Script;
            auto compiled = Gameplay::LuaPerceptionSenseProgram::Compile("return function(x) if x < 1500 then return true end end",
                                                                         contract, fixture.generation);
            REQUIRE(compiled.HasValue());
            auto program = std::move(compiled).Value();
            auto acquired = program->Acquire();
            REQUIRE(acquired.HasValue());
            fixture.instance = fixture.Instance(acquired.Value());
            AiWorkBudget budget{100};
            const auto first = fixture.Run(1, budget);
            REQUIRE(first.HasValue());
            CHECK(first.Value().candidates == 2);
            CHECK(first.Value().samples == 1);
            CHECK(budget.Remaining() == 93);
            CHECK(fixture.state->calls == 0);
            const auto before = fixture.memory->Snapshot(1, PerceptionSceneLiveness(*fixture.scene));
            REQUIRE(before.HasValue());
            REQUIRE(before.Value().count == 1);
            CHECK(before.Value().entries[0].lastKnownPosition == fixture.sources[0].position);
            program->Retire();
            ExpectError(fixture.Run(2, budget), AIErrors::RuntimeUnavailable);
            CHECK(budget.Remaining() == 93);
            const auto after = fixture.memory->Snapshot(2, PerceptionSceneLiveness(*fixture.scene));
            REQUIRE(after.HasValue());
            REQUIRE(after.Value().count == 1);
            CHECK(after.Value().entries[0].lastSensedTick == 1);
        }

        TEST_CASE("Lua later-candidate instruction failure preserves actual Scene memory atomically",
                  "[unit][ai][sense-scene][sense-lua]") {
            Fixture fixture;
            auto contract = fixture.generation->Contracts()[0];
            contract.sense.origin.kind = PerceptionDescriptorSourceKind::Script;
            contract.stimulus.origin.kind = PerceptionDescriptorSourceKind::Script;
            auto compiled = Gameplay::LuaPerceptionSenseProgram::Compile(R"(
                local calls = 0
                return function()
                    calls = calls + 1
                    if calls == 4 then while true do end end
                    return true
                end
            )",
                                                                         contract, fixture.generation);
            REQUIRE(compiled.HasValue());
            auto program = std::move(compiled).Value();
            auto acquired = program->Acquire();
            REQUIRE(acquired.HasValue());
            fixture.instance = fixture.Instance(acquired.Value());
            AiWorkBudget budget{100};
            REQUIRE(fixture.Run(1, budget).HasValue());
            ExpectError(fixture.Run(2, budget), AIErrors::PerceptionProviderFailed);
            CHECK_FALSE(fixture.instance->CanEvaluate());
            const auto snapshot = fixture.memory->Snapshot(2, PerceptionSceneLiveness(*fixture.scene));
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().count == 2);
            CHECK(snapshot.Value().entries[0].lastSensedTick == 1);
            CHECK(snapshot.Value().entries[1].lastSensedTick == 1);
            CHECK(fixture.state->calls == 0);
        }
    }  // namespace
}  // namespace Horo::AI
