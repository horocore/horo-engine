#include "GameplayModuleTestSupport.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameplayRegistrationRuntime.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "RuntimeUiCapabilityTestSupport.h"
#include "unit/gameplay/GameplayAiTestSupport.h"

#include <cstring>

namespace Horo::Tests::UiAccess {
    TEST_CASE("Busy native retirement revokes UI admission before actual system generation leases drain",
              "[runtime-ui][native][lifetime]") {
        Fixture fixture;
        auto anchor = fixture.Client();
        Gameplay::GameModuleHost host{Gameplay::GameModuleHostOptions{{}, {.runtimeUi = anchor}}};
        auto candidate = Take(host.Load(HORO_TEST_GAME_MODULE_PATH, {"game.tests", Gameplay::CurrentGameplayBuildFingerprint(),
                                                                     ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH)}));
        REQUIRE_FALSE(candidate.diagnostic);
        auto module = std::move(candidate.owner);
        auto retained = module->RuntimeUiContext();
        REQUIRE(retained != nullptr);
        REQUIRE(retained->Validate().HasValue());
        auto runtime = Take(Gameplay::GameplaySystemRuntime::Create(module->Systems(), module->ActiveServices(), module->Capabilities(),
                                                                    module->Cancellation()));
        module->RequestRetirement();
        CHECK(module->TryRetire().HasError());
        const auto revoked = retained->Validate();
        REQUIRE(revoked.HasError());
        CHECK(revoked.ErrorValue().code.Value() == UiErrors::CapabilityRevoked.code.Value());
        CHECK(anchor->Validate().HasValue());
        runtime.reset();
        REQUIRE(module->TryRetire().HasValue());
        CHECK(retained->Validate().HasError());
        CHECK(anchor->Validate().HasValue());
    }

    namespace {
        using Copy = void (*)(std::shared_ptr<const RuntimeUiClient> *);
        using Queue = void (*)(Result<RuntimeUiRequestId> *);

        /** @brief Resolves one exact fixture export while the caller pins its actual loaded native image. */
        template <class Function> Function Export(Platform::DynamicLibrary &image, const char *name) {
            void *symbol = image.GetSymbol(name);
            REQUIRE(symbol != nullptr);
            Function function{};
            static_assert(sizeof(function) == sizeof(symbol));
            std::memcpy(&function, &symbol, sizeof(function));
            return function;
        }

        /** @brief Proves retired AI access is denied and its host-owned control block releases after native unload. */
        void RequireRetiredAiChildReleased(std::shared_ptr<const Gameplay::GameplayAiContext> &child,
                                           const Gameplay::GameplayAiContext &anchor, AI::AgentId agent) {
            CHECK(child->ResolveAgent(agent).HasError());
            CHECK(anchor.ResolveAgent(agent).HasValue());
            std::weak_ptr<const Gameplay::GameplayAiContext> weak = child;
            child.reset();
            CHECK(weak.expired());
            weak.reset();
        }
    }  // namespace

    TEST_CASE("Native UI module unload revokes queued work and preserves host-issued client and weak storage destruction",
              "[runtime-ui][native][lifetime]") {
        Fixture fixture;
        auto anchor = fixture.Client();
        Gameplay::AiTestSupport::Fixture aiFixture;
        auto aiAnchor = Take(Gameplay::GameplayAiContext::Create({"game.tests", true, true}, aiFixture.runtime.CapabilityAtSafePoint()));
        auto host = std::make_unique<Gameplay::GameModuleHost>(Gameplay::GameModuleHostOptions{{}, {.ai = aiAnchor, .runtimeUi = anchor}});
        auto loaded = host->Load(HORO_TEST_GAME_MODULE_PATH, {"game.tests", Gameplay::CurrentGameplayBuildFingerprint(),
                                                              ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH)});
        REQUIRE(loaded.HasValue());
        REQUIRE_FALSE(loaded.Value().diagnostic);
        auto module = std::move(loaded).Value().owner;
        REQUIRE(module->RuntimeUiContext() != nullptr);
        auto escapedAi = module->AiContext();
        REQUIRE(escapedAi != nullptr);
        REQUIRE(escapedAi->ResolveAgent(aiFixture.scene.agent.agent.agent).HasValue());
        auto library = Platform::LoadDynamicLibrary(module->LoadedArtifactPath().string());
        REQUIRE(library.HasValue());
        auto image = std::move(library).Value();
        auto copy = Export<Copy>(*image, "HoroTestCopyRuntimeUiClient");
        auto queue = Export<Queue>(*image, "HoroTestQueueRuntimeUiScreen");
        std::shared_ptr<const RuntimeUiClient> escaped;
        copy(&escaped);
        REQUIRE(escaped != nullptr);
        REQUIRE(escaped->Validate().HasValue());
        auto queued = Result<RuntimeUiRequestId>::Failure(MakeError(UiErrors::CapabilityUnavailable));
        queue(&queued);
        REQUIRE(queued.HasValue());
        REQUIRE(module->PrepareReload().HasValue());
        CHECK(escaped->Validate().HasError());
        CHECK(escapedAi->ResolveAgent(aiFixture.scene.agent.agent.agent).HasError());
        module.reset();
        copy = nullptr;
        queue = nullptr;
        image.reset();
        CHECK(escaped->Validate().HasError());
        CHECK(anchor->Validate().HasValue());
        RequireRetiredAiChildReleased(escapedAi, *aiAnchor, aiFixture.scene.agent.agent.agent);
        CHECK(Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands)) == 1);
        CHECK(Take(anchor->ScreenGuard()).revision.Value() == 1);
        std::weak_ptr<const RuntimeUiClient> weak = escaped;
        escaped.reset();
        CHECK(weak.expired());
        weak.reset();
        host.reset();
        anchor.reset();
        fixture.service.Shutdown();
        REQUIRE(fixture.service.CollectRetired().HasValue());
        CHECK(Take(fixture.service.CanReclaim()));
    }
}  // namespace Horo::Tests::UiAccess
