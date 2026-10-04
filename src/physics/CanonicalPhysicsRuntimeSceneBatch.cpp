#include "CanonicalSceneBodyBatchState.h"

#include <cassert>

namespace Horo::Physics::Detail {
    /** @copydoc CanonicalSceneBodyBatch::CanonicalSceneBodyBatch */
    CanonicalSceneBodyBatch::CanonicalSceneBodyBatch(std::shared_ptr<CanonicalSceneBodyBatchState> state) noexcept
        : state_(std::move(state)) {}

    CanonicalSceneBodyBatch::~CanonicalSceneBodyBatch() {
        state_.reset();
    }

    CanonicalSceneBodyBatch::CanonicalSceneBodyBatch(CanonicalSceneBodyBatch &&) noexcept = default;
    CanonicalSceneBodyBatch &CanonicalSceneBodyBatch::operator=(CanonicalSceneBodyBatch &&) noexcept = default;

    /** @copydoc CanonicalSceneBodyBatch::Handles */
    std::span<const BodyHandle> CanonicalSceneBodyBatch::Handles() const noexcept {
        return state_ ? std::span<const BodyHandle>{state_->handles} : std::span<const BodyHandle>{};
    }

    /** @copydoc CanonicalSceneBodyBatch::Shapes */
    std::span<const ShapeHandle> CanonicalSceneBodyBatch::Shapes() const noexcept {
        return state_ ? std::span<const ShapeHandle>{state_->shapeHandles} : std::span<const ShapeHandle>{};
    }

    /** @copydoc CanonicalSceneBodyBatch::Constraints */
    std::span<const ConstraintHandle> CanonicalSceneBodyBatch::Constraints() const noexcept {
        return state_ ? std::span<const ConstraintHandle>{state_->constraintHandles} : std::span<const ConstraintHandle>{};
    }

    /** @copydoc CanonicalSceneBodyBatch::RetiredConstraints */
    std::span<const ConstraintHandle> CanonicalSceneBodyBatch::RetiredConstraints() const noexcept {
        return state_ ? std::span<const ConstraintHandle>{state_->retirement.retiredConstraints} : std::span<const ConstraintHandle>{};
    }

    namespace {
        /** @brief Builds the post-retirement collision exclusions before any native publication. */
        std::vector<std::uint64_t> RetainedCollisionPairs(const CanonicalWorld &world,
                                                          const std::span<const ConstraintHandle> retiredConstraints) {
            std::vector<std::uint64_t> pairs;
            pairs.reserve(world.scene.maximumConstraints);
            for (const auto &record : world.scene.constraints) {
                if (std::ranges::find(retiredConstraints, record.handle) != retiredConstraints.end() || record.secondBody.IsInvalid() ||
                    record.collisionPolicy != PhysicsJointCollisionPolicy::DisableBetweenBodies)
                    continue;
                const auto first = record.firstBody.GetIndexAndSequenceNumber();
                const auto second = record.secondBody.GetIndexAndSequenceNumber();
                const auto key = (static_cast<std::uint64_t>(std::min(first, second)) << 32U) | std::max(first, second);
                const auto position = std::ranges::lower_bound(pairs, key);
                if (position == pairs.end() || *position != key)
                    pairs.insert(position, key);
            }
            return pairs;
        }

        /** @brief Inserts one canonical ordered body pair only when the constraint disables collisions. */
        void AddCollisionPair(std::vector<std::uint64_t> &pairs, const CanonicalSceneConstraintRecord &record) {
            if (record.secondBody.IsInvalid() || record.collisionPolicy != PhysicsJointCollisionPolicy::DisableBetweenBodies)
                return;
            const auto first = record.firstBody.GetIndexAndSequenceNumber();
            const auto second = record.secondBody.GetIndexAndSequenceNumber();
            const auto key = (static_cast<std::uint64_t>(std::min(first, second)) << 32U) | std::max(first, second);
            const auto position = std::ranges::lower_bound(pairs, key);
            if (position == pairs.end() || *position != key)
                pairs.insert(position, key);
        }

