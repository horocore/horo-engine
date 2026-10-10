#include "Horo/Runtime/Render/PostProcessErrors.h"

#include "RenderErrorDescriptor.h"

namespace Horo::Render::PostProcessErrors {
    namespace {
        const ErrorDomainId Domain{"render.post_process"};
    }

    const ErrorCodeDescriptor InvalidSettings =
        Detail::MakeErrorDescriptor(Domain, "render.post_process.settings_invalid", ErrorSeverity::Error,
                                    "Post-process settings are invalid.",
                                    "Supply finite parameters and bounded positive quality/work counts.");
    const ErrorCodeDescriptor InvalidVolume =
        Detail::MakeErrorDescriptor(Domain, "render.post_process.volume_invalid", ErrorSeverity::Error, "A post-process volume is invalid.",
                                    "Supply unique non-zero IDs, finite world bounds, non-negative blend radii and weights in [0,1].");
    const ErrorCodeDescriptor InvalidProfile =
        Detail::MakeErrorDescriptor(Domain, "render.post_process.profile_invalid", ErrorSeverity::Error,
                                    "A post-process profile reference is invalid.",
                                    "Supply unique non-zero profile IDs and resolve every volume reference before publication.");
    const ErrorCodeDescriptor CapacityExceeded =
        Detail::MakeErrorDescriptor(Domain, "render.post_process.capacity_exceeded", ErrorSeverity::Error,
                                    "Post-process authoring exceeds a finite bound.",
                                    "Reduce profiles, volumes, recipes, or raise the admitted limit within engine bounds.");
    const ErrorCodeDescriptor InvalidGraph =
        Detail::MakeErrorDescriptor(Domain, "render.post_process.graph_invalid", ErrorSeverity::Error,
                                    "Post-process graph metadata or ordering is invalid.",
                                    "Supply complete generations and exactly one occurrence of every known effect in the order.");
    const ErrorCodeDescriptor MissingInput =
        Detail::MakeErrorDescriptor(Domain, "render.post_process.input_missing", ErrorSeverity::Error,
                                    "A post-process semantic input is missing.",
                                    "Provide the effect's declared depth, normal, roughness, velocity, history, or exposure input.");
    const ErrorCodeDescriptor IncompatibleInput =
        Detail::MakeErrorDescriptor(Domain, "render.post_process.input_incompatible", ErrorSeverity::Error,
                                    "Post-process input metadata is incompatible.",
                                    "Provide matching view/color/exposure/history generations, ACEScg RGBA16F color, and sampled "
                                    "single-view textures.");
    const ErrorCodeDescriptor UnsupportedRecipe =
        Detail::MakeErrorDescriptor(Domain, "render.post_process.recipe_unsupported", ErrorSeverity::Error,
                                    "No authored post-process recipe is admitted.",
                                    "Cook an explicitly supported variant and provide its required capabilities, format, queue, and finite "
                                    "budgets.");
    const ErrorCodeDescriptor AllocationFailed =
        Detail::MakeErrorDescriptor(Domain, "render.post_process.allocation_failed", ErrorSeverity::Error,
                                    "Post-process preparation storage could not be allocated.",
                                    "Reduce bounded authoring work and retry outside frame-hot execution.");
}  // namespace Horo::Render::PostProcessErrors
