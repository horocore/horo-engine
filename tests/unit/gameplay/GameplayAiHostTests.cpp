#include "GameplayAiTestSupport.h"
#include "GameplayModuleTestSupport.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Platform/DynamicLibrary.h"

#include <cstring>
#include <memory>

namespace Horo::Gameplay {
    namespace {
        /** @brief Matches the real fixture image to its generated module revision. */
        GameModuleLoadExpectation AiExpectation() {
            return {"game.ai.tests", CurrentGameplayBuildFingerprint(), Tests::ReadDescriptorRevision(HORO_TEST_AI_MODULE_REVISION_PATH)};
        }

        /** @brief Calls the real native frontend and releases its extra loader reference before returning. */
        std::shared_ptr<const Telemetry::OperationContext> CaptureModuleDiagnostics(const std::shared_ptr<const GameplayAiContext> &context,
                                                                                    AI::AgentHandle agent, AI::TaskHandle &task) {
            auto image = Platform::LoadDynamicLibrary(HORO_TEST_AI_MODULE_PATH);
            REQUIRE(image.HasValue());
            void *symbol = image.Value()->GetSymbol("HoroTestStartAiTaskWithDiagnostics");
            REQUIRE(symbol != nullptr);
            using Capture = bool (*)(const std::shared_ptr<const GameplayAiContext> &, AI::AgentHandle, AI::TaskHandle &,
                                     std::shared_ptr<const Telemetry::OperationContext> &);
            Capture capture{};
            static_assert(sizeof(capture) == sizeof(symbol));
            std::memcpy(&capture, &symbol, sizeof(capture));
            std::shared_ptr<const Telemetry::OperationContext> diagnostics;
            REQUIRE(capture(context, agent, task, diagnostics));
            REQUIRE(diagnostics);
            return diagnostics;
        }
    }  // namespace

    TEST_CASE("Gameplay AI host revokes authoritative scope and tasks before native callbacks and unload", "[gameplay][ai][host]") {
        AiTestSupport::Fixture fixture;
        auto anchor = GameplayAiContext::Create({"game.ai.tests", true, true}, fixture.runtime.CapabilityAtSafePoint());
        REQUIRE(anchor.HasValue());
        GameModuleHost host{GameModuleHostOptions{{}, {.ai = anchor.Value()}}};
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

    TEST_CASE("Native AI diagnostics and their last weak owner survive module retirement and image unload", "[gameplay][ai][host]") {
        AiTestSupport::Fixture fixture;
        auto anchor = GameplayAiContext::Create({"game.ai.tests", true, true}, fixture.runtime.CapabilityAtSafePoint());
        REQUIRE(anchor.HasValue());
        GameModuleHost host{GameModuleHostOptions{{}, {.ai = anchor.Value()}}};
        auto admitted = host.Load(HORO_TEST_AI_MODULE_PATH, AiExpectation());
        REQUIRE(admitted.HasValue());
        auto loaded = std::move(admitted).Value();
        auto context = loaded->AiContext();
        REQUIRE(context);
        std::weak_ptr<const GameplayAiContext> weakContext = context;
        AI::TaskHandle task;
        auto diagnostics = CaptureModuleDiagnostics(context, fixture.agent, task);
        std::weak_ptr<const Telemetry::OperationContext> weakDiagnostics = diagnostics;
        REQUIRE(task.IsValid());
        CHECK(fixture.runtime.Find(fixture.agent).Value().hasRunningTask);
        REQUIRE(loaded->PrepareReload().HasValue());
        CHECK_FALSE(fixture.runtime.Find(fixture.agent).Value().hasRunningTask);
        loaded.reset();  // The helper's extra image reference is already gone; native Stop/destruction and unload have completed.
        CHECK(context->InspectTask(fixture.agent, task).HasError());
        context.reset();
        CHECK(weakContext.expired());
        weakContext.reset();
        CHECK(diagnostics->operationId == 741);
        CHECK(diagnostics->parentOperationId == 740);
        const auto fields = diagnostics->diagnosticContext.Fields();
        REQUIRE(fields.size() == 1);
        CHECK(fields[0].first == "module.source");
        CHECK(fields[0].second == "native-ai-image");
        diagnostics.reset();  // The captured object's host-emitted destructor runs after the module image has unloaded.
        CHECK(weakDiagnostics.expired());
        weakDiagnostics.reset();  // The final host-emitted control-block deallocator runs after unload as well.
    }

    TEST_CASE("Gameplay host typed AI options preserve empty and Physics legacy constructor calls", "[gameplay][ai][host]") {
        GameModuleHost defaultHost;
        GameModuleHost emptyVector({});
        GameModuleHost legacyPhysics({}, {});
        GameModuleHost legacyNavigation({}, {}, {});
        GameModuleHost typed{GameModuleHostOptions{{}, {}}};
        for (auto *host : {&defaultHost, &emptyVector, &legacyPhysics, &legacyNavigation, &typed}) {
            auto loaded = host->Load(HORO_TEST_AI_MODULE_PATH, AiExpectation());
            REQUIRE(loaded.HasValue());
            CHECK_FALSE(loaded.Value()->AiContext());
        }
    }

    TEST_CASE("Gameplay AI module principal mismatch fails before native startup", "[gameplay][ai][host]") {
        AiTestSupport::Fixture fixture;
        GameModuleHost host{GameModuleHostOptions{{}, {.ai = fixture.context}}};
        auto loaded = host.Load(HORO_TEST_AI_MODULE_PATH, AiExpectation());
        AiTestSupport::ExpectError(loaded, AI::AIErrors::CapabilityUnavailable);
    }
}  // namespace Horo::Gameplay
