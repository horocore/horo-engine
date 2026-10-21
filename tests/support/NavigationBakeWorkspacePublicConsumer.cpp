#include "Horo/Editor/NavigationBakeWorkspace.h"

#include <type_traits>
static_assert(std::is_default_constructible_v<Horo::Editor::NavigationBakeWorkspaceSnapshot>);
static_assert(std::is_copy_constructible_v<Horo::Editor::NavigationBakeWorkspaceAccess>);

static_assert(sizeof(Horo::Editor::NavigationOverlayFilter) > 0);

static_assert(std::is_copy_constructible_v<Horo::Editor::NavigationViewportAccess>);
