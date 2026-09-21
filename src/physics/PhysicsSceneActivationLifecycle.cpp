#include "Horo/Physics/CharacterWorld.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsSceneActivation.h"

#include <limits>
#include <ranges>
#include <thread>
#include <utility>

namespace Horo::Physics {
    /** @copydoc PhysicsSceneActivationCandidate::PhysicsSceneActivationCandidate */
    PhysicsSceneActivationCandidate::PhysicsSceneActivationCandidate(std::unique_ptr<PhysicsWorld> physics,
                                                                     std::unique_ptr<Character::CharacterWorld> character,
                                                                     const PhysicsSceneActivationAuthority &authority,
                                                                     const PhysicsSceneActivationEvidence evidence,
                                                                     std::vector<PhysicsSceneBodyBinding> bodies,
                                                                     std::vector<PhysicsSceneShapeBinding> shapes,
                                                                     std::vector<PhysicsSceneConstraintBinding> constraints) noexcept
        : physics_(std::move(physics)), character_(std::move(character)), authority_(&authority), evidence_(evidence),
          bodyBindings_(std::move(bodies)), shapeBindings_(std::move(shapes)), constraintBindings_(std::move(constraints)) {}

    /** @copydoc PhysicsSceneActivationCandidate::~PhysicsSceneActivationCandidate */
    PhysicsSceneActivationCandidate::~PhysicsSceneActivationCandidate() = default;

    /** @copydoc PhysicsSceneActivationCandidate::ValidatePublication */
    Result<void> PhysicsSceneActivationCandidate::ValidatePublication() const {
        if (authority_ == nullptr || !authority_->IsCurrent(evidence_))
            return Result<void>::Failure(MakeError(PhysicsErrors::QuerySnapshotStale));
        if (!physics_ || !character_ ||
            (physics_->State() != PhysicsWorldState::ActiveSolver && physics_->State() != PhysicsWorldState::ActiveNull) ||
            character_->State() != Character::CharacterWorldState::Active)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        return Result<void>::Success();
    }

    /** @copydoc PhysicsSceneActivationCandidate::Shutdown */
    void PhysicsSceneActivationCandidate::Shutdown() noexcept {
        if (character_)
            character_->Shutdown();
        if (physics_)
            physics_->Shutdown();
        bodyBindings_.clear();
        shapeBindings_.clear();
        constraintBindings_.clear();
    }

    /** @copydoc PhysicsSceneActivationCandidate::WorldIdentity */
    PhysicsWorldId PhysicsSceneActivationCandidate::WorldIdentity() const noexcept {
        return physics_ ? physics_->Identity() : PhysicsWorldId{};
    }

    /** @copydoc PhysicsSceneActivationCandidate::BodyBindings */
    std::span<const PhysicsSceneBodyBinding> PhysicsSceneActivationCandidate::BodyBindings() const noexcept {
        return bodyBindings_;
    }

    /** @copydoc PhysicsSceneActivationCandidate::ShapeBindings */
    std::span<const PhysicsSceneShapeBinding> PhysicsSceneActivationCandidate::ShapeBindings() const noexcept {
        return shapeBindings_;
    }

    /** @copydoc PhysicsSceneActivationCandidate::ConstraintBindings */
    std::span<const PhysicsSceneConstraintBinding> PhysicsSceneActivationCandidate::ConstraintBindings() const noexcept {
        return constraintBindings_;
    }

    /** @copydoc PhysicsSceneActivationCandidate::FindBody */
    std::optional<BodyHandle> PhysicsSceneActivationCandidate::FindBody(const Runtime::SceneObjectId object,
                                                                        const Runtime::PhysicsBodySlotId body) const noexcept {
        const auto found = std::ranges::find_if(bodyBindings_, [object, body](const auto &binding) {
            return binding.object == object && binding.body == body;
        });
        return found == bodyBindings_.end() ? std::nullopt : std::optional<BodyHandle>{found->handle};
    }

    /** @copydoc PhysicsSceneActivationCandidate::FindShape */
    std::optional<ShapeHandle> PhysicsSceneActivationCandidate::FindShape(const Runtime::SceneObjectId object,
                                                                          const Runtime::PhysicsColliderSlotId collider) const noexcept {
        const auto found = std::ranges::find_if(shapeBindings_, [object, collider](const auto &binding) {
            return binding.object == object && binding.collider == collider;
        });
        return found == shapeBindings_.end() ? std::nullopt : std::optional<ShapeHandle>{found->handle};
    }

    /** @copydoc PhysicsSceneActivationCandidate::FindConstraint */
    std::optional<ConstraintHandle> PhysicsSceneActivationCandidate::FindConstraint(
        const Runtime::SceneObjectId object, const Runtime::PhysicsConstraintSlotId constraint) const noexcept {
        const auto found = std::ranges::find_if(constraintBindings_, [object, constraint](const auto &binding) {
            return binding.object == object && binding.constraint == constraint;
        });
        return found == constraintBindings_.end() ? std::nullopt : std::optional<ConstraintHandle>{found->handle};
    }

    /** @copydoc PhysicsSceneActivationAuthority::PhysicsSceneActivationAuthority */
    PhysicsSceneActivationAuthority::PhysicsSceneActivationAuthority() noexcept : ownerThread_(std::this_thread::get_id()) {}

    /** @copydoc PhysicsSceneActivationAuthority::Capture */
    PhysicsSceneActivationEvidence PhysicsSceneActivationAuthority::Capture() const noexcept {
        return current_;
    }

    /** @copydoc PhysicsSceneActivationAuthority::IsCurrent */
    bool PhysicsSceneActivationAuthority::IsCurrent(const PhysicsSceneActivationEvidence evidence) const noexcept {
        return std::this_thread::get_id() == ownerThread_ && evidence == current_;
    }

    /** @copydoc PhysicsSceneActivationAuthority::Advance */
    Result<void> PhysicsSceneActivationAuthority::Advance(std::uint64_t &generation) const {
        if (std::this_thread::get_id() != ownerThread_)
            return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (generation == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(PhysicsErrors::GenerationExhausted));
        ++generation;
        return Result<void>::Success();
    }

    /** @copydoc PhysicsSceneActivationAuthority::AdvanceCollisionFilterGeneration */
    Result<void> PhysicsSceneActivationAuthority::AdvanceCollisionFilterGeneration() {
        return Advance(current_.collisionFilterGeneration);
    }

    /** @copydoc PhysicsSceneActivationAuthority::AdvanceOriginGeneration */
    Result<void> PhysicsSceneActivationAuthority::AdvanceOriginGeneration() {
        return Advance(current_.originGeneration);
    }
}  // namespace Horo::Physics
