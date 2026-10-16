#include "Horo/Application/ProjectCollisionSettings.h"
#include "Horo/Editor/CollisionProjectSettingsSession.h"
#include "Horo/Editor/ProjectCollisionSettingsStore.h"

#include <concepts>
#include <type_traits>
#include <utility>

using namespace Horo;
static_assert(!std::is_convertible_v<Physics::CollisionProfileId, Physics::CollisionLayerId>);
static_assert(!std::is_convertible_v<Physics::PhysicsQueryChannelId, Physics::CollisionProfileId>);
static_assert(std::same_as<decltype(std::declval<const Application::ProjectCollisionSettings &>().Schema()),
                           const std::shared_ptr<const Physics::NormalizedCollisionSchema> &>);
static_assert(!std::is_copy_constructible_v<Editor::CollisionProjectSettingsSession>);
static_assert(std::same_as<decltype(std::declval<const Editor::CollisionSettingsReview &>().Impacts()),
                           std::span<const Editor::CollisionReferenceImpact>>);

int main() {
    // This public headless contract must return a typed error, never install an ambient default.
    const auto invalid = Application::LoadProjectCollisionSettings({}, "project");
    return invalid.HasError() && invalid.ErrorValue().domain.Value() == "horo.project.collision" ? 0 : 1;
}
