#pragma once

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::VulkanBackendErrors {
    extern const ErrorCodeDescriptor AlreadyInitialized;
    extern const ErrorCodeDescriptor InvalidConfig;
    extern const ErrorCodeDescriptor NotInitialized;
    extern const ErrorCodeDescriptor LoaderVersionUnsupported;
    extern const ErrorCodeDescriptor ApiVariantUnsupported;
    extern const ErrorCodeDescriptor InstanceExtensionMissing;
    extern const ErrorCodeDescriptor ValidationUnavailable;
    extern const ErrorCodeDescriptor AdapterDataInvalid;
    extern const ErrorCodeDescriptor AdapterNotFound;
    extern const ErrorCodeDescriptor AdapterRequirementsUnsupported;
    extern const ErrorCodeDescriptor AdapterRevisionStale;
    extern const ErrorCodeDescriptor PresentationUnsupported;
    extern const ErrorCodeDescriptor PresentationInUse;
    extern const ErrorCodeDescriptor DiscoveryStopped;
    extern const ErrorCodeDescriptor UnsupportedOperation;
    extern const ErrorCodeDescriptor EntryPointMissing;
    extern const ErrorCodeDescriptor NativeCallFailed;
    extern const ErrorCodeDescriptor EnumerationLimitExceeded;
    extern const ErrorCodeDescriptor PresentationExtensionUnsupported;
    extern const ErrorCodeDescriptor DeviceVersionUnsupported;
    extern const ErrorCodeDescriptor DeviceExtensionMissing;
    extern const ErrorCodeDescriptor RequiredFeaturesUnavailable;
    extern const ErrorCodeDescriptor GraphicsQueueUnavailable;
    extern const ErrorCodeDescriptor DriverUnsupported;
    extern const ErrorCodeDescriptor ResourceRequestInvalid;
    extern const ErrorCodeDescriptor ResourceUnsupported;
    extern const ErrorCodeDescriptor ResourceCreationFailed;
    extern const ErrorCodeDescriptor ResourceIdentityInvalid;
    extern const ErrorCodeDescriptor MemoryTypeUnavailable;
}  // namespace Horo::Render::VulkanBackendErrors
