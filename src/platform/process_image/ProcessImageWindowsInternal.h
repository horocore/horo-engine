#pragma once

#include "Horo/Foundation/Result.h"

namespace Horo::Platform::Detail {
    /** @brief Verifies a retained read-only file view against the actual Windows main executable through the kernel.
     * @param view A live read-only data mapping owned by the caller for this operation.
     * @return Typed unavailability for foreign files, invalid mappings or a missing native comparison API.
     * @details This private check never issues ProcessImageIdentity or accepts a pathname/digest as authority.
     */
    [[nodiscard]] Result<void> VerifyWindowsMainImageFileView(const void *view);
}  // namespace Horo::Platform::Detail
