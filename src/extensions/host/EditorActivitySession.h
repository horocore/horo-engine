#pragma once
#include "../capabilities/asset_pipeline_points/ExternalAssetImporter.h"
#include "Horo/Extensions/EditorActivityAbi.h"
#include "Horo/Extensions/EditorActivityHost.h"

/** @brief Copies a declared activity pair into an uncommitted load session; no live UI is published. */
extern "C" HoroExtensionStatus RegisterExternalEditorActivity(void *context, const HoroEditorActivityDescriptor *descriptor,
                                                              HoroEditorActivitySessionApi *session) noexcept;

namespace Horo::Extensions {
    /** @brief Publishes all already copied sessions for this extension, with rollback on any conflict. */
    [[nodiscard]] Result<void> CommitEditorActivities(std::span<const std::shared_ptr<ExtensionModuleLifetime>> lifetimes);
}  // namespace Horo::Extensions
