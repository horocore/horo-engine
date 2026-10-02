#include <Horo/Runtime/Ui/UiBindingStore.h>
#include <type_traits>

static_assert(std::is_base_of_v<Horo::Runtime::Ui::UiActionHandler, Horo::Runtime::Ui::UiBindingWriteActionHandler>);
static_assert(!std::is_copy_constructible_v<Horo::Runtime::Ui::UiBindingStore>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Runtime::Ui::UiBindingStore>);
static_assert(noexcept(std::declval<Horo::Runtime::Ui::UiBindingWriteAuthority &>().Prepare(
    std::declval<const Horo::Runtime::Ui::UiBindingWriteCommand &>())));

int main() {
    const Horo::Runtime::Ui::UiBindingWriteAdmission admission;
    return admission.authority ? 1 : 0;
}
