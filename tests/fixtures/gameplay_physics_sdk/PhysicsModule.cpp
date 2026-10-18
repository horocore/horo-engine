#include "Horo/Assets/AssetCookInputSnapshot.h"
#include "Horo/Gameplay/GameModule.h"
#include "Horo/Gameplay/GameplayCharacterContext.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Gameplay/GameplayPhysicsContext.h"
#include "Horo/Physics/PhysicsContinuousCollision.h"
#include "Horo/Physics/PhysicsWorld.h"

#include <array>
#include <optional>
#include <type_traits>

static_assert(std::is_same_v<decltype(Horo::Physics::PhysicsWorldSimulationBinding{}.schema),
                             std::shared_ptr<const Horo::Physics::NormalizedCollisionSchema>>);
static_assert(std::is_same_v<decltype(Horo::Physics::PhysicsSceneBodyDescriptor{}.collision),
                             std::optional<Horo::Physics::PhysicsSceneCollisionBinding>>);

#if __has_include(<Jolt/Jolt.h>)
#error "The gameplay Physics SDK leaked native solver headers"
#endif
#if defined(JPH_OBJECT_LAYER_BITS) || defined(JPH_DOUBLE_PRECISION)
#error "The gameplay Physics SDK leaked native solver configuration"
#endif

using namespace Horo;
using namespace Horo::Gameplay;

namespace {
    std::optional<Physics::PhysicsQueryBatchHandle> retainedBatch;

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
        retainedBatch = queued.Value();
        if (pending.Value() || !queued.Value().Cancel())
            return Result<void>::Failure(MakeError(Physics::PhysicsErrors::DescriptorInvalid));
        return Result<void>::Success();
    }

    class Module final : public IGameModule {
    public:
        Result<void> Register(GameRegistrationContext &) override {
            // The source-free exported Assets archive must supply the additive host factory symbol.
            // An empty root rejects before any filesystem access and grants no project authority.
            const auto rejected = Assets::AssetCookInputSnapshot::CaptureSelected({}, {}, {}, {}, 1);
            return rejected.HasError() ? Result<void>::Success()
                                       : Result<void>::Failure(MakeError(GameplayErrors::InvalidBehaviorComponent));
        }

        Result<void> Start(GameRuntimeContext &context) override {
            // Public value validation is linked from the SDK; it grants no native world mutation authority.
            const auto ccd =
                Physics::ValidatePhysicsContinuousCollisionPolicy({Physics::PhysicsDefaultMotionQuality::LinearCast, 0.5F, 0.25F});
            if (ccd.HasError())
                return ccd;
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
            if (client.HasValue()) {
                auto verified = VerifyPhysicsOperations(client.Value(), binding);
                if (verified.HasError())
                    return verified;
            }
            if (!context.character)
                return Result<void>::Failure(MakeError(GameplayErrors::CharacterUnavailable));
            const auto &characterBinding = context.character->Binding();
            auto granted = context.character->Acquire("game.physics_sdk", characterBinding.scene, characterBinding.sceneGeneration);
            if (granted.HasError()) {
                if (granted.ErrorValue().code.Value() != GameplayErrors::CharacterUnavailable.code.Value() &&
                    granted.ErrorValue().code.Value() != GameplayErrors::CharacterPermissionDenied.code.Value())
                    return Result<void>::Failure(granted.ErrorValue());
                return Result<void>::Success();
            }
            auto child = granted.Value().Derive();
            if (child.HasError())
                return Result<void>::Failure(child.ErrorValue());
            character_ = std::move(child).Value();
            return character_.Validate();
        }

        void Stop(GameRuntimeContext &) noexcept override {}

    private:
        Character::CharacterCapability character_;
    };
}  // namespace

extern "C" HORO_GAME_EXPORT IGameModule *CreateGameModule() noexcept {
    return new Module{};
}

extern "C" HORO_GAME_EXPORT void DestroyGameModule(IGameModule *module) noexcept {
    delete module;
}

/** @brief Exports the actual module-created terminal handle for host teardown after the DLL has unloaded. */
extern "C" HORO_GAME_EXPORT void HoroTestCopyPhysicsBatch(std::optional<Physics::PhysicsQueryBatchHandle> *destination) {
    *destination = retainedBatch;
}
