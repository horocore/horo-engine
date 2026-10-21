#include "Horo/Application/UiStyleWorkspace.h"
#include "Horo/Runtime/Ui/UiStyleAsset.h"

#include <type_traits>
static_assert(
    std::is_same_v<decltype(Horo::Runtime::Ui::UiStyleAssetDocument{}.style.id.Bytes()), const Horo::Runtime::Ui::SerializedUiId &>);
static_assert(!std::is_copy_constructible_v<Horo::Application::UiStyleWorkspaceProducer>);
static_assert(std::is_same_v<decltype(Horo::Application::UiStyleWorkspaceCandidate{}.registry),
                             std::shared_ptr<const Horo::Runtime::Ui::RuntimeStyleRegistry>>);
