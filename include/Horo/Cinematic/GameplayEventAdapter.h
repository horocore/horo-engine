#pragma once

/** @file GameplayEventAdapter.h @brief Application composition of native gameplay callbacks into cinematic dispatch. */

#include "Horo/Cinematic/EventDispatcher.h"
#include "Horo/Gameplay/GameModuleHost.h"

namespace Horo::Cinematic {
    /** @brief Acquires a native gameModule callback lease for one exact cooked event contract.
     * @param gameModule Active native gameModule; the returned lease pins its code and callback context.
     * @param binding Cooked event identity. @param schema Cooked payload schema.
     * @param context Host-admitted runtime context. Preview cannot receive native gameplay callbacks.
     * @param generation Non-zero host registry generation; replacement requires explicit revoke and reactivation.
     * @return Dispatcher registration or a typed unavailable/context failure before retaining any callback. */
    [[nodiscard]] Result<EventHandlerRegistration> BindGameplayEvent(const Gameplay::LoadedGameModule &gameModule, EventBindingId binding,
                                                                     EventPayloadSchemaId schema, EventRuntimeContext context,
                                                                     std::uint64_t generation);
}  // namespace Horo::Cinematic
