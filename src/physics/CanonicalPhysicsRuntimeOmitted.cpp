#include "CanonicalPhysicsRuntime.h"
#include "Horo/Physics/PhysicsErrors.h"

namespace Horo::Physics::Detail {
    /** @copydoc CreateCanonicalRuntime */
    Result<CanonicalRuntimeHandle> CreateCanonicalRuntime(const CanonicalFailurePoint) {
        return Result<CanonicalRuntimeHandle>::Failure(
            MakeError(PhysicsErrors::CapabilityUnavailable, "Canonical Physics was omitted from this product composition."));
    }

    /** @copydoc DestroyCanonicalRuntime */
    void DestroyCanonicalRuntime(const CanonicalRuntimeHandle) noexcept {}

    /** @copydoc CreateCanonicalWorld */
    Result<CanonicalWorldHandle> CreateCanonicalWorld(const CanonicalRuntimeHandle, const PhysicsWorldSettings &,
                                                      const CanonicalFailurePoint) {
        return Result<CanonicalWorldHandle>::Failure(
            MakeError(PhysicsErrors::CapabilityUnavailable, "Canonical Physics was omitted from this product composition."));
    }

    /** @copydoc DestroyCanonicalWorld */
    void DestroyCanonicalWorld(const CanonicalWorldHandle) noexcept {}

    /** @copydoc CreateCanonicalQueryFixture */
    Result<PhysicsQueryFixture> CreateCanonicalQueryFixture(const CanonicalWorldHandle, const PhysicsWorldId,
                                                            const PhysicsQueryFixtureDescriptor &) {
        return Result<PhysicsQueryFixture>::Failure(
            MakeError(PhysicsErrors::CapabilityUnavailable, "Canonical Physics was omitted from this product composition."));
    }

    /** @copydoc DestroyCanonicalQueryFixture */
    Result<void> DestroyCanonicalQueryFixture(const CanonicalWorldHandle, const PhysicsQueryFixture &) {
        return Result<void>::Failure(
            MakeError(PhysicsErrors::CapabilityUnavailable, "Canonical Physics was omitted from this product composition."));
    }

    /** @copydoc ExecuteCanonicalQuery */
    Result<PhysicsQueryResult> ExecuteCanonicalQuery(const CanonicalWorldHandle, const PhysicsQueryDescriptor &,
                                                     const std::span<PhysicsQueryHit>) {
        return Result<PhysicsQueryResult>::Failure(
            MakeError(PhysicsErrors::CapabilityUnavailable, "Canonical Physics was omitted from this product composition."));
    }

    /** @copydoc StepCanonicalWorld */
    Result<CanonicalStepOutcome> StepCanonicalWorld(const CanonicalWorldHandle, const float) {
        return Result<CanonicalStepOutcome>::Failure(
            MakeError(PhysicsErrors::CapabilityUnavailable, "Canonical Physics was omitted from this product composition."));
    }

    /** @copydoc SubmitCanonicalDiagnosticForTesting */
    void SubmitCanonicalDiagnosticForTesting(const CanonicalWorldHandle, const CanonicalDiagnosticKind, const std::string_view) noexcept {}

    /** @copydoc InvokeCanonicalDiagnosticCallbackForTesting */
    void InvokeCanonicalDiagnosticCallbackForTesting(const CanonicalWorldHandle, const CanonicalDiagnosticKind, const std::string_view) {}

    /** @copydoc InspectCanonicalResources */
    CanonicalResourceCounts InspectCanonicalResources(const CanonicalRuntimeHandle) noexcept {
        return {};
    }
}  // namespace Horo::Physics::Detail
