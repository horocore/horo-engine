#include "Horo/Editor/UiInspectorResources.h"

#include <concepts>
#include <type_traits>
static_assert(std::constructible_from<Horo::Editor::UiInspectorResources, const Horo::Assets::AssetRegistry &>);
static_assert(std::is_same_v<decltype(Horo::Editor::UiInspectorResourceSnapshot{}.assets), Horo::Assets::AssetRegistrySnapshot>);
static_assert(std::is_same_v<decltype(Horo::Editor::UiInspectorResourceSnapshot{}.styles),
                             std::shared_ptr<const Horo::Runtime::Ui::RuntimeStyleRegistry>>);
