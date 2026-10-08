#include "Horo/Runtime/Ui/UiAnimationClock.h"
#include "Horo/Runtime/Ui/UiAnimationOwner.h"
#include "Horo/Runtime/Ui/UiAnimationTimeline.h"
#include "Horo/Runtime/Ui/UiAnimationTracks.h"
#include "Horo/Runtime/Ui/UiLayout.h"
#include "Horo/Runtime/Ui/UiScreenStack.h"
#include "Horo/Runtime/Ui/UiStyle.h"

#include <type_traits>

using namespace Horo::Runtime::Ui;
static_assert(UiTimeDomainCount == 6);
static_assert(!std::is_convertible_v<UiStyleGeometryRevision, UiStylePublicationRevision>);
static_assert(std::is_same_v<decltype(UiComputedStyleSnapshotDescriptor{}.geometry), UiStyleGeometryRevision>);
static_assert(!std::is_convertible_v<UiAnimationTimelineId, UiAnimationClockId>);
static_assert(!std::is_convertible_v<UiAnimationTimelineId, UiElementHandle>);
static_assert(std::is_copy_constructible_v<UiAnimationTimePolicy>);
static_assert(!std::is_constructible_v<UiAnimationHostRead>);
static_assert(!std::is_copy_constructible_v<UiAnimationHostRead>);
static_assert(!std::is_move_constructible_v<UiAnimationHostRead>);
static_assert(!std::is_copy_constructible_v<UiStyleResolver::PreparedUpdate>);
static_assert(std::is_nothrow_move_constructible_v<UiStyleResolver::PreparedUpdate>);
static_assert(!std::is_copy_constructible_v<UiLayoutEngine::PreparedUpdate>);
static_assert(std::is_nothrow_move_constructible_v<UiLayoutEngine::PreparedUpdate>);
static_assert(!std::is_convertible_v<UiAnimationClockId, UiElementHandle>);
static_assert(!std::is_convertible_v<UiAnimationHostSourceId, UiAnimationClockId>);
static_assert(!std::is_convertible_v<UiAnimationHostSourceId, UiElementHandle>);
static_assert(std::is_same_v<decltype(std::declval<UiAnimationHostRead>().Source()), UiAnimationHostSourceId>);
static_assert(std::is_same_v<decltype(std::declval<UiScreenStack &>().DrainRetiredActions()), Horo::Result<std::size_t>>);
template <typename T>
concept CanDrainRetiredActions = requires(T &stack) { stack.DrainRetiredActions(); };
template <typename T>
concept ExposesAnimationGate = requires { typename T::AnimationGate; };
template <typename T>
concept ExposesAnimationTerminalProof = requires { typename T::AnimationTerminalProof; };
static_assert(!ExposesAnimationGate<UiScreenStack>);
static_assert(!ExposesAnimationTerminalProof<UiScreenStack>);
static_assert(CanDrainRetiredActions<UiScreenStack>);
static_assert(!CanDrainRetiredActions<const UiScreenStack>);
static_assert(!std::is_default_constructible_v<UiAnimationOwner>);
static_assert(!std::is_copy_constructible_v<UiAnimationOwner>);
static_assert(!std::is_default_constructible_v<UiAnimationOwner::Prepared>);
static_assert(!std::is_copy_constructible_v<UiAnimationOwner::Prepared>);
static_assert(std::is_nothrow_move_constructible_v<UiAnimationOwner::Prepared>);
static_assert(std::is_nothrow_copy_constructible_v<UiAnimationFrameLease>);

using ReloadSignature = Horo::Result<UiAnimationReloadResult> (UiAnimationOwner::*)(UiReloadGeneration, UiElementSlotAllocator &,
                                                                                    RuntimeStyleRegistry, UiStyleResolver,
                                                                                    UiAnimationCanvasDefinition,
                                                                                    const UiAnimationReloadAdmission &,
                                                                                    const Horo::CancellationToken &);
static_assert(std::is_same_v<decltype(&UiAnimationOwner::Reload), ReloadSignature>);
static_assert(std::is_same_v<decltype(std::declval<const RuntimeStyleRegistry &>().BeginRetirement()), Horo::Result<void>>);

int main() {
    return UiPlaybackRate{}.IsValid() ? 0 : 1;
}
