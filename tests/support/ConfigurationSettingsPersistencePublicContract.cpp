#include "Horo/Editor/EditorConfiguration.h"

#include <type_traits>

static_assert(std::is_constructible_v<Horo::Editor::ConfigurationSettingsSession, Horo::ConfigurationService &,
                                      Horo::Editor::ConfigurationSettingsPersistence>);
static_assert(std::is_same_v<decltype(Horo::Editor::ConfigurationSettingsPersistence{}.source), Horo::ConfigurationSource>);
