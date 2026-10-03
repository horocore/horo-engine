#include "Horo/Cinematic/GameplayEventAdapter.h"

#include "Horo/Cinematic/EventTrackErrors.h"
#include "Horo/Gameplay/GameEventRegistry.h"

namespace Horo::Cinematic {
    namespace {
        /** @brief Translates stable occurrence evidence to the pinned gameplay owner contract. */
        EventDispatchOutcome InvokeGameplay(const BorrowedCallbackContext &context, const EventDispatchRequest &request) {
            const auto *lease = context.Get<Gameplay::GameplayEventLease>();
            if (lease == nullptr)
                return EventDispatchOutcome::HandlerFailed;
            const auto &occurrence = request.occurrence;
            const Gameplay::GameplayEventRequest native{occurrence.player.session.stableValue,
                                                        occurrence.player.session.generation,
                                                        occurrence.player.player.stableValue,
                                                        occurrence.player.player.generation,
                                                        occurrence.track.stableValue,
                                                        occurrence.track.generation,
                                                        occurrence.key.stableValue,
                                                        occurrence.key.generation,
                                                        occurrence.traversal,
                                                        request.committedTick,
                                                        occurrence.direction == SequenceTraversalDirection::Reverse,
                                                        request.payload};
            using enum Gameplay::GameplayEventOutcome;
            switch (lease->Invoke(native)) {
                case Accepted:
                    return EventDispatchOutcome::Accepted;
                case SuppressedByAuthority:
                    return EventDispatchOutcome::SuppressedByAuthority;
                case CapabilityUnavailable:
                    return EventDispatchOutcome::CapabilityUnavailable;
                case InvalidTarget:
                    return EventDispatchOutcome::InvalidTarget;
                case Backpressured:
                    return EventDispatchOutcome::Backpressured;
                case HandlerFailed:
                    return EventDispatchOutcome::HandlerFailed;
            }
            return EventDispatchOutcome::HandlerFailed;
        }
    }  // namespace

    /** @copydoc BindGameplayEvent */
    Result<EventHandlerRegistration> BindGameplayEvent(const Gameplay::LoadedGameModule &gameModule, const EventBindingId binding,
                                                       const EventPayloadSchemaId schema, const EventRuntimeContext context,
                                                       const std::uint64_t generation) {
        using enum EventRuntimeContext;
        if (!binding.IsValid() || !schema.IsValid() || generation == 0 || (context != Runtime && context != Pie && context != Headless))
            return Result<EventHandlerRegistration>::Failure(MakeError(EventTrackErrors::BindingUnavailable));
        auto acquired = gameModule.Events().Acquire(binding.stableValue, binding.generation, schema.stableValue, schema.generation,
                                                    gameModule.Cancellation());
        if (acquired.HasError())
            return Result<EventHandlerRegistration>::Failure(MakeError(EventTrackErrors::BindingUnavailable));
        auto lease = std::move(acquired).Value();
        EventHandlerRegistration registration{binding,        schema, context, generation, BorrowedCallbackContext{lease.get()},
                                              InvokeGameplay, lease};
        return Result<EventHandlerRegistration>::Success(std::move(registration));
    }
}  // namespace Horo::Cinematic
