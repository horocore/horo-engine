#include "VulkanRenderBackendErrors.h"

namespace Horo::Render::VulkanBackendErrors {
    namespace {
        const ErrorDomainId Domain{"horo.render.vulkan"};
    }

    const ErrorCodeDescriptor AlreadyInitialized{Domain, ErrorCode{"render.backend.already_initialized"}, ErrorSeverity::Error,
                                                 "Vulkan backend is already initialized.",
                                                 "Shut down the backend before initializing it again."};
    const ErrorCodeDescriptor InvalidConfig{Domain, ErrorCode{"render.backend.invalid_config"}, ErrorSeverity::Error,
                                            "Vulkan configuration is invalid.", "Use a valid adapter and presentation policy."};
    const ErrorCodeDescriptor NotInitialized{Domain, ErrorCode{"render.backend.not_initialized"}, ErrorSeverity::Error,
                                             "Vulkan backend is not initialized.", "Initialize the backend before use."};
    const ErrorCodeDescriptor LoaderVersionUnsupported{Domain,
                                                       ErrorCode{"render.vulkan.loader_version_unsupported"},
                                                       ErrorSeverity::Critical,
                                                       "The Vulkan loader cannot provide Vulkan 1.3.",
                                                       "Install or repair a Vulkan 1.3 loader and driver.",
                                                       false,
                                                       true};
    const ErrorCodeDescriptor ApiVariantUnsupported{Domain,
                                                    ErrorCode{"render.vulkan.api_variant_unsupported"},
                                                    ErrorSeverity::Critical,
                                                    "The Vulkan API variant is unsupported.",
                                                    "Use a native standard Vulkan implementation.",
                                                    false,
                                                    true};
    const ErrorCodeDescriptor InstanceExtensionMissing{Domain,
                                                       ErrorCode{"render.vulkan.instance_extension_missing"},
                                                       ErrorSeverity::Critical,
                                                       "A required Vulkan instance extension is unavailable.",
                                                       "Repair the Vulkan loader or selected window-system integration.",
                                                       false,
                                                       true};
    const ErrorCodeDescriptor ValidationUnavailable{Domain,
                                                    ErrorCode{"render.vulkan.validation_unavailable"},
                                                    ErrorSeverity::Error,
                                                    "Requested Vulkan validation is unavailable.",
                                                    "Install VK_LAYER_KHRONOS_validation or disable the explicit validation request.",
                                                    false,
                                                    true};
    const ErrorCodeDescriptor AdapterDataInvalid{Domain, ErrorCode{"render.vulkan.adapter_data_invalid"}, ErrorSeverity::Critical,
                                                 "Vulkan adapter discovery returned invalid data.", "Update or repair the Vulkan driver."};
    const ErrorCodeDescriptor AdapterNotFound{Domain,
                                              ErrorCode{"render.vulkan.adapter_not_found"},
                                              ErrorSeverity::Error,
                                              "The requested Vulkan adapter was not found.",
                                              "Select an available adapter or refresh adapter discovery.",
                                              false,
                                              true};
    const ErrorCodeDescriptor AdapterRequirementsUnsupported{Domain,
                                                             ErrorCode{"render.vulkan.adapter_requirements_unsupported"},
                                                             ErrorSeverity::Critical,
                                                             "No Vulkan adapter satisfies the required device contract.",
                                                             "Use a Vulkan 1.3 device with required features and queues.",
                                                             false,
                                                             true};
    const ErrorCodeDescriptor AdapterRevisionStale{Domain,
                                                   ErrorCode{"render.vulkan.adapter_revision_stale"},
                                                   ErrorSeverity::Error,
                                                   "The Vulkan adapter discovery revision is stale.",
                                                   "Refresh adapter discovery and retry explicit selection.",
                                                   true};
    const ErrorCodeDescriptor PresentationUnsupported{Domain,
                                                      ErrorCode{"render.vulkan.presentation_unsupported"},
                                                      ErrorSeverity::Error,
                                                      "The selected Vulkan adapter cannot present through the requested host surface.",
                                                      "Select a compatible adapter or use headless rendering.",
                                                      false,
                                                      true};
    const ErrorCodeDescriptor PresentationInUse{Domain,
                                                ErrorCode{"render.vulkan.presentation_in_use"},
                                                ErrorSeverity::Error,
                                                "Vulkan initialization is already using the shared runtime port.",
                                                "Shut down the active Vulkan backend before creating another.",
                                                true};
    const ErrorCodeDescriptor DiscoveryStopped{Domain, ErrorCode{"render.vulkan.discovery_stopped"}, ErrorSeverity::Error,
                                               "Vulkan adapter discovery has stopped.",
                                               "Create a new discovery service before querying adapters."};
    const ErrorCodeDescriptor UnsupportedOperation{Domain, ErrorCode{"render.vulkan.unsupported_operation"}, ErrorSeverity::Error,
                                                   "This Vulkan operation is not implemented by the current component stage.",
                                                   "Use a component that implements the required Vulkan command or resource stage."};
    const ErrorCodeDescriptor EntryPointMissing{Domain,
                                                ErrorCode{"render.vulkan.entry_point_missing"},
                                                ErrorSeverity::Critical,
                                                "The Vulkan loader or driver is missing a required entry point.",
                                                "Repair or update the Vulkan loader and selected device driver.",
                                                false,
                                                true};
    const ErrorCodeDescriptor NativeCallFailed{Domain,
                                               ErrorCode{"render.vulkan.native_call_failed"},
                                               ErrorSeverity::Critical,
                                               "A required Vulkan initialization call failed.",
                                               "Inspect the Vulkan result and repair the loader, driver, or device configuration.",
                                               true,
                                               true};
    const ErrorCodeDescriptor EnumerationLimitExceeded{Domain,
                                                       ErrorCode{"render.vulkan.enumeration_limit_exceeded"},
                                                       ErrorSeverity::Error,
                                                       "Vulkan enumeration exceeded its bounded startup limit.",
                                                       "Disable unexpected layers or reduce the enumerated device/extension set.",
                                                       false,
                                                       true};
    const ErrorCodeDescriptor PresentationExtensionUnsupported{Domain,
                                                               ErrorCode{"render.vulkan.presentation_extension_unsupported"},
                                                               ErrorSeverity::Error,
                                                               "The platform returned an unsupported Vulkan presentation extension.",
                                                               "Use the qualified Win32, Xlib, XCB, or Wayland Vulkan presentation path.",
                                                               false,
                                                               true};
    const ErrorCodeDescriptor DeviceVersionUnsupported{Domain,
                                                       ErrorCode{"render.vulkan.device_version_unsupported"},
                                                       ErrorSeverity::Critical,
                                                       "The selected device cannot provide Vulkan 1.3.",
                                                       "Select or install a Vulkan 1.3-capable device driver.",
                                                       false,
                                                       true};
    const ErrorCodeDescriptor DeviceExtensionMissing{Domain,
                                                     ErrorCode{"render.vulkan.device_extension_missing"},
                                                     ErrorSeverity::Error,
                                                     "The selected device lacks a required Vulkan extension.",
                                                     "Select a device that supports the required presentation contract.",
                                                     false,
                                                     true};
    const ErrorCodeDescriptor
        RequiredFeaturesUnavailable{Domain,
                                    ErrorCode{"render.vulkan.required_features_unavailable"},
                                    ErrorSeverity::Critical,
                                    "The selected device lacks a required Vulkan 1.3 feature.",
                                    "Select a device supporting dynamic rendering, synchronization2, and timeline semaphores.",
                                    false,
                                    true};
    const ErrorCodeDescriptor GraphicsQueueUnavailable{Domain,
                                                       ErrorCode{"render.vulkan.graphics_queue_unavailable"},
                                                       ErrorSeverity::Critical,
                                                       "The selected device has no usable graphics queue.",
                                                       "Select a device exposing a graphics-capable queue family.",
                                                       false,
                                                       true};
    const ErrorCodeDescriptor DriverUnsupported{Domain,
                                                ErrorCode{"render.vulkan.driver_unsupported"},
                                                ErrorSeverity::Critical,
                                                "The selected Vulkan driver is blocked by qualification policy.",
                                                "Install a qualified driver version or select another adapter.",
                                                false,
                                                true};
    const ErrorCodeDescriptor ResourceRequestInvalid{Domain, ErrorCode{"render.vulkan.resource_request_invalid"}, ErrorSeverity::Error,
                                                     "A Vulkan resource request or admitted placement is invalid.",
                                                     "Revalidate the descriptor and reserve its exact reported memory plan."};
    const ErrorCodeDescriptor ResourceUnsupported{Domain, ErrorCode{"render.vulkan.resource_unsupported"}, ErrorSeverity::Error,
                                                  "The selected Vulkan device cannot realize this resource policy.",
                                                  "Use a supported format, dimension, sample count, usage, or host-access policy."};
    const ErrorCodeDescriptor ResourceCreationFailed{Domain,
                                                     ErrorCode{"render.vulkan.resource_creation_failed"},
                                                     ErrorSeverity::Critical,
                                                     "The Vulkan driver failed to realize an admitted resource.",
                                                     "Inspect device memory pressure and driver diagnostics.",
                                                     true,
                                                     true};
    const ErrorCodeDescriptor ResourceIdentityInvalid{Domain, ErrorCode{"render.vulkan.resource_identity_invalid"}, ErrorSeverity::Error,
                                                      "A Vulkan resource references an unknown or retired backend identity.",
                                                      "Use only ready resource generations owned by this backend."};
    const ErrorCodeDescriptor MemoryTypeUnavailable{Domain,
                                                    ErrorCode{"render.vulkan.memory_type_unavailable"},
                                                    ErrorSeverity::Error,
                                                    "No Vulkan memory type satisfies the admitted resource policy.",
                                                    "Select a compatible device or resource access policy.",
                                                    false,
                                                    true};
}  // namespace Horo::Render::VulkanBackendErrors
