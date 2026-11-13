#include "Horo/Physics/PhysicsCapabilities.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <algorithm>

namespace Horo::Physics {
    namespace {
        /** @brief Validate each feature against the composition fact without strengthening unknown evidence. */
        bool CoherentSupport(const PhysicsCapabilitySupport support, const PhysicsAvailability availability) noexcept {
            if (support > PhysicsCapabilitySupport::Available)
                return false;
            if (availability == PhysicsAvailability::Omitted)
                return support == PhysicsCapabilitySupport::Unsupported;
            return availability == PhysicsAvailability::Available || support != PhysicsCapabilitySupport::Available;
        }
    }  // namespace

    /** @copydoc ValidatePhysicsCapabilities */
    bool ValidatePhysicsCapabilities(const PhysicsCapabilities &capabilities) noexcept {
        if (capabilities.contractVersion != PhysicsCapabilityContractVersion || capabilities.revision == 0 ||
            capabilities.availability > PhysicsAvailability::Available)
            return false;
        return std::ranges::all_of(capabilities.features, [&capabilities](const auto support) {
            return CoherentSupport(support, capabilities.availability);
        });
    }

    /** @copydoc RequirePhysicsCapability */
    Result<void> RequirePhysicsCapability(const PhysicsCapabilities &capabilities, const PhysicsCapability capability,
                                          const std::uint64_t expectedRevision) {
        if (!ValidatePhysicsCapabilities(capabilities) || capability >= PhysicsCapability::Count || expectedRevision == 0)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Invalid physics capability snapshot, feature or expected revision."));
        if (capabilities.revision != expectedRevision)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        const auto support = capabilities.features[static_cast<std::size_t>(capability)];
        if (support == PhysicsCapabilitySupport::Available)
            return Result<void>::Success();
        if (support == PhysicsCapabilitySupport::Unsupported)
            return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
        return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
    }

    /** @copydoc AdmitPhysicsWorldDescriptor */
    Result<void> AdmitPhysicsWorldDescriptor(const PhysicsWorldDescriptor &descriptor, const PhysicsCapabilities &capabilities,
                                             const std::uint64_t expectedRevision) {
        if (const auto policy = ValidatePhysicsWorldDescriptor(descriptor); policy.HasError())
            return policy;
        return RequirePhysicsCapability(capabilities, PhysicsCapability::WorldCreation, expectedRevision);
    }
}  // namespace Horo::Physics
