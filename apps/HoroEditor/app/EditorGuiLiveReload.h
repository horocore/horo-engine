#pragma once

/** @file EditorGuiLiveReload.h
 * @brief Optional host-owned development presentation and frame-boundary RCC++ publication.
 */

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Foundation/Result.h"

#include <memory>

namespace Horo::Editor {
    /** @brief Owns compiler/runtime modules until all development draws have stopped. */
    class EditorGuiLiveReload final : public GuiLive::IRegistry {
    public:
        EditorGuiLiveReload();
        ~EditorGuiLiveReload();
        EditorGuiLiveReload(const EditorGuiLiveReload &) = delete;
        EditorGuiLiveReload &operator=(const EditorGuiLiveReload &) = delete;
        /** @brief Initializes the explicitly enabled development compiler; a disabled build is inert. */
        [[nodiscard]] Result<void> Start();
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
