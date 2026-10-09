#include "Horo/Runtime/Ui/UiTextEditing.h"
#include "Horo/Runtime/Ui/UiTextLayout.h"
#include "Horo/Runtime/Ui/UiTextUnicode.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Runtime::Ui::UiTextUnicodeRuntime>);
static_assert(!std::is_copy_constructible_v<Horo::Runtime::Ui::UiTextUnicodeAnalyzer>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Runtime::Ui::UiTextUnicodeRuntime>);
static_assert(std::is_copy_constructible_v<Horo::Runtime::Ui::UiTextUnicodeAnalysis>);
static_assert(std::is_standard_layout_v<Horo::Runtime::Ui::UiTextLayoutCluster>);
static_assert(std::is_standard_layout_v<Horo::Runtime::Ui::UiTextEditSnapshot>);

void VerifyRuntimeUiTextEditPublicContract() {
    auto text = Horo::Runtime::Ui::UiActionText::Create("public");
    auto buffer = Horo::Runtime::Ui::UiTextEditBuffer::Create({}, text.Value());
    auto editor = std::move(buffer).Value();
    (void)editor.Apply({Horo::Runtime::Ui::UiTextEditKind::Previous});
    (void)editor.Display();
}
