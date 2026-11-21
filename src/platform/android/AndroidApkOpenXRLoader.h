#pragma once

/** @file AndroidApkOpenXRLoader.h
 * @brief Private installed-APK loader lease for an explicitly selected Android OpenXR profile.
 */

#include "AndroidGameActivityJni.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Platform/DynamicLibrary.h"

namespace Horo::Platform::Android {
    /** @brief Opens the pinned library for this process ABI directly from the installed base APK.
     * @param application Live GameActivity's retained VM and application Context.
     * @param imageDigest Compiled product pin for the exact bundled ABI image.
     * @return Native loader that retains the Context through unload, or a typed error with no native owner.
     * @pre Called on the host lifecycle lane with an explicitly selected monolithic APK profile.
     */
    [[nodiscard]] Result<std::shared_ptr<const DynamicLibrary>> LoadBundledOpenXRLoader(const GameActivityJniApplication &application,
                                                                                        const Sha256Digest &imageDigest);
}  // namespace Horo::Platform::Android
