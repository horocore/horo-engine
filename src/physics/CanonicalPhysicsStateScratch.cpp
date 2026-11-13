#include "CanonicalPhysicsRuntimeInternal.h"
#include "CanonicalPhysicsStateCapture.h"

#include <limits>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Counts come from the actual joined native owner; source history holes
         * cannot increase these typed prefixes. */
        bool BodiesFit(const CanonicalWorld &world, const StateDetail::SolverStorage &staging,
                       const CanonicalPhysicsStateCaptureScratch &scratch) noexcept {
            const auto count = world.native.system->GetNumBodies();
            return staging.bodies.size() >= count && staging.activeBodyOrder.size() >= count && staging.activeBodyScratch.size() >= count &&
                   scratch.activeBodies.size() >= count && staging.constraints.size() >= world.scene.constraints.size();
        }

        /** @brief Native cache limits bound every possible typed contact prefix,
         * independent of this tick's occupancy. */
        bool CacheFits(const CanonicalPhysicsStateBounds &bounds, const StateDetail::SolverStorage &staging,
                       const CanonicalPhysicsStateCaptureScratch &scratch) noexcept {
            return staging.pairs.size() >= bounds.pairEntries && staging.manifolds.size() >= bounds.manifoldEntries &&
                   staging.ccdManifolds.size() >= bounds.manifoldEntries && scratch.temporary.size() >= bounds.temporaryBytes &&
                   scratch.recorder.size() >= bounds.recorderBytes;
        }

        /** @brief Range checks never form an overflowing pointer end. */
        bool Overlap(std::span<const std::byte> one, std::span<const std::byte> two) noexcept {
            if (one.empty() || two.empty())
                return false;
            const auto a = reinterpret_cast<std::uintptr_t>(one.data());
            const auto b = reinterpret_cast<std::uintptr_t>(two.data());
            return a <= b ? b - a < one.size() : a - b < two.size();
        }

        /** @brief An external private caller cannot alias arena/recorder bytes with
         * active IDs or typed decode destinations. */
        bool Disjoint(const StateDetail::SolverStorage &storage, const CanonicalPhysicsStateCaptureScratch &scratch) noexcept {
            const std::array ranges{std::span<const std::byte>{scratch.temporary},
                                    std::span<const std::byte>{scratch.recorder},
                                    std::as_bytes(scratch.activeBodies),
                                    std::as_bytes(std::span{storage.bodies}),
                                    std::as_bytes(std::span{storage.activeBodyOrder}),
                                    std::as_bytes(std::span{storage.constraints}),
                                    std::as_bytes(std::span{storage.pairs}),
                                    std::as_bytes(std::span{storage.manifolds}),
                                    std::as_bytes(std::span{storage.ccdManifolds}),
                                    std::as_bytes(std::span{storage.activeBodyScratch})};
            for (std::size_t a = 0; a < ranges.size(); ++a)
                for (std::size_t b = a + 1; b < ranges.size(); ++b)
                    if (Overlap(ranges[a], ranges[b]))
                        return false;
            return true;
        }
    }  // namespace

    /** @copydoc ValidateCanonicalPhysicsStateScratch */
    Result<CanonicalPhysicsStateBounds> ValidateCanonicalPhysicsStateScratch(CanonicalWorldHandle handle,
                                                                             const PhysicsWorldSettings &settings,
                                                                             const StateDetail::SolverStorage &staging,
                                                                             const CanonicalPhysicsStateCaptureScratch &scratch) {
        using Outcome = Result<CanonicalPhysicsStateBounds>;
        const auto *world = static_cast<const CanonicalWorld *>(handle.value);
        if (!world || !world->native.system || world->settingsIdentity != settings.Identity())
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        auto bounds = PlanCanonicalPhysicsStateBounds(settings, std::numeric_limits<std::size_t>::max());
        if (bounds.HasError())
            return bounds;
        if (!BodiesFit(*world, staging, scratch) || !CacheFits(bounds.Value(), staging, scratch))
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (!Disjoint(staging, scratch))
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        return bounds;
    }
}  // namespace Horo::Physics::Detail
