#include "Horo/Gameplay/GameplayCharacterContext.h"

#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Physics/CharacterErrors.h"
#include "Horo/Physics/CharacterWorld.h"

#include <new>

namespace Horo::Gameplay {
    /** @copydoc GameplayCharacterContext::GameplayCharacterContext */
    GameplayCharacterContext::GameplayCharacterContext(ConstructionKey, GameplayCharacterBinding binding,
                                                       Character::CharacterCapability capability,
                                                       std::optional<GameplayCharacterDenial> denial)
        : binding_(std::move(binding)), capability_(std::move(capability)), denial_(denial) {}

    /** @copydoc GameplayCharacterContext::Withheld */
    std::shared_ptr<GameplayCharacterContext> GameplayCharacterContext::Withheld(const GameplayCharacterBinding &binding,
                                                                                 const GameplayCharacterDenial denial) {
        return std::make_shared<GameplayCharacterContext>(ConstructionKey{}, binding, Character::CharacterCapability{}, denial);
    }

    GameplayCharacterContext::~GameplayCharacterContext() {
        Revoke();
    }

    /** @copydoc GameplayCharacterContext::Create */
    Result<std::shared_ptr<GameplayCharacterContext>> GameplayCharacterContext::Create(const GameplayCharacterBinding &binding,
                                                                                       Character::CharacterWorld *world,
                                                                                       const GameplayPhysicsContext *physics,
                                                                                       CancellationToken cancellation) {
        using Output = Result<std::shared_ptr<GameplayCharacterContext>>;
        if (!binding.permissionGranted)
            return Output::Failure(MakeError(GameplayErrors::CharacterPermissionDenied));
        if (!binding.moduleEnabled || !world || !physics || world->State() != Character::CharacterWorldState::Active)
            return Output::Failure(MakeError(GameplayErrors::CharacterUnavailable));
        if (binding.scene == 0 || binding.sceneGeneration == 0 || !binding.moduleId.starts_with("game.") ||
            GameplayCapabilityId::Parse(binding.moduleId + ".character").HasError())
            return Output::Failure(MakeError(GameplayErrors::InvalidCapabilityId));
        if (const auto &owner = world->Descriptor(); binding.world != owner.identity || binding.sceneGeneration != owner.sceneGeneration ||
                                                     binding.physicsWorld != owner.physicsWorld ||
                                                     binding.physicsWorld != physics->Binding().world)
            return Output::Failure(MakeError(Character::CharacterErrors::HandleWorldMismatch));
        if (auto dependency = physics->Acquire(binding.moduleId, binding.scene, binding.sceneGeneration); dependency.HasError())
            return Output::Failure(dependency.ErrorValue());
        auto capability = world->IssueCapability(cancellation);
        if (capability.HasError())
            return Output::Failure(capability.ErrorValue());
        try {
            return Output::Success(std::make_shared<GameplayCharacterContext>(ConstructionKey{}, binding, std::move(capability).Value()));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(Character::CharacterErrors::CapacityExceeded));
        }
    }

    /** @copydoc GameplayCharacterContext::Acquire */
    Result<Character::CharacterCapability> GameplayCharacterContext::Acquire(const std::string_view moduleId, const std::uint64_t scene,
                                                                             const std::uint64_t sceneGeneration) const {
        using Output = Result<Character::CharacterCapability>;
        if (denial_ == GameplayCharacterDenial::Unavailable)
            return Output::Failure(MakeError(GameplayErrors::CharacterUnavailable));
        if (denial_ == GameplayCharacterDenial::PermissionDenied || moduleId != binding_.moduleId)
            return Output::Failure(MakeError(GameplayErrors::CharacterPermissionDenied));
        if (scene != binding_.scene || sceneGeneration != binding_.sceneGeneration)
            return Output::Failure(MakeError(Character::CharacterErrors::HandleWorldMismatch));
        if (auto valid = capability_.Validate(); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        return Output::Success(capability_);
    }

    /** @copydoc GameplayCharacterContext::ValidateDependency */
    Result<void> GameplayCharacterContext::ValidateDependency(const GameplayPhysicsContext *physics) const {
        if (denial_)
            return Result<void>::Success();
        if (!physics)
            return Result<void>::Failure(MakeError(GameplayErrors::CharacterUnavailable));
        if (physics->Binding().world != binding_.physicsWorld)
            return Result<void>::Failure(MakeError(Character::CharacterErrors::HandleWorldMismatch));
        if (const auto dependency = physics->Acquire(binding_.moduleId, binding_.scene, binding_.sceneGeneration); dependency.HasError())
            return Result<void>::Failure(dependency.ErrorValue());
        return Result<void>::Success();
    }

    /** @copydoc GameplayCharacterContext::Derive */
    Result<std::shared_ptr<GameplayCharacterContext>> GameplayCharacterContext::Derive() const {
        using Output = Result<std::shared_ptr<GameplayCharacterContext>>;
        try {
            if (denial_)
                return Output::Success(Withheld(binding_, *denial_));
            auto child = capability_.Derive();
            if (child.HasError())
                return Output::Failure(child.ErrorValue());
            return Output::Success(std::make_shared<GameplayCharacterContext>(ConstructionKey{}, binding_, std::move(child).Value()));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(Character::CharacterErrors::CapacityExceeded));
        }
    }

    /** @copydoc GameplayCharacterContext::Revoke */
    void GameplayCharacterContext::Revoke() const noexcept {
        capability_.Revoke();
    }

    /** @copydoc GameplayCharacterContext::Binding */
    const GameplayCharacterBinding &GameplayCharacterContext::Binding() const noexcept {
        return binding_;
    }
}  // namespace Horo::Gameplay
