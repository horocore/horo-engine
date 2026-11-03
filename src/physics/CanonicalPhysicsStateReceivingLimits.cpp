#include "CanonicalPhysicsRuntimeInternal.h"
#include "CanonicalPhysicsStateBounds.h"
#include "CanonicalPhysicsStateSceneInventory.h"

#include <limits>

namespace Horo::Physics::Detail {
    namespace {
        std::uint32_t WireRows(std::size_t bytes, std::size_t stride) noexcept {
            return static_cast<std::uint32_t>(std::min(bytes / stride, std::size_t{std::numeric_limits<std::uint32_t>::max()}));
        }
    }  // namespace

    /** @copydoc ReadCanonicalStateReceivingLimits */
    Result<StateDetail::RestartInputLimits> ReadCanonicalStateReceivingLimits(CanonicalWorldHandle handle,
                                                                              const PhysicsWorldSettings &settings, std::size_t wireBytes,
                                                                              std::size_t retainedBytes) {
        using Outcome = Result<StateDetail::RestartInputLimits>;
        auto counts = InspectCanonicalStateSceneInventory(handle);
        if (counts.HasError())
            return Outcome::Failure(counts.ErrorValue());
        const auto &world = *static_cast<const CanonicalWorld *>(handle.value);
        if (world.settingsIdentity != settings.Identity() || !world.owner.constructionRecipes)
            return Outcome::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        auto bounds = PlanCanonicalPhysicsStateBounds(settings, retainedBytes);
        if (bounds.HasError())
            return Outcome::Failure(bounds.ErrorValue());
        const auto &values = settings.Values();
        StateDetail::RestartInputLimits result;
        result.solver = {values.world.capacity.maximumBodies,
                         values.world.capacity.maximumConstraints,
                         static_cast<std::uint32_t>(bounds.Value().pairEntries),
                         static_cast<std::uint32_t>(bounds.Value().manifoldEntries),
                         static_cast<std::uint32_t>(bounds.Value().manifoldEntries),
                         retainedBytes};
        result.construction = {.shapes = world.owner.constructionRecipes->Limits(),
                               .maximumBodies = values.world.capacity.maximumBodies,
                               .maximumColliders = world.scene.maximumShapes,
                               .maximumMaterials = WireRows(wireBytes, StateDetail::ConstructionRowBytes[2]),
                               .maximumJoints = values.world.capacity.maximumConstraints,
                               .maximumFixtures = world.query.maximumFixtures,
                               .maximumQueryChildren = WireRows(wireBytes, StateDetail::ConstructionRowBytes[5]),
                               .maximumRetainedBytes = retainedBytes,
                               .maximumEncodedBytes = wireBytes,
                               .maximumOwners = WireRows(wireBytes, StateDetail::ConstructionRowBytes[6])};
        result.maximumEventPairs = values.budgets.maximumInFlightPairs;
        result.maximumEvents = values.budgets.maximumEvents;
        result.maximumWireBytes = wireBytes;
        result.maximumRetainedBytes = retainedBytes;
        return Outcome::Success(result);
    }
}  // namespace Horo::Physics::Detail
