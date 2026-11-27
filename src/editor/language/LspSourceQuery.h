#pragma once

/** @file LspSourceQuery.h @brief Source selection passed to a language provider. */

#include <cstddef>

namespace Horo::Editor::Language {
    /** @brief Position or selection in the exact immutable source snapshot being queried. */
    struct LspSourceQuery final {
        std::size_t offset{};
        std::size_t length{};      /**< Optional selection extent for code actions. */
        bool includeDeclaration{}; /**< Reference lookup option. */
    };
}  // namespace Horo::Editor::Language
