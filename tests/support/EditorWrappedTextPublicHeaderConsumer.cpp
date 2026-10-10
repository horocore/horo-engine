#include "Horo/Editor/EditorUiComponents.h"

#include <type_traits>

using WrappedText = void (*)(float, std::string_view, ImVec4, const Horo::Editor::Theme::Fonts &);
static_assert(std::is_same_v<decltype(&Horo::Editor::Ui::WrappedText), WrappedText>);
