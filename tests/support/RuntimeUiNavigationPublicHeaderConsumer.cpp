#include "Horo/Runtime/Ui/UiNavigationInput.h"

int main() {
    const auto actions = Horo::Runtime::Ui::DefaultUiNavigationActions(Horo::Input::InputContextId{"game.ui"});
    return actions.size() == Horo::Runtime::Ui::UiNavigationActionCount + 1 ? 0 : 1;
}
