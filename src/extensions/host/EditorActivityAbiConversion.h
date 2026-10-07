#pragma once
#include "Horo/Extensions/EditorActivityAbi.h"
#include "Horo/Extensions/EditorSurfaceRegistry.h"

namespace Horo::Extensions::Detail {
    /** @brief Copies one bounded versioned ABI snapshot without calling provider code or publishing state. */
    [[nodiscard]] Result<EditorUiForm> CopyActivityForm(const HoroEditorActivitySnapshot &snapshot, std::string_view drawerId,
                                                        std::string_view titleKey);
    /** @brief Copies a bounded ABI string; malformed strings fail before allocating. */
    [[nodiscard]] bool CopyActivityText(HoroExtensionStringView input, std::string &output, std::size_t maximum = 256);
}  // namespace Horo::Extensions::Detail
