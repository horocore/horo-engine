#pragma once
/** @file EditorViewportNativeSession.h @brief Private whole-activation native viewport staging and claim validation. */
#include "../capabilities/asset_pipeline_points/ExternalAssetImporter.h"
#include "Horo/Extensions/EditorViewportAbi.h"
#include "Horo/Extensions/EditorViewportHost.h"

namespace Horo::Extensions {
    /** @brief Sole private adapter to the explicit host's never-reused native publication sequence. */
    struct EditorViewportHostAccess final {
        static std::uint64_t Generation(EditorViewportHost &host) noexcept {
            return host.AllocateGeneration();
        }
    };

    /** @brief Proves exact manifest-owned declarations before committing any native viewport. */
    [[nodiscard]] Result<void> ValidateEditorViewportClaims(const AssetImporterRegistrationSession &registration);
    /** @brief Publishes every declaration under existing whole-activation rollback ownership. */
    [[nodiscard]] Result<void> CommitEditorViewports(std::span<const std::shared_ptr<ExtensionModuleLifetime>> lifetimes);
    /** @brief Deep-copies a structurally checked native viewport declaration; permission remains separate host admission. */
    [[nodiscard]] Result<EditorViewportDefinition> CopyViewportDefinition(const HoroEditorViewportDescriptor &input);
}  // namespace Horo::Extensions

/** @brief Copies inert native declarations through the C ABI callback into the current module load transaction.
 * @param context Native module registration session owned by the enclosing activation transaction.
 * @param descriptor Native declaration copied before callback return; never retained by pointer.
 * @return Native status preserving invalid input, rejected admission or successful staging.
 */
extern "C" HoroExtensionStatus RegisterExternalEditorViewport(void *context, const HoroEditorViewportDescriptor *descriptor) noexcept;
