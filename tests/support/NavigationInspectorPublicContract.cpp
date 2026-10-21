#include "Horo/Editor/NavigationInspector.h"

#include <type_traits>
static_assert(std::is_copy_constructible_v<Horo::Editor::NavigationInspectorEdit>);
static_assert(std::variant_size_v<Horo::Editor::NavigationInspectorValue> == 5);
