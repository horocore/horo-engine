#include "Horo/Editor/PhysicsInspector.h"

#include <type_traits>
#include <utility>

static_assert(std::is_copy_constructible_v<Horo::Editor::PhysicsInspectorValues>);
static_assert(std::is_same_v<decltype(Horo::Editor::PreparePhysicsInspectorEdit(
                                 std::declval<const Horo::Editor::SceneTransactionSnapshot &>(), Horo::SceneSource::SceneObjectId{},
                                 std::declval<const Horo::Editor::PhysicsInspectorValues &>())),
                             Horo::Result<Horo::Editor::SceneObjectReplacement>>);

static_assert(
    std::is_same_v<decltype(Horo::Editor::ProjectPhysicsColliders(std::declval<std::span<const Horo::SceneSource::SceneObjectSnapshot>>())),
                   Horo::Result<std::vector<Horo::Editor::ColliderPresentation>>>);
static_assert(std::is_same_v<
              decltype(Horo::Editor::PrepareColliderTransformEdits(std::declval<const Horo::Editor::SceneTransactionSnapshot &>(),
                                                                   std::declval<std::span<const Horo::Editor::ColliderTransformEdit>>())),
              Horo::Result<std::vector<Horo::Editor::SceneObjectReplacement>>>);
