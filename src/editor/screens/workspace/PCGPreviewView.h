#pragma once
/** @file PCGPreviewView.h @brief Bounded point/output overlay rendering for the registered PCG surface. */
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/PCGPreviewSession.h"

namespace Horo::Editor {
    class ILocalizationService;
    /** @brief Closed presentation intent; neither action changes authored source or Scene. */
    enum class PCGPreviewAction : std::uint8_t {
        None,
        Refresh,
        Cancel
    };
    /** @brief Draws point positions and generated output bounds in an isolated top-down overlay.
     * @param preview Current exact-fence projection. @param region Captured provider bounds used by the orthographic view.
     * @param maximumPoints Global point/bounds draw budget. @param localization Shared catalog. @param fonts Shared typography.
     * @return Typed session-only control intent. No callbacks or authoritative state are borrowed. */
    [[nodiscard]] PCGPreviewAction DrawPCGPreview(const PCGPreviewSnapshot &preview, const Math::Aabb &region, std::size_t maximumPoints,
                                                  const ILocalizationService &localization, const Theme::Fonts &fonts);
}  // namespace Horo::Editor
