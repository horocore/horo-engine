#pragma once

/** @file AndroidWindowErrors.h
 * @brief Non-installed Android window error metadata for explicit platform host composition.
 */
#include "Horo/Foundation/ModuleDescriptor.h"

namespace Horo::Platform::Android {
    /** @brief Stable platform-owned descriptors shared by failure sites and inert composition. */
    namespace AndroidWindowErrors {
        /** @brief Reentrant native-window operation rejected before partial state is observed. */
        extern const ErrorCodeDescriptor OperationInProgress;
        /** @brief Fixed host selection cannot accept Android native windows. */
        extern const ErrorCodeDescriptor UnsupportedBackend;
        /** @brief Explicit no-presentation host cannot accept native-window presentation. */
        extern const ErrorCodeDescriptor NoPresentation;
        /** @brief Native reference acquisition failed without transferring ownership. */
        extern const ErrorCodeDescriptor AcquireFailed;
    }  // namespace AndroidWindowErrors

    /**
     * @brief Contribute Android window errors to the existing horo.platform module, without activation.
     * @return Independently owned domain container borrowing process-lifetime descriptors.
     * Merge descriptors into the owner's single horo.platform domain claim if it already exists.
     * Never register a competing Android module or overlap a separately owned nested domain.
     * The caller must validate the complete selected module set before activation.
     */
    [[nodiscard]] ModuleErrorDomainDescriptor AndroidWindowErrorDomain();
}  // namespace Horo::Platform::Android
