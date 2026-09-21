#include "Horo/Physics/PhysicsWorldSettings.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <array>
#include <bit>
#include <cmath>

namespace Horo::Physics {
    namespace {
        /** @brief Normalizes signed zero before encoding an already validated finite float. */
        std::uint64_t FloatWord(const float value) noexcept {
            return std::bit_cast<std::uint32_t>(value == 0 ? 0.0F : value);
        }

        /** @brief Lists every schema-2 field in stable order, independent of C++ structure layout. */
        auto SettingsWords(const PhysicsWorldSettingsDescriptor &v) noexcept {
            return std::array<std::uint64_t, 34>{PhysicsWorldSettingsSchemaVersion,
                                                 v.world.contractVersion,
                                                 static_cast<std::uint64_t>(v.world.profile),
                                                 FloatWord(v.world.gravity.x),
                                                 FloatWord(v.world.gravity.y),
                                                 FloatWord(v.world.gravity.z),
                                                 std::bit_cast<std::uint64_t>(v.world.fixedDeltaSeconds),
                                                 v.world.capacity.maximumBodies,
                                                 v.world.capacity.maximumColliderSlots,
                                                 v.world.capacity.maximumConstraints,
                                                 v.world.capacity.maximumPlanBytes,
                                                 v.step.substepsPerTick,
                                                 v.step.velocityIterations,
                                                 v.step.positionIterations,
                                                 FloatWord(v.step.sleepPointSpeedMetersPerSecond),
                                                 FloatWord(v.step.sleepDelaySeconds),
                                                 v.step.sleepingEnabled,
                                                 static_cast<std::uint64_t>(v.step.defaultMotionQuality),
                                                 v.budgets.maximumShapes,
                                                 v.budgets.maximumContactPairs,
                                                 v.budgets.maximumContactConstraints,
                                                 v.budgets.maximumInFlightPairs,
                                                 v.budgets.maximumCommands,
                                                 v.budgets.maximumEvents,
                                                 v.budgets.maximumQueries,
                                                 v.budgets.maximumCommandsPerTick,
                                                 v.budgets.maximumQueriesPerTick,
                                                 v.budgets.scratchBytes,
                                                 v.budgets.residentShapeBytes,
                                                 static_cast<std::uint64_t>(v.budgets.scratchExhaustion),
                                                 static_cast<std::uint64_t>(v.budgets.eventOverflow),
                                                 FloatWord(v.bounds.localHalfExtentMeters),
                                                 FloatWord(v.bounds.dynamicContactRadiusMeters),
                                                 static_cast<std::uint64_t>(v.nonFinitePolicy)};
        }

        /** @brief Hashes a fixed-size little-endian preimage without allocation, native bytes or padding. */
        PhysicsWorldSettingsIdentity SettingsIdentity(const PhysicsWorldSettingsDescriptor &values) noexcept {
            const auto words = SettingsWords(values);
            std::array<std::byte, sizeof(std::uint64_t) * words.size()> bytes{};
            std::size_t cursor = 0;
            for (const auto word : words) {
                for (unsigned int shift = 0; shift < 64; shift += 8)
                    bytes[cursor++] = static_cast<std::byte>((word >> shift) & 0xffU);
            }
            return {ComputeSha256(bytes)};
        }

        /** @brief Checks local hard/contact envelopes without interpreting any global coordinate or origin generation. */
        Result<void> ValidateBounds(const PhysicsWorldBounds &bounds) {
            if (!std::isfinite(bounds.localHalfExtentMeters) || !std::isfinite(bounds.dynamicContactRadiusMeters) ||
                bounds.localHalfExtentMeters <= 0 || bounds.dynamicContactRadiusMeters <= 0)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Physics world bounds must be finite and positive."));
            if (bounds.localHalfExtentMeters > MaximumPhysicsLocalHalfExtentMeters ||
                bounds.dynamicContactRadiusMeters > MaximumPhysicsDynamicContactRadiusMeters ||
                bounds.dynamicContactRadiusMeters > bounds.localHalfExtentMeters)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ProfileUnsupported, "Physics bounds exceed the hard/contact envelope or are inconsistent."));
            return Result<void>::Success();
        }

        /** @brief Composes component validation and complete-snapshot-only scheduling/bounds policy. */
        Result<void> ValidateSettings(const PhysicsWorldSettingsDescriptor &descriptor) {
            if (const auto world = ValidatePhysicsWorldDescriptor(descriptor.world); world.HasError())
                return world;
            if (const auto step = ValidatePhysicsStepPolicy(descriptor.step); step.HasError())
                return step;
            if (const auto budgets = ValidatePhysicsWorldBudgets(descriptor.budgets); budgets.HasError())
                return budgets;
            if (descriptor.world.fixedDeltaSeconds != 1.0 / 60.0)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::ProfileUnsupported, "CanonicalV1 world settings require the 60 Hz fixed schedule."));
            if (descriptor.nonFinitePolicy != PhysicsNonFinitePolicy::FailWorld &&
                descriptor.nonFinitePolicy != PhysicsNonFinitePolicy::QuarantineBody)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::OperationUnsupported, "Unknown non-finite runtime containment policy."));
            return ValidateBounds(descriptor.bounds);
        }
    }  // namespace

    /** @copydoc PhysicsWorldSettings::Capture */
    Result<PhysicsWorldSettings> PhysicsWorldSettings::Capture(const PhysicsWorldSettingsDescriptor &descriptor) {
        if (const auto valid = ValidateSettings(descriptor); valid.HasError())
            return Result<PhysicsWorldSettings>::Failure(valid.ErrorValue());
        return Result<PhysicsWorldSettings>::Success(PhysicsWorldSettings{descriptor, SettingsIdentity(descriptor)});
    }

    /** @copydoc PhysicsWorldSettings::PhysicsWorldSettings */
    PhysicsWorldSettings::PhysicsWorldSettings(const PhysicsWorldSettingsDescriptor &values, const PhysicsWorldSettingsIdentity &identity)
        : values_(values), identity_(identity) {}

    /** @copydoc PhysicsWorldSettings::Values */
    const PhysicsWorldSettingsDescriptor &PhysicsWorldSettings::Values() const noexcept {
        return values_;
    }

    /** @copydoc PhysicsWorldSettings::Identity */
    const PhysicsWorldSettingsIdentity &PhysicsWorldSettings::Identity() const noexcept {
        return identity_;
    }
}  // namespace Horo::Physics
