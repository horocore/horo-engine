#include "CharacterWorldInternal.h"

namespace Horo::Character {
    const Physics::PhysicsSurfaceWorldBinding &CharacterWorld::SurfaceBinding() const noexcept {
        return impl_->surfaces;
    }

    CharacterEventCapture CharacterWorld::CaptureEvents(const CharacterEventCaptureRequest &request,
                                                        std::span<CharacterMovementEvent> events,
                                                        std::span<CharacterEventTerminalState> terminal) const noexcept {
        using enum Horo::Character::CharacterEventCaptureStatus;
        if (impl_->ownerThread != std::this_thread::get_id())
            return {.status = WrongThread};
        if (impl_->state.load() == CharacterWorldState::Destroyed)
            return {.status = Closed};
        if (impl_->ticking.load() || impl_->placementActive)
            return {.status = Busy};
        if (request.world != impl_->descriptor.identity || request.sceneGeneration != impl_->descriptor.sceneGeneration)
            return {.status = Stale};
        return impl_->fastPath.CaptureEvents(request.afterPublicationRevision, events, terminal);
    }

}  // namespace Horo::Character
