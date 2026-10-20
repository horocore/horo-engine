#pragma once

/** @file ProjectErrorDomain.h @brief Inert metadata for project compatibility, metadata and migration failures. */
#include "Horo/Foundation/ModuleDescriptor.h"

namespace Horo::Application {
    /** @brief Describes existing project failures without reading metadata, migrating files or activating services.
     * @return Independently owned inert metadata with process-lifetime immutable descriptor pointers.
     */
    [[nodiscard]] ModuleErrorDomainDescriptor ProjectErrorDomain();
}  // namespace Horo::Application
