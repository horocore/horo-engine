#include "AiSceneTestSupport.h"

namespace Horo::AI {
    namespace {
        using namespace TestSupport;

        struct GroupFixture {
            RestoreHarness resident;
            ActivationFixture added{MakeFixture(3, 11, 7, 2, 2)};
            Runtime::RuntimeComponentSet components{.aiAgent = added.agent.agent, .aiController = added.agent.controller};
            Runtime::RuntimeEntityView view{.entity = added.owner, .components = &components};
            std::unique_ptr<Runtime::SceneStructuralParticipant> participant{
                resident.runtime.MakeStructuralParticipant(std::array{resident.fixtures.front().descriptor})};

            [[nodiscard]] std::unique_ptr<Runtime::SceneStructuralCandidate> Prepare(
                const std::span<const Runtime::EntityRef> removed = {}) {
                auto prepared = participant->Prepare(resident.scene->View(), std::array{view}, removed);
                REQUIRE(prepared.HasValue());
                return std::move(prepared).Value();
            }
        };

        TEST_CASE("AI group staging preserves resident tasks and rollback never publishes additions", "[unit][ai][prefab][group]") {
            GroupFixture fixture;
            REQUIRE(fixture.resident.runtime.StartTaskAtSafePoint(fixture.resident.handle, MakeIdentity<TaskId>(31)).HasValue());
            auto staged = fixture.Prepare();
            REQUIRE(staged->ValidatePublication().HasValue());
            REQUIRE(fixture.resident.runtime.Snapshot().Value().Agents().size() == 2);
            REQUIRE(fixture.resident.runtime.Find(fixture.resident.handle).Value().hasRunningTask);
            staged.reset();
            REQUIRE(fixture.resident.runtime.Snapshot().Value().Agents().size() == 2);
            REQUIRE(fixture.resident.runtime.Find(fixture.resident.handle).Value().hasRunningTask);

            staged = fixture.Prepare();
            REQUIRE(staged->ValidatePublication().HasValue());
            staged->Publish();
            REQUIRE(staged->AfterPublication().HasValue());
            REQUIRE(fixture.resident.runtime.Snapshot().Value().Agents().size() == 3);
            REQUIRE(fixture.resident.runtime.Find(fixture.resident.handle).Value().hasRunningTask);
        }

        TEST_CASE("AI group owner mutations and shutdown invalidate detached candidates", "[unit][ai][prefab][group]") {
            GroupFixture fixture;
            auto staged = fixture.Prepare();
            REQUIRE(fixture.resident.runtime.StartTaskAtSafePoint(fixture.resident.handle, MakeIdentity<TaskId>(32)).HasValue());
            REQUIRE(staged->ValidatePublication().HasError());
            staged.reset();
            staged = fixture.Prepare();
            fixture.resident.runtime.BeginShutdown();
            REQUIRE(staged->ValidatePublication().HasError());
            staged.reset();
        }

        TEST_CASE("AI group replacement reuses retired capacity with a fresh generation", "[unit][ai][prefab][group]") {
            GroupFixture fixture;
            const auto previous = fixture.resident.handle;
            REQUIRE(fixture.resident.runtime.StartTaskAtSafePoint(previous, MakeIdentity<TaskId>(33)).HasValue());
            auto staged = fixture.Prepare(std::array{fixture.resident.fixtures.front().owner});
            REQUIRE(staged->ValidatePublication().HasValue());
            staged->Publish();
            REQUIRE(staged->AfterPublication().HasValue());
            REQUIRE(fixture.resident.runtime.Find(previous).HasError());
            const auto snapshot = fixture.resident.runtime.Snapshot();
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Agents().size() == 2);
            bool found = false;
            for (const auto &agent : snapshot.Value().Agents()) {
                if (agent.owner != fixture.added.owner)
                    continue;
                found = true;
                REQUIRE(agent.handle.slot.index == previous.slot.index);
                REQUIRE(agent.handle.slot.generation > previous.slot.generation);
            }
            REQUIRE(found);
        }

        TEST_CASE("AI full-budget replacement admits only capacity refunded by the same transaction", "[unit][ai][prefab][group]") {
            GroupFixture fixture;
            auto bounded = std::move(AiSceneRuntime::Create({.maximumAgents = 2})).Value();
            auto prepared = bounded.PrepareScene(fixture.resident.fixtures.front().binding,
                                                 std::array{fixture.resident.fixtures[0].agent, fixture.resident.fixtures[1].agent},
                                                 std::array{fixture.resident.fixtures.front().descriptor});
            REQUIRE(prepared.HasValue());
            auto publication = std::move(prepared).Value();
            publication->Publish();
            auto participant = bounded.MakeStructuralParticipant(std::array{fixture.resident.fixtures.front().descriptor});
            auto rejected = participant->Prepare(fixture.resident.scene->View(), std::array{fixture.view}, {});
            REQUIRE(rejected.HasError());
            REQUIRE(bounded.Snapshot().Value().Agents().size() == 2);
            auto replacement = participant->Prepare(fixture.resident.scene->View(), std::array{fixture.view},
                                                    std::array{fixture.resident.fixtures.front().owner});
            REQUIRE(replacement.HasValue());
            REQUIRE(replacement.Value()->ValidatePublication().HasValue());
            replacement.Value()->Publish();
            REQUIRE(replacement.Value()->AfterPublication().HasValue());
            REQUIRE(bounded.Snapshot().Value().Agents().size() == 2);
        }
    }  // namespace
}  // namespace Horo::AI
