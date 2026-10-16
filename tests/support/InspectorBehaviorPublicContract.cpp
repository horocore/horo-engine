#include "Horo/Editor/InspectorBehaviorBinding.h"
#include "Horo/Editor/SceneDocumentTransaction.h"

#include <type_traits>

namespace {
    using Owner = Horo::Editor::SceneDocumentTransactions;
    static_assert(!std::is_copy_constructible_v<Owner> && std::is_nothrow_move_constructible_v<Owner>);
    static_assert(std::is_same_v<decltype(std::declval<const Owner &>().CheckCurrent()), Horo::Result<void>>);
    static_assert(std::is_same_v<decltype(Horo::Editor::InspectorBehaviorValue::value), Horo::Gameplay::BehaviorFieldValue>);
    static_assert(Horo::Editor::MaximumInspectorBehaviorSelection == 256);
}  // namespace
