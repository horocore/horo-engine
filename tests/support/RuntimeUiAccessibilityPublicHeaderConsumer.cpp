#include "Horo/Runtime/Ui/UiAccessibilityChanges.h"

#include <type_traits>

using namespace Horo::Runtime::Ui;
static_assert(std::is_member_function_pointer_v<decltype(&UiAccessibilitySnapshot::ReadingOrder)>);
static_assert(std::is_member_function_pointer_v<decltype(&UiAccessibilitySnapshot::FocusOrder)>);
static_assert(std::is_member_function_pointer_v<decltype(&UiAccessibilitySnapshot::FocusState)>);

int main() {
    const auto owner = UiOwnershipGeneration::Create(1).Value();
    SerializedUiId bytes{};
    bytes.back() = 1;
    auto result =
        UiAccessibilityChangePublisher::Create({{owner, 1, 1}, {owner, 2, 1}, UiDocumentId::Create(bytes).Value(), {1, 0, 0, 64}, 1});
    if (result.HasError())
        return 1;
    auto publisher = std::move(result).Value();
    if (!publisher.Announcements().empty() || !publisher.Acknowledge({}).HasError())
        return 1;
    publisher.Retire();
    return publisher.IsRetired() ? 0 : 1;
}
