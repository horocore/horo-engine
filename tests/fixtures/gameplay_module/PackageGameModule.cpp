#include "Horo/Gameplay/GameEventRegistry.h"
#include "Horo/Gameplay/GameModule.h"
#include "Horo/Gameplay/GameServiceRegistry.h"

#include <memory>

namespace {
    using namespace Horo;
    using namespace Horo::Gameplay;

    /** @brief Actual native service whose destruction remains owned by its module image. */
    class PackageService final : public IGameplayService {
    public:
        Result<void> Start(const GameplayServiceContext &) override {
            return Result<void>::Success();
        }

        void Stop(const GameplayServiceContext &) noexcept override {
            // This stateless service has no work or resources to retire.
        }
    };

    /** @brief Minimal real SDK module for package activation, callback pinning and native retirement tests. */
    class PackageModule final : public IGameModule {
    public:
        Result<void> Register(GameRegistrationContext &context) override {
            if (auto registered = context.events.Register({90, 1, 91, 1, BorrowedCallbackContext{this}, &Invoke}); registered.HasError())
                return registered;
            return context.services.Register({
                .descriptor = {.id = GameplayServiceId::Parse("game.tests.package_service").Value(),
                               .scope = GameplayServiceScope::Project,
                               .affinity = GameplayThreadAffinity::RuntimeOwner,
                               .sceneReplacement = GameplaySceneReplacementPolicy::Preserve,
                               .observabilityCategory = "game.tests.packages"},
                .factory = {.create = &CreateService, .destroy = &DestroyService},
            });
        }

        Result<void> Start(GameRuntimeContext &) override {
            return Result<void>::Success();
        }

        void Stop(GameRuntimeContext &) noexcept override {
            // Registrations and service retirement remain owned by the native module host.
        }

    private:
        /** @brief Keeps an actual module-resident callback behind the host's generation lease. */
        static GameplayEventOutcome Invoke(const BorrowedCallbackContext &, const GameplayEventRequest &) {
            return GameplayEventOutcome::Accepted;
        }

        /** @brief Constructs the package's only module-owned service. */
        static IGameplayService *CreateService(void *) {  // NOSONAR(cpp:S5008) The SDK factory ABI requires an opaque, unused context.
            return std::make_unique<PackageService>().release();
        }

        /** @brief Destroys the service while its owning native image is still retained. */
        static void DestroyService(
            void *, IGameplayService *service) noexcept {  // NOSONAR(cpp:S5008) The SDK factory ABI requires an opaque context.
            const std::unique_ptr<IGameplayService> owned{service};
        }
    };
}  // namespace

/** @brief Constructs the real package fixture module through the supported native SDK boundary. */
extern "C" HORO_GAME_EXPORT Horo::Gameplay::IGameModule *CreateGameModule() noexcept {
    return std::make_unique<PackageModule>().release();
}

/** @brief Releases the package fixture module before its native image is unloaded. */
extern "C" HORO_GAME_EXPORT void DestroyGameModule(Horo::Gameplay::IGameModule *gameModule) noexcept {
    const std::unique_ptr<Horo::Gameplay::IGameModule> owned{gameModule};
}
