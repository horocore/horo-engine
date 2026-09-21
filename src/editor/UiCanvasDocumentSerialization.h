#pragma once

#include "Horo/Editor/UiCanvasDocument.h"

#include <string>
#include <string_view>

namespace Horo::Editor::UiCanvasDocumentSerialization {
    /** @brief Parses one bounded versioned UI Canvas JSON payload. */
    [[nodiscard]] Result<Runtime::Ui::UiDocument> Parse(std::string_view contents);

    /** @brief Serializes one validated authored UI document deterministically. */
    [[nodiscard]] std::string Serialize(const Runtime::Ui::UiDocument &document);
}  // namespace Horo::Editor::UiCanvasDocumentSerialization
