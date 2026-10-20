#pragma once

/** @file FoundationErrorDomains.h
 * @brief Inert error metadata for explicit host composition without private Foundation headers.
 */
#include "Horo/Foundation/ModuleDescriptor.h"

namespace Horo {
    /** @brief Describes the stable scheduler failure domain without constructing or accessing a scheduler.
     * @return Independently owned domain contribution with process-lifetime immutable descriptor pointers.
     * The composition root registers this metadata under horo.foundation and supplies its own exit mappings.
     */
    [[nodiscard]] ModuleErrorDomainDescriptor JobSystemErrorDomain();

    /** @brief Describes validated Scene Math failures without evaluating geometry or changing runtime state.
     * @return Independently owned domain contribution with process-lifetime immutable descriptor pointers.
     * The composition root registers this metadata under horo.foundation and supplies its own exit mappings.
     */
    [[nodiscard]] ModuleErrorDomainDescriptor SceneMathErrorDomain();
}  // namespace Horo
