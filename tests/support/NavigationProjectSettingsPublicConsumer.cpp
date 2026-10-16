#include "Horo/Application/NavigationProjectSettings.h"
#include "Horo/Application/NavigationProjectSettingsStore.h"
#include "Horo/Editor/NavigationDefinitionDocument.h"
#include "Horo/Editor/NavigationProjectSettingsSession.h"
#include "Horo/Editor/NavigationSettingsWorkflow.h"

#include <type_traits>
static_assert(!std::is_copy_constructible_v<Horo::Editor::NavigationDefinitionDocument>);
static_assert(!std::is_copy_constructible_v<Horo::Editor::NavigationProjectSettingsSession>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Application::NavigationProjectSettings &>().RuntimeProfile()),
                             const Horo::Navigation::NavigationProjectProfile &>);

static_assert(std::is_same_v<decltype(std::declval<const Horo::Editor::NavigationProjectSettingsSession &>().Definitions()),
                             Horo::Result<std::span<const Horo::Editor::NavigationDefinitionSnapshot>>>);

static_assert(std::is_copy_constructible_v<Horo::Editor::NavigationProjectSettingsSessionBorrow>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Editor::NavigationProjectSettingsSession &>().Borrow()),
                             Horo::Editor::NavigationProjectSettingsSessionBorrow>);
static_assert(noexcept(std::declval<const Horo::Editor::NavigationProjectSettingsSessionBorrow &>().Resolve()));

int main() {
    const auto malformed = Horo::Application::DecodeNavigationProjectSettings("{}");
    return malformed.HasError() ? 0 : 1;
}
