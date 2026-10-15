#include "GameplayAiTestSupport.h"
#include "GameplayModuleTestSupport.h"
#include "Horo/Gameplay/GameModuleHost.h"

namespace Horo::Gameplay {
    namespace {
        /** @brief Matches the real fixture image to its generated module revision. */
        GameModuleLoadExpectation AiExpectation() {
            return {"game.ai.tests", CurrentGameplayBuildFingerprint(), Tests::ReadDescriptorRevision(HORO_TEST_AI_MODULE_REVISION_PATH)};
        }
    }  // namespace

    TEST_CASE("Gameplay AI host revokes authoritative scope and tasks before native callbacks and unload", "[gameplay][ai][host]") {
        AiTestSupport::Fixture fixture;
        auto anchor = GameplayAiContext::Create({"game.ai.tests", true, true}, fixture.runtime.CapabilityAtSafePoint());
        REQUIRE(anchor.HasValue());
        GameModuleHost host{GameModuleHostOptions{{}, {{}, anchor.Value()}}};
        auto admitted = host.Load(HORO_TEST_AI_MODULE_PATH, AiExpectation());
        REQUIRE(admitted.HasValue());
        auto loaded = std::move(admitted).Value();
        auto retained = loaded->AiContext();
        REQUIRE(retained);
        std::weak_ptr<const GameplayAiContext> weak = retained;
        auto started = retained->StartTask(fixture.agent, AiTestSupport::MakeIdentity<AI::TaskId>(411));
        REQUIRE(started.HasValue());
        auto prepared = loaded->PrepareReload();
        REQUIRE(prepared.HasValue());
        REQUIRE(prepared.Value().payload == std::vector{std::byte{1}});
        CHECK_FALSE(fixture.runtime.Find(fixture.agent).Value().hasRunningTask);
        CHECK(retained->ResolveAgent(fixture.scene.agent.agent.agent).HasError());
        loaded.reset();  // Actual native library is now unloaded; the host-created control block remains safe.
        CHECK(retained->ResolveAgent(fixture.scene.agent.agent.agent).HasError());
        retained.reset();
        CHECK(weak.expired());
        weak.reset();
        auto replacement = host.Load(HORO_TEST_AI_MODULE_PATH, AiExpectation());
        REQUIRE(replacement.HasValue());
        auto next = replacement.Value()->AiContext();
        REQUIRE(next);
        CHECK(next->StartTask(fixture.agent, AiTestSupport::MakeIdentity<AI::TaskId>(412)).HasValue());
    }

    TEST_CASE("Gameplay host typed AI options preserve empty and Physics legacy constructor calls", "[gameplay][ai][host]") {
        GameModuleHost defaultHost;
        GameModuleHost emptyVector({});
        GameModuleHost legacyPhysics({}, {});
        GameModuleHost typed{GameModuleHostOptions{{}, {}}};
        for (auto *host : {&defaultHost, &emptyVector, &legacyPhysics, &typed}) {
            auto loaded = host->Load(HORO_TEST_AI_MODULE_PATH, AiExpectation());
            REQUIRE(loaded.HasValue());
            CHECK_FALSE(loaded.Value()->AiContext());
        }
    }

    TEST_CASE("Gameplay AI module principal mismatch fails before native startup", "[gameplay][ai][host]") {
        AiTestSupport::Fixture fixture;
        GameModuleHost host{GameModuleHostOptions{{}, {{}, fixture.context}}};
        auto loaded = host.Load(HORO_TEST_AI_MODULE_PATH, AiExpectation());
        AiTestSupport::ExpectError(loaded, AI::AIErrors::CapabilityUnavailable);
    }
}  // namespace Horo::Gameplay
