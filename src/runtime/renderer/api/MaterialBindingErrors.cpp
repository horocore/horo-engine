#include "Horo/Runtime/Render/MaterialBindingErrors.h"

#include "RenderErrorDescriptor.h"

namespace Horo::Render::MaterialBindingErrors {
    namespace {
        const ErrorDomainId Domain{"render.material_binding"};
    }

    const ErrorCodeDescriptor InvalidDescriptor =
        Detail::MakeErrorDescriptor(Domain, "render.material_binding.invalid_descriptor", ErrorSeverity::Error,
                                    "Material binding input is invalid.",
                                    "Supply exact cooked reflection and complete, correctly owned resource generations.");
    const ErrorCodeDescriptor Unsupported =
        Detail::MakeErrorDescriptor(Domain, "render.material_binding.unsupported", ErrorSeverity::Error,
                                    "The selected renderer cannot realize this material binding.",
                                    "Select an explicitly cooked and admitted compatible optional fallback or report unsupported.");
    const ErrorCodeDescriptor CapacityExceeded =
        Detail::MakeErrorDescriptor(Domain, "render.material_binding.capacity_exceeded", ErrorSeverity::Error,
                                    "Material generation or parameter storage capacity is exhausted.",
                                    "Retire consumer leases or increase finite preparation bounds outside frame execution.");
    const ErrorCodeDescriptor StaleBinding =
        Detail::MakeErrorDescriptor(Domain, "render.material_binding.stale_binding", ErrorSeverity::Error,
                                    "The exact material binding generation is not discoverable.",
                                    "Acquire the published generation before releasing its table reference.");
    const ErrorCodeDescriptor Closed =
        Detail::MakeErrorDescriptor(Domain, "render.material_binding.closed", ErrorSeverity::Error, "Material binding admission is closed.",
                                    "Create a new owner table after draining the old backend and consumer leases.");
    const ErrorCodeDescriptor WrongThread =
        Detail::MakeErrorDescriptor(Domain, "render.material_binding.wrong_thread", ErrorSeverity::Error,
                                    "Material binding operation used a different owner thread.",
                                    "Dispatch preparation and retirement to the creating render-capable thread.");
    const ErrorCodeDescriptor AllocationFailed =
        Detail::MakeErrorDescriptor(Domain, "render.material_binding.allocation_failed", ErrorSeverity::Error,
                                    "Material binding preparation storage allocation failed.",
                                    "Reduce bounded material storage and retry at a preparation safe point.");
    const ErrorCodeDescriptor BackendFailure =
        Detail::MakeErrorDescriptor(Domain, "render.material_binding.backend_failure", ErrorSeverity::Error,
                                    "The material adapter threw or returned no resident binding.",
                                    "Inspect the selected adapter and preserve the last successfully published generation.");

}  // namespace Horo::Render::MaterialBindingErrors
