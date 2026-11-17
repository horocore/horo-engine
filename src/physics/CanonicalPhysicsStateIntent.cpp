#include "CanonicalPhysicsStateIntent.h"

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Uses the canonical indexed owner table and exact native identity, not
         * a duplicate body registry. */
        const CanonicalSceneBodyRecord *Record(const CanonicalWorld &world, const CanonicalPhysicsStateBodyBinding &binding) noexcept {
            const JPH::BodyID native{binding.nativeId};
            if (native.GetIndex() >= world.scene.nativeBodyIndices.size())
                return nullptr;
            const auto index = world.scene.nativeBodyIndices[native.GetIndex()];
            if (index >= world.scene.bodies.size())
                return nullptr;
            const auto &body = world.scene.bodies[index];
            return body.handle == binding.body && body.nativeBody == native ? &body : nullptr;
        }

        /** @brief Only explicit last-applied velocity intent changes; immutable
         * settings/shape/mass policy remain compatibility inputs. */
        void SetIntent(PhysicsBodyDescriptor &policy, const PhysicsStateBody &body) noexcept {
            policy.linearVelocity = {body.lastLinearIntent[0], body.lastLinearIntent[1], body.lastLinearIntent[2]};
            policy.angularVelocity = {body.lastAngularIntent[0], body.lastAngularIntent[1], body.lastAngularIntent[2]};
        }
    }  // namespace

    /** @copydoc CaptureCanonicalStateIntent */
    Result<void> CaptureCanonicalStateIntent(const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings,
                                             StateDetail::SolverStorage &storage, const PhysicsStateSolverView &state) {
        auto bodies = std::span{storage.bodies}.first(state.bodies.size());
        for (const auto &record : world.scene.bodies) {
            const auto *binding = FindNativeStateBody(bindings, record.nativeBody.GetIndexAndSequenceNumber());
            if (!binding)
                return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
            const auto body = std::ranges::lower_bound(bodies, binding->id, {}, &PhysicsStateBody::id);
            if (body == bodies.end() || body->id != binding->id)
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            body->hasPolicyIntent = true;
            body->lastLinearIntent = {record.policy.linearVelocity.x, record.policy.linearVelocity.y, record.policy.linearVelocity.z};
            body->lastAngularIntent = {record.policy.angularVelocity.x, record.policy.angularVelocity.y, record.policy.angularVelocity.z};
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidateCanonicalStateIntent */
    Result<void> ValidateCanonicalStateIntent(const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings,
                                              const PhysicsStateSolverView &state) {
        for (const auto &body : state.bodies) {
            const auto *binding = FindStableStateBody(bindings, body.id);
            if (!binding || binding->hasPolicyIntent != body.hasPolicyIntent)
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            if (!body.hasPolicyIntent)
                continue;
            const auto *record = Record(world, *binding);
            if (!record)
                return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
            auto policy = record->policy;
            SetIntent(policy, body);
            if (auto valid = ValidatePhysicsBodyDescriptor(policy, record->handle.world); valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }

    /** @copydoc ApplyCanonicalStateIntent */
    Result<void> ApplyCanonicalStateIntent(CanonicalWorld &candidate, const CanonicalPhysicsStateBindings &bindings,
                                           const PhysicsStateSolverView &state) {
        if (auto valid = ValidateCanonicalStateIntent(candidate, bindings, state); valid.HasError())
            return valid;
        for (const auto &body : state.bodies) {
            if (!body.hasPolicyIntent)
                continue;
            const auto *binding = FindStableStateBody(bindings, body.id);
            const JPH::BodyID native{binding->nativeId};
            const auto index = candidate.scene.nativeBodyIndices[native.GetIndex()];
            SetIntent(candidate.scene.bodies[index].policy, body);
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
