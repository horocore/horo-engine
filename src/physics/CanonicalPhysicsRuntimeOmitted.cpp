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

    /** @copydoc CreateCanonicalSceneShape */
    Result<ShapeHandle> CreateCanonicalSceneShape(const CanonicalWorldHandle, const PhysicsWorldId, const PhysicsShapeDescriptor &) {
        return Result<ShapeHandle>::Failure(
            MakeError(PhysicsErrors::CapabilityUnavailable, "Canonical Physics was omitted from this product composition."));
    }

    /** @copydoc CreateCanonicalSceneCompoundShape */
    Result<ShapeHandle> CreateCanonicalSceneCompoundShape(const CanonicalWorldHandle, const PhysicsWorldId,
                                                          const std::span<const PhysicsSceneShapeInstance>) {
        return Result<ShapeHandle>::Failure(
            MakeError(PhysicsErrors::CapabilityUnavailable, "Canonical Physics was omitted from this product composition."));
    }

    /** @copydoc CreateCanonicalSceneBody */
    Result<BodyHandle> CreateCanonicalSceneBody(const CanonicalWorldHandle, const PhysicsWorldId, const PhysicsSceneBodyDescriptor &) {
        return Result<BodyHandle>::Failure(
            MakeError(PhysicsErrors::CapabilityUnavailable, "Canonical Physics was omitted from this product composition."));
    }

    /** @copydoc ResolveCanonicalBodyMutation */
    Result<PhysicsBodyDescriptor> ResolveCanonicalBodyMutation(const CanonicalWorldHandle, const PhysicsWorldId,
                                                               const PhysicsBodyMutation &) {
        return Result<PhysicsBodyDescriptor>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
    }

    /** @copydoc ApplyCanonicalBodyMutation */
    Result<void> ApplyCanonicalBodyMutation(const CanonicalWorldHandle, const PhysicsWorldId, const PhysicsBodyMutation &) {
        return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
    }

    /** @copydoc ReadCanonicalSceneBodyPolicy */
    Result<PhysicsBodyDescriptor> ReadCanonicalSceneBodyPolicy(const CanonicalWorldHandle, const PhysicsWorldId, const BodyHandle) {
        return Result<PhysicsBodyDescriptor>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
    }

    /** @copydoc ReadCanonicalSceneBodyReconciliation */
    Result<PhysicsBodyReconciliation> ReadCanonicalSceneBodyReconciliation(const CanonicalWorldHandle, const PhysicsWorldId,
                                                                           const BodyHandle) {
        return Result<PhysicsBodyReconciliation>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
    }

    /** @copydoc FindCanonicalNonFiniteBody */
    std::optional<CanonicalNonFiniteBody> FindCanonicalNonFiniteBody(const CanonicalWorldHandle) noexcept {
        return std::nullopt;
    }

    /** @copydoc CanonicalQueryFixtureUsesBodyHandle */
    bool CanonicalQueryFixtureUsesBodyHandle(const CanonicalWorldHandle, const BodyHandle) noexcept {
        return false;
    }

    /** @copydoc QuarantineCanonicalSceneBody */
    void QuarantineCanonicalSceneBody(const CanonicalWorldHandle, const BodyHandle, const CanonicalRetirementSink) noexcept {}

    /** @copydoc InjectCanonicalNonFiniteBodyForTesting */
    bool InjectCanonicalNonFiniteBodyForTesting(const CanonicalWorldHandle, const BodyHandle, const float) noexcept {
        return false;
    }

    /** @copydoc CreateCanonicalSceneConstraint */
    Result<ConstraintHandle> CreateCanonicalSceneConstraint(const CanonicalWorldHandle, const PhysicsWorldId,
                                                            const PhysicsConstraintDescriptor &) {
        return Result<ConstraintHandle>::Failure(
            MakeError(PhysicsErrors::CapabilityUnavailable, "Canonical Physics was omitted from this product composition."));
    }

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
    Result<CanonicalStepOutcome> StepCanonicalWorld(const CanonicalWorldHandle, const float, const std::uint64_t,
                                                    const CanonicalContactSink) {
        return Result<CanonicalStepOutcome>::Failure(
            MakeError(PhysicsErrors::CapabilityUnavailable, "Canonical Physics was omitted from this product composition."));
    }

    /** @copydoc InvokeCanonicalContactCallbackForTesting */
    bool InvokeCanonicalContactCallbackForTesting(const CanonicalWorldHandle, const PhysicsQueryFixture &, const PhysicsQueryFixture &,
                                                  const std::uint64_t, const bool, const bool, const CanonicalContactSink) {
        return false;
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
