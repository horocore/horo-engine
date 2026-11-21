#include "Horo/Runtime/Ui/UiNavigationInput.h"

#include <type_traits>

static_assert(
    std::is_same_v<decltype(Horo::Runtime::Ui::UiNavigationInputDescriptor{}.glyphProvider), const Horo::Input::IInputGlyphProvider *>);

int main() {
    const auto actions = Horo::Runtime::Ui::DefaultUiNavigationActions(Horo::Input::InputContextId{"game.ui"});
    return actions.size() == Horo::Runtime::Ui::UiNavigationActionCount + 1 ? 0 : 1;
}
