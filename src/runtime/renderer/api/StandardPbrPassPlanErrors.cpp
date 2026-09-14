#include "Horo/Runtime/Render/StandardPbrPassPlanErrors.h"

#include "RenderErrorDescriptor.h"

namespace Horo::Render::StandardPbrPassPlanErrors {
    namespace {
        const ErrorDomainId Domain{"render.standard_pbr_pass_plan"};
    }

    const ErrorCodeDescriptor InvalidRequest =
        Detail::MakeErrorDescriptor(Domain, "render.standard_pbr_pass_plan.invalid_request", ErrorSeverity::Error,
                                    "The standard PBR pass-plan request is invalid.",
                                    "Provide a known raster family, non-zero recipe generation, finite capacity, and a required depth "
                                    "prepass for Clustered Forward.");
    const ErrorCodeDescriptor InvalidMaterial =
        Detail::MakeErrorDescriptor(Domain, "render.standard_pbr_pass_plan.invalid_material", ErrorSeverity::Error,
                                    "A standard PBR pass material is invalid.",
                                    "Provide valid material and source identities with non-zero generation-safe pipeline handles.");
    const ErrorCodeDescriptor UnsupportedMaterialClass =
        Detail::MakeErrorDescriptor(Domain, "render.standard_pbr_pass_plan.unsupported_material_class", ErrorSeverity::Error,
                                    "The opaque/depth pass plan cannot admit this material class.",
                                    "Route translucent and additive materials through their separately ordered frontend pass policy.");
    const ErrorCodeDescriptor DuplicateMaterial =
        Detail::MakeErrorDescriptor(Domain, "render.standard_pbr_pass_plan.duplicate_material", ErrorSeverity::Error,
                                    "A runtime material identity appears more than once in the pass plan.",
                                    "Submit each exact runtime material identity and generation once per plan.");
    const ErrorCodeDescriptor CapacityExceeded =
        Detail::MakeErrorDescriptor(Domain, "render.standard_pbr_pass_plan.capacity_exceeded", ErrorSeverity::Error,
                                    "The standard PBR pass plan exceeds its admitted material capacity.",
                                    "Split the bounded plan or raise the composition-root limit within the hard engine maximum.");
    const ErrorCodeDescriptor AllocationFailed =
        Detail::MakeErrorDescriptor(Domain, "render.standard_pbr_pass_plan.allocation_failed", ErrorSeverity::Error,
                                    "Standard PBR pass-plan storage could not be allocated.",
                                    "Reduce bounded material work and retry outside frame-hot execution.");
}  // namespace Horo::Render::StandardPbrPassPlanErrors
