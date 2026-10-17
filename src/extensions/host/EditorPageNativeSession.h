#pragma once
#include "../capabilities/asset_pipeline_points/ExternalAssetImporter.h"
#include "EditorPageAbiConversion.h"

namespace Horo::Extensions {
    /** @brief Copies a manifest-declared native page into the current load transaction without registering live surfaces. */
    HoroExtensionStatus RegisterExternalEditorPage(void *context, const HoroEditorPageDescriptor *descriptor,
                                                   HoroEditorPageSessionApi *api) noexcept;
    /** @brief Commits every staged page only after the complete native module set has been validated. */
    [[nodiscard]] Result<void> CommitEditorPages(std::span<const std::shared_ptr<ExtensionModuleLifetime>> lifetimes);
    /** @brief Rejects duplicate/missing native page registrations against exact manifest declarations before any publication. */
    [[nodiscard]] Result<void> ValidateEditorPageClaims(const AssetImporterRegistrationSession &registration);
}  // namespace Horo::Extensions
