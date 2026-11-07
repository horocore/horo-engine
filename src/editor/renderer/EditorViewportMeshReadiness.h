#pragma once
/** @file EditorViewportMeshReadiness.h @brief Exact resident geometry admission before viewport light selection. */

#include "editor/renderer/EditorViewportResources.h"

namespace Horo::Editor {
    /** @brief Checks creation readiness and the exact source generation of an already-resolved resident projection.
     * @param resident Projection obtained from the canonical resource owner for the requested source ID.
     * @param generation Frozen instance's expected source generation.
     * @return True only for an actually Ready mesh generation; this does not claim GPU completion. */
    [[nodiscard]] inline bool IsEditorViewportMeshGenerationReady(const EditorViewportResources::MeshBinding &resident,
                                                                  const std::uint32_t generation) noexcept {
        return resident.ready && resident.sourceGeneration == generation;
    }

    /** @brief Requires the actual ready geometry to match every frozen instance's source generation.
     * @param resources Existing canonical resident mesh owner, borrowed only during this call.
     * @param instances Frozen draw instances whose bounds will drive conservative light selection.
     * @return Success without allocation, or typed Pending before encoding while a source is absent or being replaced.
     * @details Keeping an older ready mesh during upload does not authorize shading it using newer bounds. */
    [[nodiscard]] inline Result<void> ValidateEditorViewportMeshReadiness(
        const EditorViewportResources &resources, const std::span<const Render::RenderStaticMeshInstance> instances) {
        for (const auto &instance : instances) {
            const auto resident = resources.FindMesh(instance.mesh.id.value);
            if (!resident || !IsEditorViewportMeshGenerationReady(*resident, instance.mesh.generation))
                return Result<void>::Failure(
                    MakeError(Render::LightCullingErrors::Pending,
                              "The frozen viewport instance awaits its exact resident mesh source generation before light selection."));
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
