#include "Horo/Gameplay/GameModule.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Gameplay/GameplayPhysicsContext.h"

#include <array>

#if __has_include(<Jolt/Jolt.h>)
#error "The gameplay Physics SDK leaked native solver headers"
#endif
#if defined(JPH_OBJECT_LAYER_BITS) || defined(JPH_DOUBLE_PRECISION)
#error "The gameplay Physics SDK leaked native solver configuration"
#endif

using namespace Horo;
using namespace Horo::Gameplay;

namespace {
    /** @brief Exercise the exported Physics operations and pending-batch cancellation from a real external module. */
    Result<void> VerifyPhysicsOperations(const Physics::PhysicsQueryEventCapability &client, const GameplayPhysicsBinding &binding) {
        std::array<Physics::PhysicsEventRecord, 1> events{};
        const auto read = client.ReadEvents({client.Identity(), 1, 1, 1}, events);
        if (read.HasError())
            return Result<void>::Failure(read.ErrorValue());
        Physics::PhysicsQueryDescriptor descriptor;
        descriptor.world = binding.world;
        descriptor.sceneGeneration = binding.sceneGeneration;
        descriptor.geometry = Physics::PhysicsRayQuery{.maximumDistanceMeters = 10};
        descriptor.filter.channel = Physics::PhysicsQueryChannelId::Parse("12345678-1234-4234-8234-123456789abc").Value();
        const Physics::PhysicsQueryCommand command{client.Identity(), 1, descriptor};
        std::array<Physics::PhysicsQueryHit, 1> hits{};
        const auto queried = client.Submit(command, hits);
        if (queried.HasError())
            return Result<void>::Failure(queried.ErrorValue());
        const auto queued = client.SubmitBatch(std::span{&command, 1});
        if (queued.HasError())
            return Result<void>::Failure(queued.ErrorValue());
        const auto pending = queued.Value().Poll();
        if (pending.HasError())
            return Result<void>::Failure(pending.ErrorValue());
        if (pending.Value() || !queued.Value().Cancel())
            return Result<void>::Failure(MakeError(Physics::PhysicsErrors::DescriptorInvalid));
        return Result<void>::Success();
    }

    class Module final : public IGameModule {
    public:
        Result<void> Register(GameRegistrationContext &) override {
            return Result<void>::Success();
        }

        Result<void> Start(GameRuntimeContext &context) override {
            if (!context.physics)
                return Result<void>::Failure(MakeError(GameplayErrors::PhysicsUnavailable));
            const auto &binding = context.physics->Binding();
            const auto client = context.physics->Acquire("game.physics_sdk", binding.scene, binding.sceneGeneration);
            if (binding.world.IsValid() && client.HasError())
                return Result<void>::Failure(client.ErrorValue());
            if (client.HasValue() && client.Value().Identity().world != binding.world)
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::HandleWorldMismatch));
            if (client.HasError() && client.ErrorValue().code.Value() != GameplayErrors::PhysicsUnavailable.code.Value() &&
                client.ErrorValue().code.Value() != GameplayErrors::PhysicsPermissionDenied.code.Value())
                return Result<void>::Failure(client.ErrorValue());
            if (client.HasValue())
                return VerifyPhysicsOperations(client.Value(), binding);
            return Result<void>::Success();
        }

        void Stop(GameRuntimeContext &) noexcept override {}
    };
}  // namespace

extern "C" HORO_GAME_EXPORT IGameModule *CreateGameModule() noexcept {
    return new Module{};
}

extern "C" HORO_GAME_EXPORT void DestroyGameModule(IGameModule *module) noexcept {
    delete module;
}
