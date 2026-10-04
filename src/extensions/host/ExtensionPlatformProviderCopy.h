#pragma once

#include "../capabilities/asset_pipeline_points/ExternalAssetImporter.h"

namespace Horo::Extensions::Detail {
    /** @brief Borrows validated ABI text without taking ownership. */
    [[nodiscard]] std::string_view View(HoroExtensionStringView value) noexcept;
    /** @brief Checks the ABI text pointer and the bounded module/permission identity length. */
    [[nodiscard]] bool IsValidBoundedText(HoroExtensionStringView value) noexcept;
    /** @brief Checks the complete base provider ABI descriptor before copying or inspecting its permissions. */
    [[nodiscard]] bool IsValidPlatformProviderDescriptor(const HoroPlatformServicesProviderDescriptor *descriptor) noexcept;
    /** @brief Checks that the manifest explicitly grants the current module this exact provider contribution. */
    [[nodiscard]] bool IsDeclaredPlatformProvider(const AssetImporterRegistrationSession &session,
                                                  const HoroPlatformServicesProviderDescriptor &descriptor) noexcept;
    /**
     * @brief Copies a native provider descriptor and acquires its attributed executable lease.
     * @param session Owning activation transaction with validated manifest/module authority.
     * @param descriptor Borrowed descriptor whose base ABI profile was validated by registration.
     * @return Owned provider candidate, including executable lifetime.
     * @throws std::invalid_argument Invalid permission, operations profile or closed executable admission.
     */
    [[nodiscard]] ExtensionPlatformProviderCandidate CopyPlatformProviderCandidate(
        const AssetImporterRegistrationSession &session, const HoroPlatformServicesProviderDescriptor &descriptor);
}  // namespace Horo::Extensions::Detail
