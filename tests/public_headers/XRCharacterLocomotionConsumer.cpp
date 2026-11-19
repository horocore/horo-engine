#include "Horo/XR/XRCharacterLocomotion.h"

#include <concepts>
#include <type_traits>

using Horo::XR::XRCharacterLocomotionAdapter;
static_assert(!std::is_copy_constructible_v<XRCharacterLocomotionAdapter>);
static_assert(std::is_nothrow_move_constructible_v<XRCharacterLocomotionAdapter>);
static_assert(requires(XRCharacterLocomotionAdapter &adapter, const Horo::XR::XRLocomotionIntent &intent,
                       const Horo::XR::XRTrackingSnapshot &tracking, const Horo::Runtime::FixedStepContext &step) {
    {
        adapter.Submit(intent, tracking, intent.scope.assignmentRevision, step)
    } -> std::same_as<Horo::Result<Horo::XR::XRLocomotionAdmission>>;
});