        /** @brief Reserves native constraint storage without changing membership, order or enabled state. */
        void ReserveConstraintStorage(CanonicalWorld &world, const std::size_t additions) {
            // Supported capacity-only backend patch: no constraint has been registered or assigned a native index.
            const auto before = world.native.system->GetConstraints();
            std::vector<bool> enabled;
            enabled.reserve(before.size());
            for (const auto &constraint : before)
                enabled.push_back(constraint->GetEnabled());
            world.native.system->ReserveConstraints(static_cast<std::uint32_t>(world.scene.constraints.size() + additions));
            const auto after = world.native.system->GetConstraints();
            assert(before.size() == after.size());
            for (std::size_t index = 0; index < before.size(); ++index) {
                assert(before[index].GetPtr() == after[index].GetPtr());
                assert(before[index]->GetEnabled() == enabled[index]);
            }
        }

        /** @brief Resolves body/shape retirement and its dependent constraints without touching resident tables. */
        Result<void> ResolveRetirement(const CanonicalWorld &world, const std::span<const BodyHandle> bodies,
                                       const std::vector<BodyHandle> &retiringBodies, std::vector<ShapeHandle> &retiringShapes,
                                       std::vector<ConstraintHandle> &retiringConstraints, std::vector<JPH::BodyID> &nativeBodies,
                                       std::vector<JPH::Constraint *> &nativeConstraints) {
            for (const auto handle : bodies) {
                const auto found = std::ranges::find(world.scene.bodies, handle, &CanonicalSceneBodyRecord::handle);
                if (found == world.scene.bodies.end())
                    return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
                nativeBodies.push_back(found->nativeBody);
                const auto root = found->policy.shape;
                const bool shared = std::ranges::any_of(world.scene.bodies, [&](const auto &body) {
                    return body.policy.shape == root && std::ranges::find(retiringBodies, body.handle) == retiringBodies.end();
                });
                if (!shared && std::ranges::find(retiringShapes, root) == retiringShapes.end())
                    retiringShapes.push_back(root);
            }
            for (const auto handle : retiringShapes) {
                if (std::ranges::find(world.scene.shapes, handle, &CanonicalSceneShapeRecord::handle) == world.scene.shapes.end() ||
                    std::ranges::any_of(world.scene.bodies, [&](const auto &body) {
                    return body.policy.shape == handle && std::ranges::find(retiringBodies, body.handle) == retiringBodies.end();
                }))
                    return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
            }
            for (const auto &constraint : world.scene.constraints) {
                if ((std::ranges::find(retiringBodies, constraint.first) != retiringBodies.end() ||
                     std::ranges::find(retiringBodies, constraint.second) != retiringBodies.end()) &&
                    std::ranges::find(retiringConstraints, constraint.handle) == retiringConstraints.end())
                    retiringConstraints.push_back(constraint.handle);
            }
            for (const auto handle : retiringConstraints) {
                const auto found = std::ranges::find(world.scene.constraints, handle, &CanonicalSceneConstraintRecord::handle);
                if (found == world.scene.constraints.end())
                    return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
                nativeConstraints.push_back(found->constraint.GetPtr());
            }
            return Result<void>::Success();
        }

    }  // namespace

