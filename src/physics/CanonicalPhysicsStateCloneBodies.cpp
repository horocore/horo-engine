#include "CanonicalPhysicsStateCloneInternal.h"

#include <type_traits>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Preserves unchecked mass for allocated motion records and exact
         * solver-active dynamic inertia, without calling an inactive asserting getter. */
        void PreserveMotionMass(const JPH::Body &source, JPH::Body &candidate) noexcept {
            const auto *original = source.GetMotionPropertiesUnchecked();
            if (!original)
                return;
            // GetBodyCreationSettings sets mAllowDynamicOrKinematic from this exact source pointer,
            // so CreateBodyWithID allocates the matching motion storage even for reserved static bodies.
            auto *copied = candidate.GetMotionPropertiesUnchecked();
            copied->SetInverseMass(original->GetInverseMassUnchecked());
            if (source.IsDynamic())
                copied->SetInverseInertia(original->GetInverseInertiaDiagonal(), original->GetInertiaRotation());
        }

        /** @brief Exact native IDs stay private, preserving existing Horo registrations
         * and solver contact orientation. */
        bool CloneBody(const CanonicalWorld &source, CanonicalWorld &candidate, JPH::BodyID id) {
            const auto *body = source.native.system->GetBodyLockInterfaceNoLock().TryGetBody(id);
            if (!body)
                return false;
            auto &interface = candidate.native.system->GetBodyInterface();
            auto *created = interface.CreateBodyWithID(id, body->GetBodyCreationSettings());
            if (!created)
                return false;
            PreserveMotionMass(*body, *created);
            interface.AddBody(id, JPH::EActivation::DontActivate);
            return true;
        }

        /** @brief Deep copy precedes native allocation; reserved vector capacity and
         * no-throw moves ensure complete cleanup tracking. */
        Result<void> SceneBodies(const CanonicalWorld &source, CanonicalWorld &candidate) {
            static_assert(std::is_nothrow_move_constructible_v<CanonicalSceneBodyRecord>);
            for (const auto &original : source.scene.bodies) {
                auto copied = original;
                if (!CloneBody(source, candidate, original.nativeBody))
                    return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
                candidate.scene.bodies.push_back(std::move(copied));
            }
            return Result<void>::Success();
        }

        /** @brief Query fixture bodies also occupy the actual solver and must never be
         * omitted from complete restore topology. */
        Result<void> QueryBodies(const CanonicalWorld &source, CanonicalWorld &candidate) {
            static_assert(std::is_nothrow_move_constructible_v<CanonicalQueryFixtureRecord>);
            for (const auto &original : source.query.fixtures) {
                auto copied = original;
                if (!CloneBody(source, candidate, original.nativeBody))
                    return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
                candidate.query.fixtures.push_back(std::move(copied));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CloneCanonicalStateBodies */
    Result<void> CloneCanonicalStateBodies(const CanonicalWorld &source, CanonicalWorld &candidate) {
        if (source.scene.bodies.size() > candidate.scene.bodies.capacity() ||
            source.query.fixtures.size() > candidate.query.fixtures.capacity())
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (auto scene = SceneBodies(source, candidate); scene.HasError())
            return scene;
        return QueryBodies(source, candidate);
    }
}  // namespace Horo::Physics::Detail
