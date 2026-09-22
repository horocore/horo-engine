#include "Horo/Runtime/Ui/UiErrors.h"

namespace Horo::Runtime::Ui::UiErrors {
    namespace {
        const ErrorDomainId UiDomain{"horo.runtime_ui"};
    }

    /** @copydoc DocumentSchemaUnsupported */
    const ErrorCodeDescriptor DocumentSchemaUnsupported{UiDomain,
                                                        ErrorCode{"runtime_ui.document.schema_unsupported"},
                                                        ErrorSeverity::Error,
                                                        "The Runtime UI document schema is unsupported.",
                                                        "Upgrade the engine or apply an explicit bounded document migration.",
                                                        false,
                                                        true};
    /** @copydoc DocumentSerializationInvalid */
    const ErrorCodeDescriptor DocumentSerializationInvalid{UiDomain,
                                                           ErrorCode{"runtime_ui.document.serialization_invalid"},
                                                           ErrorSeverity::Error,
                                                           "The serialized Runtime UI document contains malformed data.",
                                                           "Provide the complete canonical schema with valid typed values and identities.",
                                                           false,
                                                           true};
    /** @copydoc DocumentPayloadTooLarge */
    const ErrorCodeDescriptor
        DocumentPayloadTooLarge{UiDomain,
                                ErrorCode{"runtime_ui.document.payload_too_large"},
                                ErrorSeverity::Error,
                                "The serialized Runtime UI document exceeds a bounded content limit.",
                                "Reduce source bytes, hierarchy, property, route, or text content within the declared limits.",
                                false,
                                true};
    /** @copydoc DocumentDuplicateProperty */
    const ErrorCodeDescriptor DocumentDuplicateProperty{UiDomain,
                                                        ErrorCode{"runtime_ui.document.duplicate_property"},
                                                        ErrorSeverity::Error,
                                                        "The Runtime UI element repeats a typed property key.",
                                                        "Keep one canonical value for each element property identity.",
                                                        false,
                                                        true};
    /** @copydoc DocumentReferenceInvalid */
    const ErrorCodeDescriptor
        DocumentReferenceInvalid{UiDomain,
                                 ErrorCode{"runtime_ui.document.reference_invalid"},
                                 ErrorSeverity::Error,
                                 "The Runtime UI document contains an invalid authored reference.",
                                 "Use stable typed identities and reference only resident document or declared asset identities.",
                                 false,
                                 true};
    /** @copydoc DocumentHierarchyInvalid */
    const ErrorCodeDescriptor DocumentHierarchyInvalid{UiDomain,
                                                       ErrorCode{"runtime_ui.document.hierarchy_invalid"},
                                                       ErrorSeverity::Error,
                                                       "The Runtime UI document hierarchy is disconnected or cyclic.",
                                                       "Provide one canvas root per tree and an acyclic connected parent relation.",
                                                       false,
                                                       true};
    /** @copydoc DocumentRouteInvalid */
    const ErrorCodeDescriptor DocumentRouteInvalid{UiDomain,
                                                   ErrorCode{"runtime_ui.document.route_invalid"},
                                                   ErrorSeverity::Error,
                                                   "The Runtime UI route metadata is invalid or duplicated.",
                                                   "Provide unique stable route identities and a supported presentation band.",
                                                   false,
                                                   true};
    /** @copydoc DocumentMigrationMissing */
    const ErrorCodeDescriptor DocumentMigrationMissing{UiDomain,
                                                       ErrorCode{"runtime_ui.document.migration_missing"},
                                                       ErrorSeverity::Error,
                                                       "No explicit Runtime UI document migration reaches the requested schema.",
                                                       "Supply a bounded exact migration chain or keep the source schema unchanged.",
                                                       false,
                                                       true};
    /** @copydoc DocumentMigrationInvalid */
    const ErrorCodeDescriptor DocumentMigrationInvalid{UiDomain,
                                                       ErrorCode{"runtime_ui.document.migration_invalid"},
                                                       ErrorSeverity::Error,
                                                       "The Runtime UI document migration graph is invalid or ambiguous.",
                                                       "Use unique forward edges with valid versions and non-null migration functions.",
                                                       false,
                                                       true};
}  // namespace Horo::Runtime::Ui::UiErrors
