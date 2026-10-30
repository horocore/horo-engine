#pragma once

/** @file EditorGuiLiveReload.h
 * @brief Optional host-owned development presentation and frame-boundary RCC++ publication.
 */

#include "Horo/Editor/GuiLiveReload.h"
#include "Horo/Foundation/Result.h"

#include <filesystem>
#include <memory>
#include <string>

namespace Horo::Editor {
    /** @brief Exact host build inputs supplied by executable composition; never game/project sources. */
    struct EditorGuiLiveReloadConfiguration {
        std::filesystem::path hostImage;
        std::filesystem::path intermediateRoot;
        std::filesystem::path includeManifest;
        std::string compileOptions;
        std::string linkOptions;
        std::filesystem::path compiler;
    };

    /** @brief Owns compiler/runtime modules until all development draws have stopped. */
    class EditorGuiLiveReload final : public GuiLive::IRegistry {
    public:
        EditorGuiLiveReload();
        ~EditorGuiLiveReload();
        EditorGuiLiveReload(const EditorGuiLiveReload &) = delete;
        EditorGuiLiveReload &operator=(const EditorGuiLiveReload &) = delete;
        /** @brief Initializes the engine-development GUI compiler; a disabled build is inert.
         * @param configuration Exact host-generated compiler inputs; never inferred from an open project.
         * @return Success after complete initialization, or a typed actionable failure. Repeated successful starts are inert.
         */
        [[nodiscard]] Result<void> Start(const EditorGuiLiveReloadConfiguration &configuration);
        /** @brief Publishes a completed replacement before the next frame begins.
         * @param elapsedSeconds Host elapsed time for file-change notification coalescing.
         */
        void Update(double elapsedSeconds);
        /** @brief Returns this borrowed service only in an explicitly enabled development build. */
        [[nodiscard]] const GuiLive::IRegistry *Registry() const noexcept;
        /** @copydoc GuiLive::IRegistry::Find */
        [[nodiscard]] GuiLive::Binding Find(GuiLive::Id route, GuiLive::Id signature) const noexcept override;

    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Editor
