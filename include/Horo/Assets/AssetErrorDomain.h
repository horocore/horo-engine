#pragma once

/** @file AssetErrorDomain.h @brief Inert metadata for implemented asset failures. */
#include "Horo/Foundation/ModuleDescriptor.h"

namespace Horo::Assets {
    /** @brief Describes implemented asset failures without discovery, cooking, storage access or service activation.
     * @return Independently owned metadata with process-lifetime immutable descriptor pointers.
     */
    [[nodiscard]] ModuleErrorDomainDescriptor AssetErrorDomain();
}  // namespace Horo::Assets
