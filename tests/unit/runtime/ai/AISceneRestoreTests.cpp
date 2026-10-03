#include "AiSceneTestSupport.h"

#include <type_traits>

namespace Horo::AI {
    static_assert(!std::is_default_constructible_v<AiSceneRestoreCandidate::ConstructionKey>);
    static_assert(!std::is_default_constructible_v<AiSceneRestoreCandidate>);

    namespace {
        using TestSupport::ExpectError;
        using TestSupport::MakeFixture;
        using TestSupport::MakeIdentity;
        using TestSupport::MakeSchema;
        using TestSupport::Publish;
        using TestSupport::RestoreHarness;

        /** @brief Commits ownership once outside assertion macro expansion, then checks the retained result. */
        void RequireCommit(AiSceneRuntime &runtime, std::unique_ptr<AiSceneRestoreCandidate> candidate) {
            const auto committed = runtime.CommitRestoreAtSafePoint(std::move(candidate));
            REQUIRE(committed.HasValue());
        }

        /** @brief Mutates detached input in one malformed-state scenario; never touches the live harness. */
        void BreakCanonicalPopulation(AiCanonicalState &broken) {
            SECTION("missing agent") {
                broken.agents.pop_back();
            }
            SECTION("duplicate agent") {
                broken.agents.back() = broken.agents.front();
            }
            SECTION("unknown agent") {
                broken.agents.back().authored.agent = MakeIdentity<AgentId>(99);
            }
            SECTION("invalid lifecycle") {
                broken.agents.front().state = AiCanonicalAgentState::Count;
            }
            SECTION("authored mismatch") {
                broken.agents.front().authored.enabled = false;
            }
            SECTION("missing active controller") {
                broken.agents.front().controller.reset();
            }
            SECTION("missing active blackboard") {
                broken.agents.front().controller->blackboard.reset();
            }
            SECTION("disabled with blackboard") {
                broken.agents.front().state = AiCanonicalAgentState::Disabled;
            }
            SECTION("malformed later agent") {
                RestoreHarness::Blackboard(broken, 1).entries.clear();
            }
            SECTION("duplicate key") {
                auto &entries = RestoreHarness::Blackboard(broken).entries;
                entries.push_back(entries.front());
            }
            SECTION("missing required value") {
                RestoreHarness::Blackboard(broken).entries.front().value.reset();
            }
        }

        /** @brief Exercises agent/task/schema invalidation while the Scene generation remains current. */
        void CheckAgentRestoreFences(RestoreHarness &harness, const AiCanonicalState &original,
                                     std::unique_ptr<AiSceneRestoreCandidate> &staged) {
            SECTION("task mutation expires staging") {
                harness.StartTask();
                ExpectError(harness.runtime.CommitRestoreAtSafePoint(std::exchange(staged, {})), AIErrors::CanonicalRestoreStale);
                CHECK(harness.runtime.Find(harness.handle).Value().hasRunningTask);
            }
            SECTION("disable expires staging") {
                REQUIRE(harness.runtime.DisableAtSafePoint(harness.handle).HasValue());
                ExpectError(harness.runtime.CommitRestoreAtSafePoint(std::exchange(staged, {})), AIErrors::CanonicalRestoreStale);
                CHECK(harness.runtime.Find(harness.handle).Value().state == AiAgentActivationState::Disabled);
            }
            SECTION("retirement expires staging") {
                REQUIRE(harness.runtime.RetireOwnerAtSafePoint(harness.fixtures.front().owner).Value() == 1);
                ExpectError(harness.runtime.CommitRestoreAtSafePoint(std::exchange(staged, {})), AIErrors::CanonicalRestoreStale);
            }
            SECTION("another restore expires the old instance schema fence") {
                auto winner = harness.Stage(original);
                RequireCommit(harness.runtime, std::move(winner));
                ExpectError(harness.runtime.CommitRestoreAtSafePoint(std::exchange(staged, {})), AIErrors::CanonicalRestoreStale);
            }
        }

        TEST_CASE("Canonical AI restore publishes complete values and restarts fresh transient work", "[unit][ai][restore]") {
            RestoreHarness harness;
            const auto original = harness.Capture();
            harness.StartTask();
            CHECK(harness.Capture() == original);
            auto changed = original;
            RestoreHarness::Blackboard(changed).entries.front().value = BlackboardValue{BlackboardScalarValue{true}};
            auto candidate = harness.Stage(changed);
            CHECK(harness.Capture() == original);
            REQUIRE(harness.runtime.Find(harness.handle).Value().hasRunningTask);
            RequireCommit(harness.runtime, std::move(candidate));
            CHECK(harness.Capture() == changed);
            CHECK_FALSE(harness.runtime.Find(harness.handle).Value().hasRunningTask);
            CHECK(harness.scene->View().Get(harness.fixtures.front().owner).Value().components->aiAgent ==
                  original.agents.front().authored);
            harness.StartTask();
            CHECK(harness.runtime.Find(harness.handle).Value().hasRunningTask);
        }

