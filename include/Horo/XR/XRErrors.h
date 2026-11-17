#pragma once

/**
 * @file XRErrors.h
 * @brief Stable backend-neutral XR failure identities.
 */

#include "Horo/Foundation/ErrorCode.h"

#include <span>

namespace Horo::XR::XRErrors {
    /** @brief An XR contract version uses an invalid representation. */
    extern const ErrorCodeDescriptor ContractVersionInvalid;
    /** @brief A provided XR contract version cannot satisfy the required version. */
    extern const ErrorCodeDescriptor ContractVersionIncompatible;
    /** @brief An XR identity uses an invalid or malformed representation. */
    extern const ErrorCodeDescriptor IdentityInvalid;
    /** @brief An XR identity belongs to a replaced owner generation. */
    extern const ErrorCodeDescriptor IdentityStale;
    /** @brief An immutable XR capability descriptor is malformed. */
    extern const ErrorCodeDescriptor CapabilityDescriptorInvalid;
    /** @brief XR capability evidence changed after the caller captured its revision. */
    extern const ErrorCodeDescriptor CapabilityStale;
    /** @brief The selected XR system permanently does not support the requested operation. */
    extern const ErrorCodeDescriptor OperationUnsupported;
    /** @brief A known XR operation is not currently available. */
    extern const ErrorCodeDescriptor OperationUnavailable;
    /** @brief An XR operation conflicts with the active contract or system configuration. */
    extern const ErrorCodeDescriptor OperationIncompatible;
    /** @brief An XR request exceeds an immutable system capacity limit. */
    extern const ErrorCodeDescriptor CapacityExceeded;
    /** @brief An XR operation request is malformed. */
    extern const ErrorCodeDescriptor OperationInvalid;
    /** @brief The platform or XR runtime denied a required permission. */
    extern const ErrorCodeDescriptor PermissionDenied;
    /** @brief An XR coordinate-space relation or convention is incompatible. */
    extern const ErrorCodeDescriptor CoordinateSpaceIncompatible;
    /** @brief XR clock evidence mixes or omits required time domains. */
    extern const ErrorCodeDescriptor TimeDomainIncompatible;
    /** @brief XR pose component validity, value, confidence, or loss evidence is malformed. */
    extern const ErrorCodeDescriptor PoseInvalid;
    /** @brief An XR space or pose references a retired world-origin revision. */
    extern const ErrorCodeDescriptor OriginRevisionStale;
    /** @brief An XR tracking snapshot contains malformed, contradictory, or non-canonical evidence. */
    extern const ErrorCodeDescriptor TrackingSnapshotInvalid;
    /** @brief An XR tracking snapshot revision no longer matches the consumer's retained revision. */
    extern const ErrorCodeDescriptor TrackingSnapshotStale;
    /** @brief An XR view/configuration/render-target plan is malformed or non-canonical. */
    extern const ErrorCodeDescriptor ViewPlanInvalid;
    /** @brief An XR view-configuration publication was replaced after capture. */
    extern const ErrorCodeDescriptor ViewConfigurationStale;
    /** @brief An external XR render target has malformed identity, format, usage, extent, or ordering. */
    extern const ErrorCodeDescriptor ExternalTargetInvalid;
    /** @brief XR loader preflight policy or evidence is malformed. */
    extern const ErrorCodeDescriptor LoaderPreflightInvalid;
    /** @brief The exact product-selected loader source is absent. */
    extern const ErrorCodeDescriptor LoaderAbsent;
    /** @brief The discovered loader cannot satisfy the admitted API interval. */
    extern const ErrorCodeDescriptor LoaderIncompatible;
    /** @brief The verified loader artifact could not be opened. */
    extern const ErrorCodeDescriptor LoaderOpenFailed;
    /** @brief The selected loader could not discover an active runtime. */
    extern const ErrorCodeDescriptor RuntimeUnavailable;
    /** @brief Runtime discovery was rejected by explicit product or runtime policy. */
    extern const ErrorCodeDescriptor RuntimeRejected;
    /** @brief The active runtime has no system satisfying the requested product profile. */
    extern const ErrorCodeDescriptor SystemUnsupported;
    /** @brief An otherwise admissible XR system is temporarily unavailable. */
    extern const ErrorCodeDescriptor SystemTemporarilyUnavailable;
    /** @brief A runtime-selection override is not authorized for this product execution. */
    extern const ErrorCodeDescriptor RuntimeOverrideRejected;
    /** @brief Loader preflight was cancelled before activation publication. */
    extern const ErrorCodeDescriptor LoaderPreflightCancelled;
    /** @brief Loader preflight evidence belongs to a replaced attempt or composition input. */
    extern const ErrorCodeDescriptor LoaderPreflightStale;

    /**
     * @brief Returns every stable XRApi descriptor for module-registry contribution.
     * @return Bounded immutable descriptor references owned for process lifetime by XRApi.
     */
    [[nodiscard]] std::span<const ErrorCodeDescriptor *const> Descriptors() noexcept;
}  // namespace Horo::XR::XRErrors
