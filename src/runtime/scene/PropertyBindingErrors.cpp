#include "Horo/Runtime/Scene/PropertyBindingErrors.h"

namespace Horo::Runtime::PropertyBindingErrors {
    namespace {
        const ErrorDomainId Domain{"horo.scene.property_binding"};
    }

    const ErrorCodeDescriptor InvalidDescriptor{.domain = Domain,
                                                .code = ErrorCode{"scene.property_binding.invalid_descriptor"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "The property binding descriptor is invalid.",
                                                .remediationHint = "Provide a stable identity, component/property type, and validating accessors.",
                                                .userActionable = true};
    const ErrorCodeDescriptor DuplicateBinding{.domain = Domain,
                                               .code = ErrorCode{"scene.property_binding.duplicate"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "The property binding registry contains a duplicate binding.",
                                               .remediationHint = "Register each stable binding and component/property pair once.",
                                               .userActionable = true};
    const ErrorCodeDescriptor RegistryFrozen{.domain = Domain,
                                             .code = ErrorCode{"scene.property_binding.registry_frozen"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "The property binding registry is already frozen.",
                                             .remediationHint = "Register descriptors during explicit host composition before activation."};
    const ErrorCodeDescriptor BindingMissing{.domain = Domain,
                                             .code = ErrorCode{"scene.property_binding.missing"},
                                             .defaultSeverity = ErrorSeverity::Warning,
                                             .summary = "The requested property binding is not available.",
                                             .remediationHint = "Revalidate the scene/component schema and acquire the current binding generation.",
                                             .retryable = true,
                                             .userActionable = true};
    const ErrorCodeDescriptor ComponentTypeMismatch{.domain = Domain,
                                                    .code = ErrorCode{"scene.property_binding.component_type_mismatch"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "The property binding target has an incompatible component type.",
                                                    .remediationHint = "Resolve the target against the descriptor's exact component type.",
                                                    .userActionable = true};
    const ErrorCodeDescriptor ValueTypeMismatch{.domain = Domain,
                                                .code = ErrorCode{"scene.property_binding.value_type_mismatch"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "The property binding value has an incompatible type.",
                                                .remediationHint = "Use the typed value declared by the binding descriptor.",
                                                .userActionable = true};
    const ErrorCodeDescriptor ReadRejected{.domain = Domain,
                                           .code = ErrorCode{"scene.property_binding.read_rejected"},
                                           .defaultSeverity = ErrorSeverity::Warning,
                                           .summary = "The property binding owner rejected a typed read.",
                                           .remediationHint = "Retry at the owner's permitted scene boundary.",
                                           .retryable = true};
    const ErrorCodeDescriptor WriteRejected{.domain = Domain,
                                            .code = ErrorCode{"scene.property_binding.write_rejected"},
                                            .defaultSeverity = ErrorSeverity::Warning,
                                            .summary = "The property binding owner rejected a typed write.",
                                            .remediationHint = "Use the owner's admitted write phase or repair the target state.",
                                            .retryable = true};
    const ErrorCodeDescriptor ValueOutOfRange{.domain = Domain,
                                              .code = ErrorCode{"scene.property_binding.value_out_of_range"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The property binding value is outside its declared range.",
                                              .remediationHint = "Keep authored values within the binding's finite constraint.",
                                              .userActionable = true};
}  // namespace Horo::Runtime::PropertyBindingErrors
