#include "Horo/XR/XRErrors.h"

#include <array>

namespace Horo::XR::XRErrors {
    namespace {
        const ErrorDomainId XRDomain{"horo.xr"};
    }

    const ErrorCodeDescriptor ContractVersionInvalid{
        .domain = XRDomain,
        .code = ErrorCode{"xr.contract.version_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The Horo XR contract version uses an invalid representation.",
        .remediationHint = "Use a non-zero Horo XRApi major version; do not substitute a native OpenXR API version.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ContractVersionIncompatible{
        .domain = XRDomain,
        .code = ErrorCode{"xr.contract.version_incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The provided Horo XR contract cannot satisfy the required contract.",
        .remediationHint = "Compose an XR producer with the same major version and a sufficient minor version.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor IdentityInvalid{
        .domain = XRDomain,
        .code = ErrorCode{"xr.identity.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An XR identity or its active owner uses an invalid representation.",
        .remediationHint = "Use only non-zero owner generations and generation-safe slots issued by the active XR owner.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor IdentityStale{
        .domain = XRDomain,
        .code = ErrorCode{"xr.identity.stale"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "An XR identity belongs to a replaced runtime, system, or session.",
        .remediationHint = "Resolve a new identity from the active XR generation before repeating the operation.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor CapabilityDescriptorInvalid{
        .domain = XRDomain,
        .code = ErrorCode{"xr.capability.descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "XR capability evidence contains an invalid identity, state, revision, or limit.",
        .remediationHint = "Publish complete typed evidence with a current system, known states, and limits within XRApi hard ceilings.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor CapabilityStale{
        .domain = XRDomain,
        .code = ErrorCode{"xr.capability.stale"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "XR capability evidence changed after it was captured.",
        .remediationHint = "Capture the current immutable capability snapshot and repeat complete admission.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor OperationUnsupported{
        .domain = XRDomain,
        .code = ErrorCode{"xr.operation.unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The selected XR system permanently does not support the requested operation.",
        .remediationHint = "Select a supported capability or use the product's explicitly admitted non-XR fallback.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor OperationUnavailable{
        .domain = XRDomain,
        .code = ErrorCode{"xr.operation.unavailable"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The requested XR operation is known but not currently available.",
        .remediationHint = "Resolve the reported permission, policy, dependency, temporary-loss, or lifecycle state before retrying.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor OperationIncompatible{
        .domain = XRDomain,
        .code = ErrorCode{"xr.operation.incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested XR operation is incompatible with the selected system configuration.",
        .remediationHint = "Choose a product profile and composition whose declared XR capabilities are compatible.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PermissionDenied{
        .domain = XRDomain,
        .code = ErrorCode{"xr.permission.denied"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The XR runtime or platform denied a required permission.",
        .remediationHint = "Review your XR runtime and operating system privacy permissions before retrying.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor CapacityExceeded{
        .domain = XRDomain,
        .code = ErrorCode{"xr.operation.capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The XR request exceeds an immutable system capacity limit.",
        .remediationHint = "Reduce the bounded request or select a qualified system with sufficient declared capacity.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor OperationInvalid{
        .domain = XRDomain,
        .code = ErrorCode{"xr.operation.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The XR operation request is malformed.",
        .remediationHint = "Use a known capability, non-zero captured revision, and finite typed bounds.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor CoordinateSpaceIncompatible{
        .domain = XRDomain,
        .code = ErrorCode{"xr.coordinate_space.incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "XR coordinate spaces cannot be related under the declared Horo convention.",
        .remediationHint = "Use a contiguous current-session path in metres and right-handed Y-up negative-Z-forward space.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor TimeDomainIncompatible{
        .domain = XRDomain,
        .code = ErrorCode{"xr.time_domain.incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "XR pose timing evidence mixes or omits required clock domains.",
        .remediationHint = "Correlate runtime samples explicitly with either simulation time or render prediction time.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor PoseInvalid{
        .domain = XRDomain,
        .code = ErrorCode{"xr.pose.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "XR pose values conflict with their validity, confidence, or loss evidence.",
        .remediationHint = "Publish finite unit pose values only for independently valid components and clear lost components.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor OriginRevisionStale{
        .domain = XRDomain,
        .code = ErrorCode{"xr.origin_revision.stale"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "XR coordinate evidence references a retired world-origin revision.",
        .remediationHint = "Relocate the pose through the current world-origin adapter before use.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor TrackingSnapshotInvalid{
        .domain = XRDomain,
        .code = ErrorCode{"xr.tracking_snapshot.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "XR tracking snapshot evidence is malformed, contradictory, or not canonically ordered.",
        .remediationHint = "Publish bounded canonical device and simulation-pose evidence from one exact session, time, and origin.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor TrackingSnapshotStale{
        .domain = XRDomain,
        .code = ErrorCode{"xr.tracking_snapshot.stale"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The retained XR tracking snapshot revision has been replaced.",
        .remediationHint = "Acquire the current immutable tracking snapshot before querying device pose evidence.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ActionProfileChanged{
        .domain = XRDomain,
        .code = ErrorCode{"xr.action_profile.changed"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The admitted XR interaction profile was lost or replaced.",
        .remediationHint = "Start a new XR Play session to prepare the current interaction profile.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ViewPlanInvalid{
        .domain = XRDomain,
        .code = ErrorCode{"xr.view_plan.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The XR view plan contains malformed, contradictory, or non-canonical evidence.",
        .remediationHint = "Publish one complete bounded runtime-ordered view set from the active configuration and prediction time.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ViewConfigurationStale{
        .domain = XRDomain,
        .code = ErrorCode{"xr.view_configuration.stale"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The retained XR view-configuration publication has been replaced.",
        .remediationHint = "Capture the current configuration identity and revision before acquiring render targets.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ExternalTargetInvalid{
        .domain = XRDomain,
        .code = ErrorCode{"xr.external_target.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An XR external render target has invalid identity, format, usage, extent, or ordering.",
        .remediationHint = "Use a current runtime-owned image and a compatible Horo color or depth attachment descriptor.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor LoaderPreflightInvalid{
        .domain = XRDomain,
        .code = ErrorCode{"xr.loader_preflight.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "XR loader preflight policy or evidence is malformed or exceeds its fixed work bound.",
        .remediationHint = "Use one verified backend/install record, known policy values, and bounded evidence from the same attempt.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor LoaderAbsent{
        .domain = XRDomain,
        .code = ErrorCode{"xr.loader.absent"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The exact product-selected XR loader source is absent.",
        .remediationHint =
            "Install or repair the verified loader selected by the product; Horo will not scan or fall back to another source.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor LoaderIncompatible{
        .domain = XRDomain,
        .code = ErrorCode{"xr.loader.incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The discovered XR loader API is outside the product's admitted compatibility interval.",
        .remediationHint = "Install a loader version admitted by the verified product composition record.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor LoaderOpenFailed{
        .domain = XRDomain,
        .code = ErrorCode{"xr.loader.open_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The verified XR loader artifact could not be opened through the platform capability.",
        .remediationHint = "Repair the verified loader artifact or its declared native dependency closure.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor RuntimeUnavailable{
        .domain = XRDomain,
        .code = ErrorCode{"xr.runtime.unavailable"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The selected loader could not discover an active XR runtime.",
        .remediationHint = "Install, enable, or start the product-qualified active runtime before retrying preflight.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor RuntimeRejected{
        .domain = XRDomain,
        .code = ErrorCode{"xr.runtime.rejected"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The discovered XR runtime was rejected by explicit runtime or product policy.",
        .remediationHint = "Select a qualified runtime configuration matching the product composition and release policy.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SystemUnsupported{
        .domain = XRDomain,
        .code = ErrorCode{"xr.system.unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The active runtime exposes no XR system satisfying the requested product profile.",
        .remediationHint = "Use a qualified system or request a product profile supported by the active runtime and device.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SystemTemporarilyUnavailable{
        .domain = XRDomain,
        .code = ErrorCode{"xr.system.temporarily_unavailable"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The selected XR system is known but temporarily unavailable.",
        .remediationHint = "Restore the device/runtime connection and repeat the complete preflight attempt.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor RuntimeOverrideRejected{
        .domain = XRDomain,
        .code = ErrorCode{"xr.runtime_override.rejected"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested XR runtime override is not authorized for this product execution.",
        .remediationHint = "Use system-default selection or an explicitly approved non-shipping developer override.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor LoaderPreflightCancelled{
        .domain = XRDomain,
        .code = ErrorCode{"xr.loader_preflight.cancelled"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "XR loader preflight was cancelled before activation publication.",
        .remediationHint = "Start a new owner-issued preflight attempt when XR activation is requested again.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor LoaderPreflightStale{
        .domain = XRDomain,
        .code = ErrorCode{"xr.loader_preflight.stale"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "XR loader preflight evidence belongs to a replaced attempt or composition input.",
        .remediationHint = "Discard retained evidence and repeat preflight for the current composition generation.",
        .retryable = true,
        .userActionable = false,
    };

    /** @copydoc Descriptors */
    std::span<const ErrorCodeDescriptor *const> Descriptors() noexcept {
        static constexpr std::array descriptors{
            &ContractVersionInvalid,
            &ContractVersionIncompatible,
            &IdentityInvalid,
            &IdentityStale,
            &CapabilityDescriptorInvalid,
            &CapabilityStale,
            &OperationUnsupported,
            &OperationUnavailable,
            &OperationIncompatible,
            &PermissionDenied,
            &CapacityExceeded,
            &OperationInvalid,
            &CoordinateSpaceIncompatible,
            &TimeDomainIncompatible,
            &PoseInvalid,
            &OriginRevisionStale,
            &TrackingSnapshotInvalid,
            &TrackingSnapshotStale,
            &ActionProfileChanged,
            &ViewPlanInvalid,
            &ViewConfigurationStale,
            &ExternalTargetInvalid,
            &LoaderPreflightInvalid,
            &LoaderAbsent,
            &LoaderIncompatible,
            &LoaderOpenFailed,
            &RuntimeUnavailable,
            &RuntimeRejected,
            &SystemUnsupported,
            &SystemTemporarilyUnavailable,
            &RuntimeOverrideRejected,
            &LoaderPreflightCancelled,
            &LoaderPreflightStale,
        };
        return descriptors;
    }
}  // namespace Horo::XR::XRErrors