        TEST_CASE("Canonical AI restore rolls back staging and rejects malformed complete populations", "[unit][ai][restore]") {
            RestoreHarness harness;
            harness.StartTask();
            const auto original = harness.Capture();
            SECTION("candidate destruction is rollback") {
                auto staged = harness.Stage(original);
                staged.reset();
            }
            SECTION("malformed input never publishes") {
                auto broken = original;
                BreakCanonicalPopulation(broken);
                ExpectError(harness.runtime.PrepareRestoreAtSafePoint(harness.scene->View(), harness.fixtures.front().binding, broken),
                            AIErrors::CanonicalStateInvalid);
            }
            SECTION("unsupported canonical version") {
                auto broken = original;
                ++broken.version;
                ExpectError(harness.runtime.PrepareRestoreAtSafePoint(harness.scene->View(), harness.fixtures.front().binding, broken),
                            AIErrors::CanonicalSchemaUnsupported);
            }
            CHECK(harness.Capture() == original);
            CHECK(harness.runtime.Find(harness.handle).Value().hasRunningTask);
        }

        TEST_CASE("Canonical AI migration preserves prior state and authored bindings on failure", "[unit][ai][restore][migration]") {
            RestoreHarness harness;
            harness.StartTask();
            const auto original = harness.Capture();
            auto source = original;
            RestoreHarness::Blackboard(source).schemaVersion = 1;
            RestoreHarness::Blackboard(source).entries.front().value = BlackboardValue{BlackboardScalarValue{true}};
            const auto sourceSchema = MakeSchema(70);
            const BlackboardSchemaMigration migration{.source = sourceSchema,
                                                      .destination = sourceSchema->Identity(),
                                                      .destinationVersion = 2};
            SECTION("compatible migration publishes source values") {
                auto candidate = harness.Stage(source, std::array{migration});
                RequireCommit(harness.runtime, std::move(candidate));
                RestoreHarness::Blackboard(source).schemaVersion = 2;
                CHECK(harness.Capture() == source);
            }
            SECTION("failure preserves all agents and work") {
                SECTION("no source schema") {
                    ExpectError(harness.runtime.PrepareRestoreAtSafePoint(harness.scene->View(), harness.fixtures.front().binding, source),
                                AIErrors::CanonicalSchemaUnsupported);
                }
                SECTION("ambiguous migration") {
                    ExpectError(harness.runtime.PrepareRestoreAtSafePoint(harness.scene->View(), harness.fixtures.front().binding, source,
                                                                          std::array{migration, migration}),
                                AIErrors::CanonicalSchemaUnsupported);
                }
                SECTION("malformed source value") {
                    RestoreHarness::Blackboard(source).entries.front().value = BlackboardValue{BlackboardScalarValue{2.0}};
                    ExpectError(harness.runtime.PrepareRestoreAtSafePoint(harness.scene->View(), harness.fixtures.front().binding, source,
                                                                          std::array{migration}),
                                AIErrors::BlackboardValueTypeMismatch);
                }
                SECTION("controller plan identity mismatch") {
                    source.agents.front().controller->authored.decisionAsset = MakeIdentity<DecisionGraphAssetId>(555);
                    ExpectError(harness.runtime.PrepareRestoreAtSafePoint(harness.scene->View(), harness.fixtures.front().binding, source),
                                AIErrors::CanonicalSchemaUnsupported);
                }
                CHECK(harness.Capture() == original);
                CHECK(harness.runtime.Find(harness.handle).Value().hasRunningTask);
                CHECK(harness.scene->View().Get(harness.fixtures.front().owner).Value().components->aiController ==
                      original.agents.front().controller->authored);
            }
        }

        TEST_CASE("Canonical restore services cancellation before staging and before commit", "[unit][ai][restore][cancel]") {
            RestoreHarness harness;
            harness.StartTask();
            const auto original = harness.Capture();
            CancellationSource cancellation;
            SECTION("cancel before preparation") {
                cancellation.RequestCancellation();
                ExpectError(harness.runtime.PrepareRestoreAtSafePoint(harness.scene->View(), harness.fixtures.front().binding, original, {},
                                                                      cancellation.Token()),
                            AIErrors::CanonicalRestoreCancelled);
            }
            SECTION("cancel after preparation") {
                auto staged = harness.Stage(original, {}, cancellation.Token());
                cancellation.RequestCancellation();
                ExpectError(harness.runtime.CommitRestoreAtSafePoint(std::exchange(staged, {})), AIErrors::CanonicalRestoreCancelled);
            }
            CHECK(harness.Capture() == original);
            CHECK(harness.runtime.Find(harness.handle).Value().hasRunningTask);
        }

