#include "Horo/Runtime/Render/MultiviewExtractionErrors.h"

#include "RenderErrorDescriptor.h"

namespace Horo::Render::MultiviewExtractionErrors {
    namespace {
        const ErrorDomainId Domain{"render.multiview"};
    }

    const ErrorCodeDescriptor InvalidInput = Detail::
        MakeErrorDescriptor(Domain, "render.multiview.invalid_input", ErrorSeverity::Error,
                            "Multiview input or current owner state is invalid.",
                            "Provide live publication fences, dense ordered views, finite transforms and exact resident identities.");
    const ErrorCodeDescriptor StalePublication =
        Detail::MakeErrorDescriptor(Domain, "render.multiview.stale_publication", ErrorSeverity::Error,
                                    "Multiview presentation belongs to an obsolete publication.",
                                    "Extract from the current scene, frame, origin, view configuration and selected-device publication.");
    const ErrorCodeDescriptor CapacityExceeded = Detail::
        MakeErrorDescriptor(Domain, "render.multiview.capacity_exceeded", ErrorSeverity::Error,
                            "Multiview input or expansion exceeds its work envelope.",
                            "Reduce scene work or admit larger bounded capacities before extraction; views and draws are never truncated.");
    const ErrorCodeDescriptor Unsupported = Detail::MakeErrorDescriptor(Domain, "render.multiview.unsupported", ErrorSeverity::Error,
                                                                        "The selected backend cannot execute the requested view policy.",
                                                                        "Admit an explicitly supported multipass path or require a backend "
                                                                        "with indexed shaders and instanced target routing.");
}  // namespace Horo::Render::MultiviewExtractionErrors
