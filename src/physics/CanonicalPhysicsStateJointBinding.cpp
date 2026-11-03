#include "CanonicalPhysicsStateBindingInternal.h"

namespace Horo::Physics::Detail {
    namespace {
        struct Resident final {
            ConstraintHandle handle;
            const JPH::Constraint *native{};
        };

        /** @brief Only the four genuinely implemented Horo native joint families may
         * enter solver schema one. */
        Result<PhysicsStateConstraintKind> Kind(const JPH::Constraint &constraint) {
            using Outcome = Result<PhysicsStateConstraintKind>;
            switch (constraint.GetSubType()) {
                case JPH::EConstraintSubType::Fixed:
                    return Outcome::Success(PhysicsStateConstraintKind::Fixed);
                case JPH::EConstraintSubType::Distance:
                    return Outcome::Success(PhysicsStateConstraintKind::Distance);
                case JPH::EConstraintSubType::Hinge:
                    return Outcome::Success(PhysicsStateConstraintKind::Hinge);
                case JPH::EConstraintSubType::Slider:
                    return Outcome::Success(PhysicsStateConstraintKind::Slider);
                default:
                    return Outcome::Failure(MakeError(PhysicsErrors::OperationUnsupported));
            }
        }

        /** @brief Stable native pointer sorting is only a cold lookup, not serialized
         * ordering or identity. */
        std::vector<Resident> Inventory(const CanonicalWorld &world) {
            std::vector<Resident> result;
            result.reserve(world.scene.constraints.size());
            for (const auto &joint : world.scene.constraints)
                result.push_back({joint.handle, joint.constraint.GetPtr()});
            std::ranges::sort(result, {}, &Resident::handle);
            return result;
        }

        /** @brief Creates exact current manager indexes from the actual public native
         * manager snapshot. */
        Result<void> BindNative(const CanonicalWorld &world, CanonicalPhysicsStateBindingStorage &storage) {
            const auto native = world.native.system->GetConstraints();
            if (native.size() != storage.stableJoints.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            const auto inventory = Inventory(world);
            storage.nativeJoints.resize(native.size());
            std::vector<std::pair<const JPH::Constraint *, std::uint32_t>> indexes;
            indexes.reserve(native.size());
            for (std::uint32_t index = 0; index < native.size(); ++index)
                indexes.emplace_back(native[index].GetPtr(), index);
            std::ranges::sort(indexes, std::less<const JPH::Constraint *>{}, &decltype(indexes)::value_type::first);
            for (auto &binding : storage.stableJoints) {
                const auto record = std::ranges::lower_bound(inventory, binding.constraint, {}, &Resident::handle);
                if (record == inventory.end() || record->handle != binding.constraint)
                    return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
                const auto found = std::ranges::lower_bound(indexes, record->native, std::less<const JPH::Constraint *>{},
                                                            &decltype(indexes)::value_type::first);
                if (found == indexes.end() || found->first != record->native)
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                auto kind = Kind(*record->native);
                if (kind.HasError())
                    return Result<void>::Failure(kind.ErrorValue());
                binding.nativeIndex = found->second;
                binding.kind = kind.Value();
                if (storage.nativeJoints[found->second].id != 0)
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                storage.nativeJoints[found->second] = binding;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc BindCanonicalStateJoints */
    Result<void> BindCanonicalStateJoints(const CanonicalWorld &world, std::span<const PhysicsStateConstraintBinding> bindings,
                                          CanonicalPhysicsStateBindingStorage &storage) {
        if (bindings.size() != world.scene.constraints.size())
            return Result<void>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Physics checkpoint requires every resident joint binding."));
        storage.stableJoints.reserve(bindings.size());
        for (const auto &binding : bindings) {
            if (!binding.id || !binding.constraint.IsValid())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            storage.stableJoints.push_back({.id = binding.id, .constraint = binding.constraint});
        }
        std::ranges::sort(storage.stableJoints, {}, &CanonicalPhysicsStateJointBinding::id);
        for (std::size_t index = 1; index < bindings.size(); ++index)
            if (storage.stableJoints[index - 1].id == storage.stableJoints[index].id)
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        return BindNative(world, storage);
    }
}  // namespace Horo::Physics::Detail
