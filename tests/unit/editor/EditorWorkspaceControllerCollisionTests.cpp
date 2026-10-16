#include "CollisionSettingsTestSupport.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <nlohmann/json.hpp>

namespace Horo::Editor {
    using namespace Tests::CollisionSettings;

    namespace {
        void PrepareWorkspace(Project &project) {
            project.Scene(SceneSource::EncodeSceneSource({{}, {}}));
            const auto release = Application::CurrentEngineReleaseVersion();
            const auto *decision = Application::BuiltInReleaseCompatibilityRegistry().Find(release);
            REQUIRE(decision);
            const auto source =
                nlohmann::json{{"horoVersion", Application::FormatHoroVersion(release.value)},
                               {"persistentContract", Application::FormatPersistentContractHash(decision->persistentContract)},
                               {"projectId", "collision-project"},
                               {"name", "Collision"},
                               {"projectVersion", "0.1.0"},
                               {"createdAt", "2026-10-16T00:00:00Z"},
                               {"settings", {{"renderBackend", "opengl"}, {"defaultScene", "Assets/Scene.horoasset"}}}};
            Write(project.root / ".horo/project.json", source.dump());
        }

        EditorWorkspaceDependencies Services(Project &project) {
            return {.mutableAssetRegistry = &project.registry,
                    .collisionInventory = &project.inventory,
                    .collisionActivation = &project.activation,
                    .mutations = &project.mutations,
                    .durableFiles = &project.files,
                    .documentSessionOwner = IdOf<Assets::AssetId>(40)};
        }
    }  // namespace

    TEST_CASE("Workspace collision Save Copy and rejected Save As retain exact authoring owner", "[gui][collision][workspace]") {
        Project project;
        PrepareWorkspace(project);
        Runtime::RuntimeSceneService runtime;
        EditorWorkspaceController controller{project.root, runtime, project.registry.Snapshot(), Services(project)};
        auto opened = controller.CollisionSettings();
        REQUIRE(opened.HasValue());
        auto owner = opened.Value();
        auto input = owner->Snapshot().Value()->Input();
        input.layers.front().name = "Unsaved collision";
        Commit(*owner, input);
        EditorWorkspaceViewCommandData copy{.command = EditorWorkspaceViewCommand::SaveSceneCopyAs,
                                            .stringPayload = (project.root / "Assets/Copy.horoasset").string()};
        controller.ProcessCommand(copy);
        CHECK(std::filesystem::exists(project.root / "Assets/Copy.horoasset"));
        CHECK(controller.CollisionSettings().Value() == owner);
        CHECK(owner->IsDirty());
        EditorWorkspaceViewCommandData invalid{.command = EditorWorkspaceViewCommand::SaveSceneAs,
                                               .stringPayload = (project.root / ".horo/Rejected.horoasset").string()};
        controller.ProcessCommand(invalid);
        CHECK(controller.CollisionSettings().Value() == owner);
        CHECK(owner->Snapshot().HasValue());
        CHECK(controller.ShutdownCollisionSettings().HasError());
        REQUIRE(controller.ShutdownCollisionSettings(true).HasValue());
        CHECK(owner->Snapshot().HasError());
    }

    TEST_CASE("Workspace collision clean Save As replaces the Scene port and revokes escaped modal owners", "[gui][collision][workspace]") {
        Project project;
        PrepareWorkspace(project);
        Runtime::RuntimeSceneService runtime;
        EditorWorkspaceController controller{project.root, runtime, project.registry.Snapshot(), Services(project)};
        auto old = controller.CollisionSettings();
        REQUIRE(old.HasValue());
        EditorWorkspaceViewCommandData saveAs{.command = EditorWorkspaceViewCommand::SaveSceneAs,
                                              .stringPayload = (project.root / "Assets/New.horoasset").string()};
        controller.ProcessCommand(saveAs);
        CHECK(old.Value()->Snapshot().HasError());
        auto replacement = controller.CollisionSettings();
        REQUIRE(replacement.HasValue());
        CHECK(replacement.Value() != old.Value());
        CHECK(replacement.Value()->Snapshot().HasValue());
    }

    TEST_CASE("Workspace destruction closes collision weak presentation before canonical Scene owners retire",
              "[gui][collision][workspace]") {
        Project project;
        PrepareWorkspace(project);
        Runtime::RuntimeSceneService runtime;
        std::shared_ptr<CollisionProjectSettingsSession> escaped;
        {
            EditorWorkspaceController controller{project.root, runtime, project.registry.Snapshot(), Services(project)};
            auto opened = controller.CollisionSettings();
            REQUIRE(opened.HasValue());
            escaped = opened.Value();
        }
        CHECK(escaped->Snapshot().HasError());
        CHECK(escaped->Preview(Input()).HasError());
    }
}  // namespace Horo::Editor
