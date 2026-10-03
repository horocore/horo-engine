#include "Horo/Cinematic/EventDispatcher.h"
#include "Horo/Cinematic/EventSession.h"
#include "Horo/Cinematic/EventTrack.h"
#include "Horo/Cinematic/EventTrackErrors.h"
#include "Horo/Cinematic/GameplayEventAdapter.h"
#include "Horo/Cinematic/ScriptEventCook.h"
#include "Horo/Foundation/BorrowedCallbackContext.h"
#include "Horo/Gameplay/GameEventRegistry.h"

#include <type_traits>

static_assert(!std::is_default_constructible_v<Horo::Cinematic::CookedEventPlan>);
static_assert(!std::is_copy_constructible_v<Horo::Cinematic::CinematicEventSession>);

static_assert(std::is_trivially_copyable_v<Horo::BorrowedCallbackContext>);
static_assert(!std::is_convertible_v<int *, Horo::BorrowedCallbackContext>);

int main() {
    return 0;
}
