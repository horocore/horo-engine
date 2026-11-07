#pragma once
/** @file EditorColorPipelineStartup.h @brief Explicit cold activation of independently cooked viewport color and exposure packages. */
#include "EditorViewportColorTypes.h"

#include <filesystem>
#include <optional>

namespace Horo::Editor {
    class IEditorViewportRenderer;
    /** @brief Resolves only explicit input or one known installed subroot; never searches cwd/environment or runs tools.
     * @param explicitRoot User-selected absolute root, or absent. @param packagedAssets Actual executable-relative asset root.
     * @return Absolute known package root, absent when no installed feature exists, or original typed malformed-path failure. */
    [[nodiscard]] Result<std::optional<std::filesystem::path>> ResolveEditorColorPipelineRoot(
        const std::optional<std::filesystem::path> &explicitRoot, const std::filesystem::path &packagedAssets);
    /** @brief Loads exact read-only product generations before native frame admission. @param viewport Existing selected renderer.
     * @param root Explicit root containing color/ and exposure/. @param settings Exact authored exposure settings.
     * @return Success only after actual native programs/resources are admitted; unsupported backend remains explicit. */
    [[nodiscard]] Result<void> PrepareEditorColorPipeline(IEditorViewportRenderer &viewport, const std::filesystem::path &root,
                                                          const Render::ExposureSettings &settings,
                                                          const std::optional<std::filesystem::path> &lookRoot = std::nullopt);
}  // namespace Horo::Editor