        TEST_CASE("Canonical restore fences scene agent schema and competing restore publication", "[unit][ai][restore][generation]") {
            RestoreHarness harness;
            auto original = harness.Capture();
            auto staged = harness.Stage(original);
            CheckAgentRestoreFences(harness, original, staged);
            SECTION("scene structural generation changes") {
                const auto borrowed = harness.scene->View();
                Runtime::SceneCommandBuffer commands;
                commands.Destroy(harness.fixtures.back().owner);
                REQUIRE(harness.scene->Commit(commands).HasValue());
                CHECK_FALSE(borrowed.IsCurrent());
                ExpectError(harness.runtime.CommitRestoreAtSafePoint(std::exchange(staged, {})), AIErrors::CanonicalRestoreStale);
                ExpectError(harness.runtime.CaptureCanonicalState(harness.scene->View()), AIErrors::CanonicalRestoreStale);
            }
            SECTION("scene publication replacement expires staging") {
                auto replacement = MakeFixture(1, 11, 7, 0, 2);
                auto replaced = Publish(harness.runtime, replacement);
                REQUIRE(replaced != nullptr);
                ExpectError(harness.runtime.CommitRestoreAtSafePoint(std::exchange(staged, {})), AIErrors::CanonicalRestoreStale);
            }
            SECTION("shutdown closes restore admission") {
                harness.runtime.BeginShutdown();
                ExpectError(harness.runtime.CommitRestoreAtSafePoint(std::exchange(staged, {})), AIErrors::RuntimeUnavailable);
                ExpectError(harness.runtime.CaptureCanonicalState(harness.scene->View()), AIErrors::RuntimeUnavailable);
                ExpectError(harness.runtime.PrepareRestoreAtSafePoint(harness.scene->View(), harness.fixtures.front().binding, original),
                            AIErrors::RuntimeUnavailable);
            }
            SECTION("another runtime cannot consume this candidate") {
                RestoreHarness other;
                ExpectError(other.runtime.CommitRestoreAtSafePoint(std::exchange(staged, {})), AIErrors::CanonicalStateInvalid);
                CHECK(other.Capture() == original);
            }
            SECTION("wrong destination binding") {
                auto wrong = harness.fixtures.front().binding;
                wrong.incarnation = AiRuntimeIncarnation::Create(99).Value();
                ExpectError(harness.runtime.PrepareRestoreAtSafePoint(harness.scene->View(), wrong, original),
                            AIErrors::CanonicalRestoreStale);
            }
            SECTION("absent scene borrow") {
                ExpectError(harness.runtime.PrepareRestoreAtSafePoint({}, harness.fixtures.front().binding, original),
                            AIErrors::CanonicalRestoreStale);
            }
        }

        TEST_CASE("Canonical disabled agent state restores without transient work and can be reconstructed", "[unit][ai][restore]") {
            RestoreHarness harness;
            const auto active = harness.Capture();
            auto disabled = active;
            disabled.agents.front().state = AiCanonicalAgentState::Disabled;
            disabled.agents.front().controller->blackboard.reset();
            auto candidate = harness.Stage(disabled);
            RequireCommit(harness.runtime, std::move(candidate));
            CHECK(harness.Capture() == disabled);
            ExpectError(harness.runtime.StartTaskAtSafePoint(harness.handle, MakeIdentity<TaskId>(42)), AIErrors::HandleInvalid);
            auto restored = harness.Stage(active);
            RequireCommit(harness.runtime, std::move(restored));
            CHECK(harness.Capture() == active);
            harness.StartTask();
        }

        TEST_CASE("Canonical restore rejects stale entity-valued blackboards before publication", "[unit][ai][restore][entity]") {
            RestoreHarness harness{true};
            harness.StartTask();
            const auto original = harness.Capture();
            auto broken = original;
            auto reference = BlackboardStoredEntityReference{7, 1, 1};
            SECTION("wrong Scene incarnation") {
                reference.sceneIncarnation = 99;
            }
            SECTION("recycled entity generation") {
                ++reference.generation;
            }
            SECTION("missing entity slot") {
                reference.slot = 99;
            }
            RestoreHarness::Blackboard(broken).entries.front().value = BlackboardValue{BlackboardScalarValue{reference}};
            ExpectError(harness.runtime.PrepareRestoreAtSafePoint(harness.scene->View(), harness.fixtures.front().binding, broken),
                        AIErrors::CanonicalRestoreStale);
            CHECK(harness.Capture() == original);
            CHECK(harness.runtime.Find(harness.handle).Value().hasRunningTask);
        }

    }  // namespace
}  // namespace Horo::AI