    /** @copydoc CanonicalSceneBodyBatch::PrepareRetirement */
    Result<void> CanonicalSceneBodyBatch::PrepareRetirement(const std::span<const BodyHandle> bodies,
                                                            const std::span<const ShapeHandle> shapes,
                                                            const std::span<const ConstraintHandle> constraints) const {
        if (!IsPending() || state_->retirement.retirementPrepared || state_->retirement.retirementFailed || state_->constraintsPrepared ||
            state_->constraintsFailed)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        state_->retirement.retirementFailed = true;
        const auto &world = *state_->world;
        if (bodies.size() > world.scene.maximumBodies || shapes.size() > world.scene.maximumShapes ||
            constraints.size() > world.scene.maximumConstraints)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        auto retiringBodies = std::vector<BodyHandle>{bodies.begin(), bodies.end()};
        auto retiringShapes = std::vector<ShapeHandle>{shapes.begin(), shapes.end()};
        auto retiringConstraints = std::vector<ConstraintHandle>{constraints.begin(), constraints.end()};
        std::vector<JPH::BodyID> nativeBodies;
        std::vector<JPH::Constraint *> nativeConstraints;
        nativeBodies.reserve(bodies.size());
        nativeConstraints.reserve(world.scene.constraints.size());
        retiringShapes.reserve(shapes.size() + bodies.size());
        retiringConstraints.reserve(world.scene.constraints.size());
        if (const auto unique = [](const auto &handles) {
            for (std::size_t index = 0; index < handles.size(); ++index)
                if (std::find(handles.begin(), handles.begin() + index, handles[index]) != handles.begin() + index)
                    return false;
            return true;
        }; !unique(retiringBodies) || !unique(retiringShapes) || !unique(retiringConstraints))
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (const auto resolved =
                ResolveRetirement(world, bodies, retiringBodies, retiringShapes, retiringConstraints, nativeBodies, nativeConstraints);
            resolved.HasError())
            return resolved;
        auto pairs = RetainedCollisionPairs(world, retiringConstraints);
        state_->retirement.retiredBodies = std::move(retiringBodies);
        state_->retirement.retiredShapes = std::move(retiringShapes);
        state_->retirement.retiredConstraints = std::move(retiringConstraints);
        state_->retirement.retiredNativeBodies = std::move(nativeBodies);
        state_->retirement.retiredNativeConstraints = std::move(nativeConstraints);
        state_->collisionPairs = std::move(pairs);
        state_->retirement.retirementPrepared = true;
        state_->retirement.retirementFailed = false;
        return Result<void>::Success();
    }

    /** @copydoc CanonicalSceneBodyBatch::PrepareConstraints */
    Result<void> CanonicalSceneBodyBatch::PrepareConstraints(const std::span<const PhysicsConstraintDescriptor> descriptors) const {
        if (!IsPending() || state_->retirement.retirementFailed || state_->constraintsPrepared || state_->constraintsFailed)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        state_->constraintsFailed = true;
        auto &world = *state_->world;
        if (descriptors.size() > 256 || descriptors.size() > world.scene.maximumConstraints - world.scene.constraints.size() ||
            descriptors.size() > std::numeric_limits<std::uint32_t>::max() - world.scene.nextConstraintSlot)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        std::vector<CanonicalSceneConstraintRecord> records;
        std::vector<ConstraintHandle> handles;
        std::vector<JPH::Constraint *> native;
        records.reserve(descriptors.size());
        handles.reserve(descriptors.size());
        native.reserve(descriptors.size());
        auto pairs = RetainedCollisionPairs(world, state_->retirement.retiredConstraints);
        for (const auto &descriptor : descriptors) {
            if (std::ranges::find(state_->retirement.retiredBodies, descriptor.first.body) != state_->retirement.retiredBodies.end() ||
                (std::holds_alternative<PhysicsBodyAnchor>(descriptor.second) &&
                 std::ranges::find(state_->retirement.retiredBodies, std::get<PhysicsBodyAnchor>(descriptor.second).body) !=
                     state_->retirement.retiredBodies.end()))
                return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
            auto prepared = PrepareCanonicalConstraintRecord({&world}, state_->owner, descriptor, state_->records);
            if (prepared.HasError())
                return Result<void>::Failure(prepared.ErrorValue());
            auto record = std::move(prepared).Value();
            AddCollisionPair(pairs, record);
            handles.push_back(record.handle);
            native.push_back(record.constraint.GetPtr());
            records.push_back(std::move(record));
        }
        ReserveConstraintStorage(world, records.size());
        state_->constraints = std::move(records);
        state_->constraintHandles = std::move(handles);
        state_->nativeConstraints = std::move(native);
        state_->collisionPairs = std::move(pairs);
        state_->constraintsPrepared = true;
        state_->constraintsFailed = false;
        return Result<void>::Success();
    }

