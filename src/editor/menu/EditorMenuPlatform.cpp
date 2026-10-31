#include "editor/menu/EditorMenuPlatform.h"

#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Platform/FileManager.h"

#include <exception>

namespace Horo::Editor {
    /** @copydoc RevealInNativeFileManager */
    bool RevealInNativeFileManager(const std::filesystem::path &absolutePath) noexcept {
        try {
            const auto result = RevealInFileManager(absolutePath);
            if (result.HasValue())
                return true;
            LOG_ERROR("editor.file_manager", "%s: %s", result.ErrorValue().code.Value().c_str(), result.ErrorValue().message.c_str());
        } catch (const std::exception &error) {
            LOG_ERROR("editor.file_manager", "File Manager request failed: %s", error.what());
        }
        return false;
    }
}  // namespace Horo::Editor

#if !defined(__APPLE__)
namespace Horo::Editor {
    /** @copydoc UsesNativeEditorMenuBar */
    bool UsesNativeEditorMenuBar() noexcept {
        return false;
    }

    /** @copydoc InstallNativeEditorMenuBar */
    void InstallNativeEditorMenuBar(const EditorMenuModel &, const ILocalizationService &) {
        // Non-Apple platforms use in-window ImGui menu bars rather than OS-level menus.
    }

    /** @copydoc PollNativeEditorMenuAction */
    std::optional<EditorMenuInvocation> PollNativeEditorMenuAction() noexcept {
        return std::nullopt;
    }

    /** @copydoc OpenInExternalEditor */
    bool OpenInExternalEditor(const std::filesystem::path &) noexcept {
        return false;
    }
}  // namespace Horo::Editor
#endif
