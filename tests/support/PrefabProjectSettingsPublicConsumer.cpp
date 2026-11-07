#include "Horo/Application/PrefabProjectSettings.h"
#include "Horo/Editor/PrefabProjectSettingsSession.h"

#include <type_traits>

static_assert(!std::is_default_constructible_v<Horo::Application::PrefabProjectSettings>);
static_assert(!std::is_aggregate_v<Horo::Application::PrefabProjectSettings>);
static_assert(!std::is_copy_constructible_v<Horo::Editor::PrefabProjectSettingsSession>);

int main() {
    const auto settings = Horo::Application::PrefabProjectSettings::Resolve({});
    return settings.HasValue() && Horo::Application::PrefabLimitFields().size() == 19 ? 0 : 1;
}
