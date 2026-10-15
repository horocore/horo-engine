#include "Horo/Gameplay/GameModule.h"
#include "Horo/Runtime/Ui/RuntimeUiClient.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#if defined(SDL_MAJOR_VERSION) || defined(JPH_OBJECT_LAYER_BITS) || defined(VK_VERSION_1_0)
#error "Runtime UI SDK exposed concrete backend headers"
#endif

namespace {
    using namespace Horo;
    using namespace Horo::Gameplay;
    using namespace Horo::Runtime::Ui;
    std::shared_ptr<const RuntimeUiClient> retained;

    class Module final : public IGameModule {
    public:
        Result<void> Register(GameRegistrationContext &) override {
            return Result<void>::Success();
        }

        Result<void> Start(GameRuntimeContext &context) override {
            if (!context.runtimeUi)
                return Result<void>::Failure(MakeError(UiErrors::CapabilityUnavailable));
            auto child = context.runtimeUi->Derive();
            if (child.HasError())
                return Result<void>::Failure(child.ErrorValue());
            retained = std::move(child).Value();
            SerializedUiId route{};
            route.back() = 9;
            const auto request = retained->Screen(UiRouteOperationRequest::Push(UiRouteId::Create(route).Value()));
            context.runtimeUi.reset();
            return request.HasError() ? Result<void>::Failure(request.ErrorValue()) : Result<void>::Success();
        }

        void Stop(GameRuntimeContext &) noexcept override {}
    };
}  // namespace

extern "C" HORO_GAME_EXPORT Horo::Gameplay::IGameModule *CreateGameModule() noexcept {
    return new Module{};
}

extern "C" HORO_GAME_EXPORT void DestroyGameModule(Horo::Gameplay::IGameModule *module) noexcept {
    delete module;
}

extern "C" HORO_GAME_EXPORT void HoroRuntimeUiSdkCopyClient(std::shared_ptr<const Horo::Runtime::Ui::RuntimeUiClient> *output) {
    if (output)
        *output = retained;
}
