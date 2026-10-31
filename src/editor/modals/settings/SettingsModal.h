#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Editor/SettingsModalDraft.h"

#include <imgui.h>

namespace Horo::Editor {
    /**
     * @brief Draws the host-owned Settings modal presentation without opening or closing an ImGui popup.
     * @return A close request for explicit Cancel or header-close controls, otherwise no action.
     */
    [[nodiscard]] ModalFrameResult DrawSettingsModalPresentation(SettingsState &state, EditorSettingsService &settings,
                                                                 const EditorGuiContext &ctx, ImTextureID logo);
}  // namespace Horo::Editor
