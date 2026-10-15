#include "GameplayModuleTestSupport.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "RuntimeUiCapabilityTestSupport.h"

#include <cstring>

namespace Horo::Tests::UiAccess {
    namespace {
        /** @brief Resolves one exact fixture export while the caller pins its actual loaded native image. */
        template <class Function> Function Export(Platform::DynamicLibrary &image, const char *name) {
            void *symbol = image.GetSymbol(name);
            REQUIRE(symbol != nullptr);
            Function function{};
            static_assert(sizeof(function) == sizeof(symbol));
            std::memcpy(&function, &symbol, sizeof(function));
            return function;
        }
    }  // namespace

    TEST_CASE("Native UI module unload revokes queued work and preserves host-issued client and weak storage destruction",
              "[runtime-ui][native][lifetime]") {
        Fixture fixture;
        auto anchor = fixture.Client();
        Gameplay::GameModuleHost host{Gameplay::GameModuleHostOptions{{}, {.runtimeUi = anchor}}};
        auto loaded = host.Load(HORO_TEST_GAME_MODULE_PATH, {"game.tests", Gameplay::CurrentGameplayBuildFingerprint(),
                                                             ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH)});
        REQUIRE(loaded.HasValue());
        auto module = std::move(loaded).Value();
        REQUIRE(module->RuntimeUiContext() != nullptr);
        auto library = Platform::LoadDynamicLibrary(module->LoadedArtifactPath().string());
        REQUIRE(library.HasValue());
        auto image = std::move(library).Value();
        using Copy = void (*)(std::shared_ptr<const RuntimeUiClient> *);
        using Queue = Result<RuntimeUiRequestId> (*)();
        auto copy = Export<Copy>(*image, "HoroTestCopyRuntimeUiClient");
        auto queue = Export<Queue>(*image, "HoroTestQueueRuntimeUiScreen");
        std::shared_ptr<const RuntimeUiClient> escaped;
        copy(&escaped);
        REQUIRE(escaped != nullptr);
        REQUIRE(escaped->Validate().HasValue());
        REQUIRE(queue().HasValue());
        REQUIRE(module->PrepareReload().HasValue());
        CHECK(escaped->Validate().HasError());
        module.reset();
        copy = nullptr;
        queue = nullptr;
        image.reset();
        CHECK(escaped->Validate().HasError());
        CHECK(anchor->Validate().HasValue());
        CHECK(Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands)) == 1);
        CHECK(Take(anchor->ScreenGuard()).revision.Value() == 1);
        escaped.reset();
        anchor.reset();
        fixture.service.Shutdown();
        REQUIRE(fixture.service.CollectRetired().HasValue());
        CHECK(Take(fixture.service.CanReclaim()));
    }
}  // namespace Horo::Tests::UiAccess
