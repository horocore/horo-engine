#pragma once

/** @file SettingsPresentationFixture.h @brief Shared headless GUI, configuration and modal owners for settings presentation contracts. */
#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/SettingsModal.h"
#include "Horo/Extensions/ExtensionInventory.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <filesystem>
#include <stdexcept>

namespace Horo::Editor::Tests {
    struct SettingsPresentationFixture {
        explicit SettingsPresentationFixture(const ImVec2 displaySize = {1280.0F, 800.0F}, Horo::ConfigurationSchema schema = {})
            : gui{displaySize}, configuration{Horo::Editor::CreateEditorConfigurationService(Horo::Editor::DefaultEditorSettings(), nullptr,
                                                                                             std::move(schema))},
              settings{Horo::Editor::DefaultEditorSettings(), configuration, gui.editorEvents, gui.localization},
              modal{gui.context, settings, 0} {
            if (const auto refreshed = extensions.Refresh(); refreshed.HasError())
                throw std::runtime_error(refreshed.ErrorValue().message);
            Horo::Editor::LoadSettingsForModal(modal.Draft(), settings);
            modal.Draft().extensionInventory = &extensions;
        }

        Horo::Editor::Tests::EditorGuiContextFixture gui;
        Horo::ConfigurationService configuration = Horo::Editor::CreateEditorConfigurationService(Horo::Editor::DefaultEditorSettings());
        Horo::Editor::EditorSettingsService settings;
        Horo::Extensions::ExtensionInventory extensions{std::filesystem::temp_directory_path() / "horo-settings-presentation-extensions"};
        Horo::Editor::SettingsModal modal;
    };

}  // namespace Horo::Editor::Tests
