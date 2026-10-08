#include "Horo/Runtime/UiAnimationRuntimeParticipant.h"

#include <type_traits>

using Horo::Runtime::RuntimeDispatchFacts;
using Horo::Runtime::RuntimeDispatchSource;
using Horo::Runtime::RuntimeDispatchStatus;
using Horo::Runtime::UiAnimationClockController;
using Horo::Runtime::UiAnimationRuntimeComposition;
using Horo::Runtime::UiAnimationRuntimeParticipant;
using Horo::Runtime::UiAnimationRuntimeReload;

static_assert(!std::is_copy_constructible_v<UiAnimationRuntimeComposition>);
static_assert(std::is_move_constructible_v<UiAnimationRuntimeComposition>);
static_assert(!std::is_default_constructible_v<UiAnimationRuntimeParticipant>);
static_assert(!std::is_copy_constructible_v<UiAnimationRuntimeParticipant>);
static_assert(std::is_default_constructible_v<UiAnimationClockController>);
static_assert(!std::is_copy_constructible_v<UiAnimationClockController>);
static_assert(std::is_nothrow_move_constructible_v<UiAnimationClockController>);
static_assert(std::is_same_v<decltype(std::declval<const RuntimeDispatchSource &>().BindingStatus()), RuntimeDispatchStatus>);
static_assert(std::is_trivially_copyable_v<RuntimeDispatchFacts>);

static_assert(
    std::is_same_v<decltype(std::declval<UiAnimationRuntimeParticipant &>().Navigate(Horo::Runtime::Ui::UiRouteOperationRequest{})),
                   Horo::Result<Horo::Runtime::Ui::UiRouteOperationId>>);
static_assert(std::is_same_v<decltype(std::declval<UiAnimationRuntimeParticipant &>().CancelNavigation(
                                 Horo::Runtime::Ui::UiRouteOperationId{}, Horo::Runtime::Ui::UiAnimationCancellation::Explicit)),
                             Horo::Result<void>>);

static_assert(std::is_nothrow_move_constructible_v<Horo::Runtime::Ui::UiAnimationReloadResult>);
static_assert(std::is_nothrow_move_constructible_v<UiAnimationRuntimeReload>);
static_assert(!std::is_copy_constructible_v<UiAnimationRuntimeReload>);
using ReloadSignature = Horo::Result<UiAnimationRuntimeReload> (UiAnimationRuntimeParticipant::*)(
    Horo::Runtime::Ui::UiReloadGeneration, Horo::Runtime::Ui::UiElementSlotAllocator &, Horo::Runtime::Ui::RuntimeStyleRegistry,
    Horo::Runtime::Ui::UiStyleResolver, Horo::Runtime::Ui::UiAnimationCanvasDefinition,
    const Horo::Runtime::Ui::UiAnimationReloadAdmission &, const Horo::CancellationToken &);
static_assert(std::is_same_v<decltype(&UiAnimationRuntimeParticipant::Reload), ReloadSignature>);

using NavigateSignature = Horo::Result<Horo::Runtime::Ui::UiRouteOperationId> (UiAnimationRuntimeParticipant::*)(
    const Horo::Runtime::Ui::UiRouteOperationRequest &);
static_assert(std::is_same_v<decltype(&UiAnimationRuntimeParticipant::Navigate), NavigateSignature>);
static_assert(std::is_same_v<decltype(std::declval<const UiAnimationClockController &>().Seek(Horo::Runtime::Ui::UiAnimationClockId{},
                                                                                              Horo::Runtime::Ui::UiDuration{})),
                             Horo::Result<Horo::Runtime::Ui::UiAnimationClockId>>);

using StepSignature = Horo::Result<void> (UiAnimationClockController::*)(Horo::Runtime::Ui::UiAnimationClockId,
                                                                         Horo::Runtime::Ui::UiDuration) const;
static_assert(std::is_same_v<decltype(&UiAnimationClockController::Step), StepSignature>);

int main() {
    const RuntimeDispatchSource absent;
    if (absent.BindingStatus() != RuntimeDispatchStatus::Invalid)
        return 1;
    UiAnimationClockController absentController;
    return absentController.Clock(Horo::Runtime::Ui::UiTimeDomain::Manual).HasError() ? 0 : 2;
}
