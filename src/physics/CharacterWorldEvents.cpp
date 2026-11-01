#include "CharacterWorldInternal.h"

namespace Horo::Character {
    const Physics::PhysicsSurfaceWorldBinding &CharacterWorld::SurfaceBinding() const noexcept {
        return impl_->surfaces;
    }

    CharacterEventCapture CharacterWorld::CaptureEvents(const CharacterEventCaptureRequest &request,
                                                        std::span<CharacterMovementEvent> events,
                                                        std::span<CharacterEventTerminalState> terminal) const noexcept {
        if (impl_->ownerThread != std::this_thread::get_id())
            return {.status = CharacterEventCaptureStatus::WrongThread};
        if (impl_->state.load() == CharacterWorldState::Destroyed)
            return {.status = CharacterEventCaptureStatus::Closed};
        if (impl_->ticking.load() || impl_->placementActive)
            return {.status = CharacterEventCaptureStatus::Busy};
        if (request.world != impl_->descriptor.identity || request.sceneGeneration != impl_->descriptor.sceneGeneration)
            return {.status = CharacterEventCaptureStatus::Stale};
        return impl_->fastPath.CaptureEvents(request.afterPublicationRevision, events, terminal);
    }

}  // namespace Horo::Character
