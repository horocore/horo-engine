#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/GuiScreen.h"
#include "Horo/Editor/ProjectOpenService.h"
#include "Horo/Editor/SettingsModal.h"

#include <concepts>

static_assert(requires(Horo::Editor::GuiScreen &screen, const Horo::Editor::ProjectOpenService &projects,
                       Horo::Editor::SettingsModalProjectOpening opening) {
    { screen.ProjectConfiguration() } -> std::same_as<Horo::Result<std::shared_ptr<Horo::Editor::ConfigurationSettingsSession>>>;
    { projects.PlatformPolicy() } -> std::same_as<const Horo::Application::PlatformServicesProjectValidationContext &>;
    { opening.configuration } -> std::same_as<std::shared_ptr<Horo::Editor::ConfigurationSettingsSession> &>;
});
