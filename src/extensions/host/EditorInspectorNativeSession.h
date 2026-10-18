#pragma once
/** @file EditorInspectorNativeSession.h @brief Private whole-activation native inspector staging and claim validation. */
#include "../capabilities/asset_pipeline_points/ExternalAssetImporter.h"
#include "Horo/Extensions/EditorInspectorAbi.h"
#include "Horo/Extensions/EditorInspectorHost.h"

namespace Horo::Extensions {
    /** @brief Sole private adapter to the explicit host's never-reused native publication sequence. */
    struct EditorInspectorHostAccess final {
        static std::uint64_t Generation(EditorInspectorHost &host) noexcept {
            return host.AllocateGeneration();
        }
    };

    /** @brief Proves exact manifest-owned declarations before committing any native inspector. */
    [[nodiscard]] Result<void> ValidateEditorInspectorClaims(const AssetImporterRegistrationSession &registration);
    /** @brief Publishes every declaration under existing whole-activation rollback ownership. */
    [[nodiscard]] Result<void> CommitEditorInspectors(std::span<const std::shared_ptr<ExtensionModuleLifetime>> lifetimes);
    /** @brief Deep-copies a structurally checked native inspector declaration; permission remains separate host admission. */
    [[nodiscard]] Result<EditorInspectorDefinition> CopyInspectorDefinition(const HoroEditorInspectorDescriptor &input);
}  // namespace Horo::Extensions

/** @brief Copies inert native declarations through the C ABI callback into the current module load transaction.
 * @param context Native module registration session owned by the enclosing activation transaction.
 * @param descriptor Native declaration copied before callback return; never retained by pointer.
 * @return Native status preserving invalid input, rejected admission or successful staging.
 */
extern "C" HoroExtensionStatus RegisterExternalEditorInspector(void *context, const HoroEditorInspectorDescriptor *descriptor) noexcept;
