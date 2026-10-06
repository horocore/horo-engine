#include "Horo/Gameplay/GameplayPhysicsContext.h"

#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Physics/PhysicsWorld.h"

namespace Horo::Gameplay {
    namespace {
        /** @brief Validate the trusted principal and exact world/Scene epoch before issuing any client. */
        Result<void> ValidateBinding(const GameplayPhysicsBinding &binding, const Physics::PhysicsWorld &world) {
            if (binding.scene == 0 || binding.sceneGeneration == 0 || !binding.world.IsValid() || binding.world != world.Identity() ||
                !binding.moduleId.starts_with("game.") || GameplayCapabilityId::Parse(binding.moduleId + ".physics").HasError())
                return Result<void>::Failure(MakeError(GameplayErrors::InvalidCapabilityId));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc GameplayPhysicsContext::GameplayPhysicsContext */
    GameplayPhysicsContext::GameplayPhysicsContext(ConstructionKey, const GameplayPhysicsBinding &binding, CancellationSource revocation,
                                                   Physics::PhysicsQueryEventCapability capability,
                                                   std::optional<GameplayPhysicsDenial> denial)
        : binding_(binding), revocation_(std::move(revocation)), capability_(std::move(capability)), denial_(denial) {}

    /** @copydoc GameplayPhysicsContext::Withheld */
    std::shared_ptr<GameplayPhysicsContext> GameplayPhysicsContext::Withheld(const GameplayPhysicsBinding &binding,
                                                                             const GameplayPhysicsDenial denial) {
        return std::make_shared<GameplayPhysicsContext>(ConstructionKey{}, binding, CancellationSource{},
                                                        Physics::PhysicsQueryEventCapability{}, denial);
    }

    GameplayPhysicsContext::~GameplayPhysicsContext() {
        Revoke();
    }

    /** @copydoc GameplayPhysicsContext::Create */
    Result<std::shared_ptr<GameplayPhysicsContext>> GameplayPhysicsContext::Create(const GameplayPhysicsBinding &binding,
                                                                                   Physics::PhysicsWorld *world,
                                                                                   CancellationToken cancellation) {
        if (world && world->State() == Physics::PhysicsWorldState::ActiveNull)
            return Result<std::shared_ptr<GameplayPhysicsContext>>::Failure(MakeError(GameplayErrors::PhysicsUnavailable));
        if (!binding.permissionGranted)
            return Result<std::shared_ptr<GameplayPhysicsContext>>::Failure(MakeError(GameplayErrors::PhysicsPermissionDenied));
        if (!binding.moduleEnabled || world == nullptr)
            return Result<std::shared_ptr<GameplayPhysicsContext>>::Failure(MakeError(GameplayErrors::PhysicsUnavailable));
        if (const auto valid = ValidateBinding(binding, *world); valid.HasError())
            return Result<std::shared_ptr<GameplayPhysicsContext>>::Failure(valid.ErrorValue());
        try {
            CancellationSource revocation{cancellation};
            auto capability = world->IssueQueryEventCapability(revocation.Token());
            if (capability.HasError())
                return Result<std::shared_ptr<GameplayPhysicsContext>>::Failure(capability.ErrorValue());
            return Result<std::shared_ptr<GameplayPhysicsContext>>::Success(
                std::make_shared<GameplayPhysicsContext>(ConstructionKey{}, binding, revocation, std::move(capability).Value()));
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<GameplayPhysicsContext>>::Failure(MakeError(Physics::PhysicsErrors::CapacityExceeded));
        }
    }

    /** @copydoc GameplayPhysicsContext::Acquire */
    Result<Physics::PhysicsQueryEventCapability> GameplayPhysicsContext::Acquire(const std::string_view moduleId, const std::uint64_t scene,
                                                                                 const std::uint64_t sceneGeneration) const {
        if (revocation_.Token().IsCancellationRequested())
            return Result<Physics::PhysicsQueryEventCapability>::Failure(MakeError(Physics::PhysicsErrors::CapabilityRevoked));
        if (denial_ == GameplayPhysicsDenial::Unavailable)
            return Result<Physics::PhysicsQueryEventCapability>::Failure(MakeError(GameplayErrors::PhysicsUnavailable));
        if (denial_ == GameplayPhysicsDenial::PermissionDenied)
            return Result<Physics::PhysicsQueryEventCapability>::Failure(MakeError(GameplayErrors::PhysicsPermissionDenied));
        if (moduleId != binding_.moduleId)
            return Result<Physics::PhysicsQueryEventCapability>::Failure(MakeError(GameplayErrors::PhysicsPermissionDenied));
        if (scene != binding_.scene || sceneGeneration != binding_.sceneGeneration)
            return Result<Physics::PhysicsQueryEventCapability>::Failure(MakeError(Physics::PhysicsErrors::HandleWorldMismatch));
        return Result<Physics::PhysicsQueryEventCapability>::Success(capability_);
    }

    /** @copydoc GameplayPhysicsContext::AcquireCharacterClearance */
    Result<Physics::CharacterClearanceQuery> GameplayPhysicsContext::AcquireCharacterClearance(
        const std::string_view moduleId, const std::uint64_t scene, const Character::CharacterPhysicsQueryExpectations expected) const {
        auto capability = Acquire(moduleId, scene, expected.sceneGeneration);
        if (capability.HasError())
            return Result<Physics::CharacterClearanceQuery>::Failure(capability.ErrorValue());
        return Physics::CharacterClearanceQuery::Capture(std::move(capability).Value(), expected);
    }

    /** @copydoc GameplayPhysicsContext::Revoke */
    void GameplayPhysicsContext::Revoke() const noexcept {
        revocation_.RequestCancellation();
    }

    /** @copydoc GameplayPhysicsContext::Binding */
    const GameplayPhysicsBinding &GameplayPhysicsContext::Binding() const noexcept {
        return binding_;
    }
}  // namespace Horo::Gameplay
