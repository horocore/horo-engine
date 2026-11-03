#include "PhysicsStateValidationInternal.h"

namespace Horo::Physics {
    namespace {
        using namespace StateDetail;

        /** @brief Missing digest components never silently match an unconfigured
         * runtime. */
        bool Nonzero(const Sha256Digest &digest) noexcept {
            return std::ranges::any_of(digest.bytes, [](auto value) {
                return value != 0;
            });
        }

        /** @brief Checked independent record ceiling and aggregate owned payload
         * accounting. */
        template <class T> bool Charge(std::span<const T> values, std::size_t maximum, std::size_t &bytes) noexcept {
            if (values.size() > maximum || values.size() > bytes / sizeof(T))
                return false;
            bytes -= values.size_bytes();
            return true;
        }

        /** @brief Rejects unreasonable caller ceilings and the complete payload before
         * scratch or native allocation. */
        bool WithinLimits(const PhysicsStateSolverView &state, const PhysicsStateLimits &limits) noexcept {
            if (!ValidStateLimits(limits))
                return false;
            std::size_t bytes = limits.maximumBytes;
            return Charge(state.bodies, limits.maximumBodies, bytes) && Charge(state.activeBodyOrder, state.bodies.size(), bytes) &&
                   Charge(state.constraints, limits.maximumConstraints, bytes) && Charge(state.pairs, limits.maximumPairs, bytes) &&
                   Charge(state.manifolds, limits.maximumManifolds, bytes) && Charge(state.ccdManifolds, limits.maximumCcdManifolds, bytes);
        }

        /** @brief Caller scratch is private writable storage, never a view into
         * candidate bytes. */
        bool ValidScratch(const PhysicsStateSolverView &state, std::span<std::uint8_t> scratch) noexcept {
            return scratch.size() >= state.bodies.size() && !Overlaps(scratch, state.bodies) && !Overlaps(scratch, state.activeBodyOrder) &&
                   !Overlaps(scratch, state.constraints) && !Overlaps(scratch, state.pairs) && !Overlaps(scratch, state.manifolds) &&
                   !Overlaps(scratch, state.ccdManifolds);
        }

        /** @brief Complete permutation admission is O(n log n), bounded and
         * allocation-free. */
        Result<void> ActiveOrder(const PhysicsStateSolverView &state, const std::span<std::uint8_t> scratch) {
            if (!ValidScratch(state, scratch))
                return Invalid("Physics snapshot validation requires separate body-capacity scratch.");
            std::ranges::fill(scratch.first(state.bodies.size()), std::uint8_t{});
            for (const auto id : state.activeBodyOrder) {
                const auto *body = FindBody(state.bodies, id);
                if (!body || !body->active)
                    return Invalid("Physics snapshot active-body order refers to a missing "
                                   "or sleeping body.");
                auto &seen = scratch[static_cast<std::size_t>(body - state.bodies.data())];
                if (seen)
                    return Invalid("Physics snapshot active-body order contains a duplicate.");
                seen = 1;
            }
            const auto expected = std::ranges::count_if(state.bodies, [](const auto &body) {
                return body.active;
            });
            return static_cast<std::size_t>(expected) == state.activeBodyOrder.size()
                       ? Result<void>::Success()
                       : Invalid("Physics snapshot active-body order is incomplete.");
        }
    }  // namespace

    /** @copydoc ValidatePhysicsStateCompatibility */
    Result<void> ValidatePhysicsStateCompatibility(const PhysicsStateCompatibility &compatibility) {
        if (compatibility.formatVersion != PhysicsStateSnapshotVersion ||
            compatibility.solverSchemaVersion != PhysicsStateSolverSchemaVersion ||
            compatibility.settingsSchemaVersion != PhysicsWorldSettingsSchemaVersion)
            return Invalid("Physics snapshot format, solver or settings schema is unsupported.");
        if (compatibility.floatingPoint.architecture <= PhysicsStateFpArchitecture::Unspecified ||
            compatibility.floatingPoint.architecture > PhysicsStateFpArchitecture::ArmFpcr)
            return Invalid("Physics snapshot floating-point control architecture is unsupported.");
        const std::array digests{&compatibility.build,   &compatibility.platform, &compatibility.solver,  &compatibility.settings.digest,
                                 &compatibility.content, &compatibility.topology, &compatibility.filters, &compatibility.surfaces};
        if (!std::ranges::all_of(digests, [](const auto *digest) {
            return Nonzero(*digest);
        }))
            return Invalid("Physics snapshot requires complete build, platform, "
                           "settings and content compatibility.");
        return Result<void>::Success();
    }

    /** @copydoc ValidatePhysicsStateSolver */
    Result<void> ValidatePhysicsStateSolver(const PhysicsStateSolverView &state, const PhysicsStateLimits &limits,
                                            const std::span<std::uint8_t> activeBodyScratch) {
        if (!WithinLimits(state, limits))
            return Result<void>::Failure(
                MakeError(PhysicsErrors::CapacityExceeded, "Physics snapshot exceeds its admitted storage ceilings."));
        if (!std::isfinite(state.previousDelta) || state.previousDelta < 0 || !Finite(state.gravity))
            return Invalid("Physics snapshot global solver state is invalid.");
        if (auto bodies = ValidateBodies(state.bodies); bodies.HasError())
            return bodies;
        if (auto active = ActiveOrder(state, activeBodyScratch); active.HasError())
            return active;
        if (auto constraints = ValidateConstraints(state.constraints); constraints.HasError())
            return constraints;
        return ValidateContacts(state);
    }
}  // namespace Horo::Physics
