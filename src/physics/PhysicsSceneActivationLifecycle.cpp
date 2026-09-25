#include "Horo/Physics/CharacterWorld.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsSceneActivation.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <ranges>
#include <thread>
#include <utility>

namespace Horo::Physics {
    /** @copydoc PhysicsSceneActivationCandidate::PhysicsSceneActivationCandidate */
    PhysicsSceneActivationCandidate::PhysicsSceneActivationCandidate(ConstructionData data) noexcept
        : physics_(std::move(data.physics)), character_(std::move(data.character)), runtime_(data.runtime), authority_(data.authority),
          evidence_(data.evidence), bodyBindings_(std::move(data.bodyBindings)), shapeBindings_(std::move(data.shapeBindings)),
          constraintBindings_(std::move(data.constraintBindings)) {}

    std::unique_ptr<PhysicsSceneActivationCandidate> PhysicsSceneActivationCandidate::Create(ConstructionData data) {
        return std::make_unique<PhysicsSceneActivationCandidate>(std::move(data));
    }

    /** @copydoc PhysicsSceneActivationCandidate::~PhysicsSceneActivationCandidate */
    PhysicsSceneActivationCandidate::~PhysicsSceneActivationCandidate() = default;

    /** @copydoc PhysicsSceneActivationCandidate::ValidatePublication */
    Result<void> PhysicsSceneActivationCandidate::ValidatePublication() const {
        if (authority_ == nullptr || !authority_->IsCurrent(evidence_))
            return Result<void>::Failure(MakeError(PhysicsErrors::QuerySnapshotStale));
        if (runtime_ == nullptr || runtime_->State() != PhysicsRuntimeState::Ready)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState, "The Physics runtime is no longer available."));
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

    /** @copydoc PhysicsPlayWorldSession::PhysicsPlayWorldSession */
    PhysicsPlayWorldSession::PhysicsPlayWorldSession(PhysicsRuntime &runtime, PhysicsSceneActivationSettings settings) noexcept
        : ownerThread_(std::this_thread::get_id()), participant_(runtime, authority_, std::move(settings)) {}

    /** @copydoc PhysicsPlayWorldSession::~PhysicsPlayWorldSession */
    PhysicsPlayWorldSession::~PhysicsPlayWorldSession() {
        assert(std::this_thread::get_id() == ownerThread_);
        Retire(pending_);
        Retire(active_);
    }

    /** @copydoc PhysicsPlayWorldSession::CheckOwner */
    Result<void> PhysicsPlayWorldSession::CheckOwner() const {
        if (std::this_thread::get_id() != ownerThread_)
            return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        return Result<void>::Success();
    }

    /** @copydoc PhysicsPlayWorldSession::CheckSafePoint */
    Result<void> PhysicsPlayWorldSession::CheckSafePoint(const Runtime::RuntimePhase phase) const {
        if (const Result<void> owner = CheckOwner(); owner.HasError())
            return owner;
        if (phase != Runtime::RuntimePhase::CommitDeferredLifecycleChanges)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::InvalidState, "Play-world publication and stop require CommitDeferredLifecycleChanges."));
        return Result<void>::Success();
    }

    /** @copydoc PhysicsPlayWorldSession::Retire */
    void PhysicsPlayWorldSession::Retire(std::unique_ptr<Runtime::SceneActivationCandidate> &candidate) const noexcept {
        if (candidate)
            candidate->Shutdown();
        candidate.reset();
    }

    /** @copydoc PhysicsPlayWorldSession::ActivePhysics */
    const PhysicsSceneActivationCandidate *PhysicsPlayWorldSession::ActivePhysics() const noexcept {
        return static_cast<const PhysicsSceneActivationCandidate *>(active_.get());
    }

    PhysicsSceneActivationCandidate *PhysicsPlayWorldSession::ActivePhysics() noexcept {
        return static_cast<PhysicsSceneActivationCandidate *>(active_.get());
    }

    /** @copydoc PhysicsPlayWorldSession::Prepare */
    Result<void> PhysicsPlayWorldSession::Prepare(const Runtime::RuntimeSceneDefinition &definition,
                                                  const Runtime::RuntimeSceneView scene) {
        if (const Result<void> owner = CheckOwner(); owner.HasError())
            return owner;
        if (pending_)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState, "A play-world candidate is already pending."));
        auto prepared = participant_.Prepare(definition, scene);
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        pendingScene_ = scene.RuntimeId();
        pendingDefinition_ = definition.Id();
        pendingRevision_ = definition.Revision();
        pendingAssets_ = scene.AssetRegistryRevision();
        pending_ = std::move(prepared).Value();
        return Result<void>::Success();
    }

    /** @copydoc PhysicsPlayWorldSession::Commit */
    Result<void> PhysicsPlayWorldSession::Commit(const Runtime::RuntimePhase phase, const Runtime::RuntimeSceneView source) {
        if (const Result<void> safePoint = CheckSafePoint(phase); safePoint.HasError())
            return safePoint;
        if (!pending_)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState, "No play-world candidate is pending."));
        if (!source.IsCurrent() || source.RuntimeId() != pendingScene_ || source.DefinitionId() != pendingDefinition_ ||
            source.DefinitionRevision() != pendingRevision_ || source.AssetRegistryRevision() != pendingAssets_) {
            Retire(pending_);
            pendingScene_ = {};
            pendingDefinition_ = {};
            pendingRevision_ = {};
            pendingAssets_ = {};
            return Result<void>::Failure(
                MakeError(PhysicsErrors::QuerySnapshotStale, "The play-world source scene changed before publication."));
        }
        if (const Result<void> valid = pending_->ValidatePublication(); valid.HasError()) {
            Retire(pending_);
            pendingScene_ = {};
            pendingDefinition_ = {};
            pendingRevision_ = {};
            pendingAssets_ = {};
            return valid;
        }
        pending_->Publish();
        std::unique_ptr<Runtime::SceneActivationCandidate> old = std::move(active_);
        active_ = std::move(pending_);
        activeScene_ = pendingScene_;
        pendingScene_ = {};
        pendingDefinition_ = {};
        pendingRevision_ = {};
        pendingAssets_ = {};
        Retire(old);
        return Result<void>::Success();
    }

    /** @copydoc PhysicsPlayWorldSession::Stop */
    Result<void> PhysicsPlayWorldSession::Stop(const Runtime::RuntimePhase phase) {
        if (const Result<void> safePoint = CheckSafePoint(phase); safePoint.HasError())
            return safePoint;
        Retire(pending_);
        Retire(active_);
        pendingScene_ = {};
        pendingDefinition_ = {};
        pendingRevision_ = {};
        pendingAssets_ = {};
        activeScene_ = {};
        return Result<void>::Success();
    }

    /** @copydoc PhysicsPlayWorldSession::AdvanceFixedTick */
    Result<void> PhysicsPlayWorldSession::AdvanceFixedTick(const PhysicsFixedTickInput &input) {
        if (const Result<void> owner = CheckOwner(); owner.HasError())
            return owner;
        if (!active_)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState, "No play world is active."));
        if (input.sceneGeneration != activeScene_.value)
            return Result<void>::Failure(MakeError(PhysicsErrors::QuerySnapshotStale, "The play-world scene generation changed."));
        return ActivePhysics()->physics_->AdvanceFixedTick(input);
    }

    /** @copydoc PhysicsPlayWorldSession::PublishedTick */
    Result<PhysicsPublishedTick> PhysicsPlayWorldSession::PublishedTick() const {
        if (const Result<void> owner = CheckOwner(); owner.HasError())
            return Result<PhysicsPublishedTick>::Failure(owner.ErrorValue());
        if (!active_)
            return Result<PhysicsPublishedTick>::Failure(MakeError(PhysicsErrors::InvalidState, "No play world is active."));
        return Result<PhysicsPublishedTick>::Success(ActivePhysics()->physics_->PublishedTick());
    }

    /** @copydoc PhysicsPlayWorldSession::ResolveBody */
    Result<BodyHandle> PhysicsPlayWorldSession::ResolveBody(const Runtime::SceneObjectId object,
                                                            const Runtime::PhysicsBodySlotId body) const {
        if (const Result<void> owner = CheckOwner(); owner.HasError())
            return Result<BodyHandle>::Failure(owner.ErrorValue());
        if (!active_)
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::InvalidState, "No play world is active."));
        const std::optional<BodyHandle> found = ActivePhysics()->FindBody(object, body);
        if (!found)
            return Result<BodyHandle>::Failure(MakeError(PhysicsErrors::HandleStale, "The authored body has no active play binding."));
        return Result<BodyHandle>::Success(*found);
    }

    /** @copydoc PhysicsPlayWorldSession::ValidateBody */
    Result<void> PhysicsPlayWorldSession::ValidateBody(const BodyHandle &handle) const {
        if (const Result<void> owner = CheckOwner(); owner.HasError())
            return owner;
        if (!active_)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState, "No play world is active."));
        if (const Result<void> valid = ValidatePhysicsHandleOwner(handle, ActivePhysics()->WorldIdentity()); valid.HasError())
            return valid;
        if (std::ranges::none_of(ActivePhysics()->BodyBindings(), [&handle](const PhysicsSceneBodyBinding &binding) {
            return binding.handle == handle;
        }))
            return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
        return Result<void>::Success();
    }

    /** @copydoc PhysicsPlayWorldSession::WorldIdentity */
    PhysicsWorldId PhysicsPlayWorldSession::WorldIdentity() const noexcept {
        return active_ ? ActivePhysics()->WorldIdentity() : PhysicsWorldId{};
    }

    /** @copydoc PhysicsPlayWorldSession::IsActive */
    bool PhysicsPlayWorldSession::IsActive() const noexcept {
        return active_ != nullptr;
    }

    /** @copydoc PhysicsPlayWorldSession::HasPendingCandidate */
    bool PhysicsPlayWorldSession::HasPendingCandidate() const noexcept {
        return pending_ != nullptr;
    }
}  // namespace Horo::Physics
