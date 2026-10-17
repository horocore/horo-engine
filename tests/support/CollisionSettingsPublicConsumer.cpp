#include "Horo/Application/ProjectCollisionSettings.h"
#include "Horo/Editor/CollisionProjectSettingsSession.h"
#include "Horo/Editor/NativeAssetRegistrySource.h"
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

static_assert(std::is_abstract_v<Assets::IAssetRegistrySource>);
static_assert(
    std::same_as<decltype(std::declval<const Assets::IAssetRegistrySource &>().Capture()), Result<Assets::AssetRegistrySourceManifest>>);
static_assert(std::is_constructible_v<Editor::ProjectCollisionSettingsStore, DurableFileSystem &, Editor::ProjectMutationCoordinator &,
                                      std::filesystem::path, std::string, const Assets::IAssetRegistrySource &>);
static_assert(!std::is_constructible_v<Editor::ProjectCollisionSettingsStore, DurableFileSystem &, Editor::ProjectMutationCoordinator &,
                                       std::filesystem::path, std::string>);

int main() {
    // This public headless contract must return a typed error, never install an ambient default.
    const auto invalid = Application::LoadProjectCollisionSettings({}, "project");
    return invalid.HasError() && invalid.ErrorValue().domain.Value() == "horo.project.collision" ? 0 : 1;
}