    /** @copydoc CanonicalSceneBodyBatch::ValidatePublication */
    Result<void> CanonicalSceneBodyBatch::ValidatePublication() const {
        if (!IsPending() || state_->constraintsFailed || state_->retirement.retirementFailed)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        return Result<void>::Success();
    }

    /** @copydoc CanonicalSceneBodyBatch::IsPending */
    bool CanonicalSceneBodyBatch::IsPending() const noexcept {
        return state_ && state_->world && state_->world->pendingBodyBatch.lock() == state_;
    }

    /** @copydoc CanonicalSceneBodyBatch::Publish */
    void CanonicalSceneBodyBatch::Publish() const noexcept {
        if (!IsPending() || state_->constraintsFailed || state_->retirement.retirementFailed)
            return;
        auto &world = *state_->world;
        auto &bodies = world.native.system->GetBodyInterface();
        if (!state_->retirement.retiredNativeConstraints.empty())
            world.native.system->RemoveConstraints(state_->retirement.retiredNativeConstraints.data(),
                                                   static_cast<int>(state_->retirement.retiredNativeConstraints.size()));
        std::erase_if(world.scene.constraints, [this](const auto &record) {
            return std::ranges::find(state_->retirement.retiredConstraints, record.handle) != state_->retirement.retiredConstraints.end();
        });
        if (!state_->retirement.retiredNativeBodies.empty()) {
            bodies.RemoveBodies(state_->retirement.retiredNativeBodies.data(),
                                static_cast<int>(state_->retirement.retiredNativeBodies.size()));
            bodies.DestroyBodies(state_->retirement.retiredNativeBodies.data(),
                                 static_cast<int>(state_->retirement.retiredNativeBodies.size()));
        }
        std::erase_if(world.scene.bodies, [this](const auto &record) {
            return std::ranges::find(state_->retirement.retiredBodies, record.handle) != state_->retirement.retiredBodies.end();
        });
        std::erase_if(world.scene.shapes, [this](const auto &record) {
            return std::ranges::find(state_->retirement.retiredShapes, record.handle) != state_->retirement.retiredShapes.end();
        });
        if (state_->broadphasePrepared)
            bodies.AddBodiesFinalize(state_->nativeBodies.data(), static_cast<int>(state_->nativeBodies.size()), state_->addState,
                                     JPH::EActivation::DontActivate);
        for (auto &shape : state_->shapes)
            world.scene.shapes.push_back(std::move(shape));
        for (auto &record : state_->records)
            world.scene.bodies.push_back(std::move(record));
        if (state_->constraintsPrepared) {
            [[maybe_unused]] const auto capacity = world.native.system->GetConstraintCapacity();
            if (!state_->nativeConstraints.empty())
                world.native.system->AddConstraints(state_->nativeConstraints.data(), static_cast<int>(state_->nativeConstraints.size()));
            assert(world.native.system->GetConstraintCapacity() == capacity);
            for (auto &constraint : state_->constraints)
                world.scene.constraints.push_back(std::move(constraint));
        }
        if (state_->constraintsPrepared || state_->retirement.retirementPrepared)
            world.scene.disabledJointCollisionPairs.swap(state_->collisionPairs);
        if (!state_->awakeBodies.empty())
            bodies.ActivateBodies(state_->awakeBodies.data(), static_cast<int>(state_->awakeBodies.size()));
        world.pendingBodyBatch.reset();
        state_->world = nullptr;
        state_->broadphasePrepared = false;
    }

    /** @copydoc HasPendingCanonicalSceneBodies */
    bool HasPendingCanonicalSceneBodies(const CanonicalWorldHandle world) noexcept {
        return world.value && !static_cast<CanonicalWorld *>(world.value)->pendingBodyBatch.expired();
    }

    /** @copydoc CancelPendingCanonicalSceneBodies */
    void CancelPendingCanonicalSceneBodies(const CanonicalWorldHandle world) noexcept {
        if (world.value) {
            if (auto pending = static_cast<CanonicalWorld *>(world.value)->pendingBodyBatch.lock())
                pending->Abort();
        }
    }

}  // namespace Horo::Physics::Detail
