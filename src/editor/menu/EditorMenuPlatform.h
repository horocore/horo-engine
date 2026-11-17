#pragma once

#include "Horo/Editor/EditorMenuModel.h"

#include <filesystem>
#include <optional>

namespace Horo::Editor {
    class ILocalizationService;

    /** @brief Reports whether this host uses the operating system's global application menu bar. */
    [[nodiscard]] bool UsesNativeEditorMenuBar() noexcept;

    /**
     * @brief Installs or refreshes the native application menu from the shared model.
     * @param model Platform-neutral menu hierarchy.
     * @param localization Active editor localization service.
     */
    void InstallNativeEditorMenuBar(const EditorMenuModel &model, const ILocalizationService &localization);

    /**
     * @brief Takes the next native menu invocation queued by the platform callback.
     * @return Pending invocation, or empty when no native menu command is waiting.
     */
    [[nodiscard]] std::optional<EditorMenuInvocation> PollNativeEditorMenuAction() noexcept;

    /**
     * @brief Opens an editor-owned directory or reveals a file through the platform desktop adapter.
     * @param absolutePath Existing absolute regular file or directory, without a symbolic link.
     * @return True when the platform accepted the reveal request.
     * @pre Called on the host main thread after a user action.
     * @note Linux opens the containing directory rather than selecting the file. Typed failures are logged at this GUI boundary.
     */
    [[nodiscard]] bool RevealInNativeFileManager(const std::filesystem::path &absolutePath) noexcept;

    /**
     * @brief Opens one absolute source file in the operating system's configured external editor.
     * @param absolutePath Existing absolute source path.
     * @return True when the platform accepted the open request.
     */
    [[nodiscard]] bool OpenInExternalEditor(const std::filesystem::path &absolutePath) noexcept;
}  // namespace Horo::Editor
