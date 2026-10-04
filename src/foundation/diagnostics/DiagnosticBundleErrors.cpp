#include "../FoundationErrors.h"
#include "Horo/Foundation/Diagnostics/DiagnosticBundle.h"

namespace Horo::Diagnostics {
    /** @copydoc DiagnosticBundleErrorDomain */
    ModuleErrorDomainDescriptor DiagnosticBundleErrorDomain() {
        return {.id = ObservabilityErrors::InvalidBundleRequest.domain,
                .descriptors = {&ObservabilityErrors::InvalidBundleRequest, &ObservabilityErrors::BundleReadFailed,
                                &ObservabilityErrors::BundleWriteFailed, &ObservabilityErrors::BundleSizeExceeded}};
    }

}  // namespace Horo::Diagnostics
