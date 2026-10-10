#include "Horo/Runtime/Ui/UiSceneReconciliation.h"

#include <type_traits>
static_assert(!std::is_copy_constructible_v<Horo::Runtime::Ui::UiSceneReconciliation>);
static_assert(!std::is_copy_constructible_v<Horo::Runtime::Ui::UiSceneReconciliation::Prepared>);

int main() {
    const auto generation = Horo::Runtime::Ui::UiOwnershipGeneration::Create(42);
    auto service = Horo::Runtime::Ui::UiSceneReconciliation::Create({generation.Value()});
    if (service.HasError())
        return 1;
    auto owner = std::move(service).Value();
    owner.Shutdown();
    return owner.CanReclaim() ? 0 : 2;
}
