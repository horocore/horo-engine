#pragma once

#include "EditorInspectionCommandEndpoint.h"
#include "Horo/Mcp/McpToolRegistry.h"

namespace Horo::Editor {
    inline constexpr std::string_view EditorSceneInspectionGrant = "editor.scene.inspect";
    inline constexpr std::string_view EditorSceneProposalGrant = "editor.scene.propose";

    /** @brief Registers owner-bound inspection and, when supplied, the separate preview-only proposal tool. */
    [[nodiscard]] Result<std::vector<Mcp::McpToolRegistration>> MakeEditorInspectionMcpTools(
        std::weak_ptr<const EditorInspectionCommandEndpoint> workspace);

    /** @brief Stages an already catalog-validated Scene intent on the bound editor owner lane.
     * @param endpoint Exact endpoint issued to the admitted editor session. @param tool Registered intent name.
     * @param arguments Arguments from the validated, immutable intent. @param authorized Retained review freshness.
     * @return A viewport review ID. Applying it still requires the normal editor confirmation.
     */
    [[nodiscard]] Result<std::uint64_t> StageEditorScenePlan(const EditorInspectionCommandEndpoint &endpoint, std::string_view tool,
                                                             const nlohmann::json &arguments, std::function<bool()> authorized);

    /** @brief Append reference viewport tools whose required workspace operations are composed. */
    void AppendEditorViewportTools(const std::shared_ptr<const EditorInspectionCommandEndpoint> &endpoint,
                                   std::vector<Mcp::McpToolRegistration> &tools);
}  // namespace Horo::Editor
