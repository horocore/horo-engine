#include "CanonicalPhysicsStateCloneInternal.h"
#include "CanonicalPhysicsStateConstraintReplay.h"

#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <type_traits>

namespace Horo::Physics::Detail {
    namespace {
        using LookupRow = std::pair<const JPH::Constraint *, const CanonicalSceneConstraintRecord *>;

        /** @brief Pointer ordering is a private lookup only; actual native manager
         * order remains the creation order. */
        std::vector<LookupRow> Lookup(const CanonicalWorld &source) {
            std::vector<LookupRow> result;
            result.reserve(source.scene.constraints.size());
            for (const auto &record : source.scene.constraints)
                result.emplace_back(record.constraint.GetPtr(), &record);
            std::ranges::sort(result, std::less<const JPH::Constraint *>{}, &LookupRow::first);
            return result;
        }

        /** @brief Resolves one exact native constraint to its original Horo ownership
         * metadata. */
        const CanonicalSceneConstraintRecord *Find(std::span<const LookupRow> lookup, const JPH::Constraint *native) noexcept {
            const auto found = std::ranges::lower_bound(lookup, native, std::less<const JPH::Constraint *>{}, &LookupRow::first);
            return found != lookup.end() && found->first == native ? found->second : nullptr;
        }

        /** @brief The fixed-world endpoint is Jolt's existing singleton, never a
         * fabricated resident body. */
        JPH::Body *Endpoint(CanonicalWorld &candidate, JPH::BodyID body) noexcept {
            return body.IsInvalid() ? &JPH::Body::sFixedToWorld : candidate.native.system->GetBodyLockInterfaceNoLock().TryGetBody(body);
        }

        /** @brief Creates only the same supported two-body family against the
         * candidate's actual resident endpoints. */
        Result<CanonicalSceneConstraintRecord> Clone(const CanonicalSceneConstraintRecord &original, CanonicalWorld &candidate) {
            using Outcome = Result<CanonicalSceneConstraintRecord>;
            if (original.constraint->GetType() != JPH::EConstraintType::TwoBodyConstraint)
                return Outcome::Failure(MakeError(PhysicsErrors::OperationUnsupported));
            auto copied = original;
            auto *first = Endpoint(candidate, original.firstBody);
            auto *second = Endpoint(candidate, original.secondBody);
            if (!first || !second)
                return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            auto native = ReplayCanonicalConstraintCreation(original, *first, *second);
            if (native.HasError())
                return Outcome::Failure(native.ErrorValue());
            copied.constraint = std::move(native).Value();
            return Outcome::Success(std::move(copied));
        }
    }  // namespace

    /** @copydoc CloneCanonicalStateJoints */
    Result<void> CloneCanonicalStateJoints(const CanonicalWorld &source, CanonicalWorld &candidate) {
        static_assert(std::is_nothrow_move_constructible_v<CanonicalSceneConstraintRecord>);
        const auto nativeOrder = source.native.system->GetConstraints();
        if (nativeOrder.size() != source.scene.constraints.size() || nativeOrder.size() > candidate.scene.constraints.capacity())
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        const auto lookup = Lookup(source);
        for (const auto &native : nativeOrder) {
            const auto *original = Find(lookup, native.GetPtr());
            if (!original)
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            auto copied = Clone(*original, candidate);
            if (copied.HasError())
                return Result<void>::Failure(copied.ErrorValue());
            candidate.native.system->AddConstraint(copied.Value().constraint.GetPtr());
            candidate.scene.constraints.push_back(std::move(copied).Value());
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
