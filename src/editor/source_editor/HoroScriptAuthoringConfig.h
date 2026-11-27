#pragma once

/** @file HoroScriptAuthoringConfig.h @brief Project-relative script-to-module authoring bindings. */

#include "Horo/Editor/EditorSurfaceIdentity.h"

#include <filesystem>
#include <span>
#include <vector>

namespace Horo::Editor::Language {
    /** @brief An exact source identity and two identifiers resolved by the current host. */
    struct ScriptModuleSourceBinding final {
        SourceDocumentId source;
        std::string package;
        std::string module;
    };

    /** @brief Loads bounded, optional script-language configuration from the canonical project root. */
    [[nodiscard]] Result<std::vector<ScriptModuleSourceBinding>> LoadScriptModuleSourceBindings(const std::filesystem::path &root);

    /** @brief Finds the one selected module for a source in the loaded, source-sorted bindings. */
    [[nodiscard]] const ScriptModuleSourceBinding *FindScriptModuleSourceBinding(std::span<const ScriptModuleSourceBinding> bindings,
                                                                                 const SourceDocumentId &source) noexcept;
}  // namespace Horo::Editor::Language
