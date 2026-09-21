#include "GameplayModuleTestSupport.h"
#include "GameplayRuntimeTestSupport.h"
#include "Horo/Gameplay/BehaviorRuntime.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "editor/gameplay/ProjectGameplayRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>

namespace {
    using namespace Horo;

    struct TemporaryProject {
        std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            ("horo-gameplay-registry-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        TemporaryProject() {
            std::filesystem::create_directories(root / "assets" / "scripts");
        }

        ~TemporaryProject() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    };

    void Write(const std::filesystem::path &path, const std::string &value) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(stream.good());
        stream << value;
        stream.close();
        REQUIRE(stream.good());
    }

    std::string Source(const int amount) {
        return "return horo.behavior { type_id='game.tests.watched', display_name='Watched', "
               "on_fixed_update=function(ctx, dt) local x,y,z=ctx.transform.position(); "
               "ctx.transform.set_position(x+" +
               std::to_string(amount) + ",y,z) end }";
    }

    Runtime::RuntimeSceneDefinition Definition(const Runtime::SceneDefinitionId id, const std::string_view behaviorType) {
        return Tests::SingleBehaviorSceneDefinition(id, Runtime::SceneDefinitionRevision{1}, Runtime::SceneObjectId{1},
                                                    Gameplay::BehaviorInstanceId{1}, Gameplay::BehaviorTypeId::Parse(behaviorType).Value());
    }

    void WriteNativeManifest(const TemporaryProject &project) {
        Tests::WriteGameplayModuleManifest(project.root, HORO_TEST_GAME_MODULE_PATH, "game.tests",
                                           Gameplay::CurrentGameplayBuildFingerprint(),
                                           Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH));
    }

    std::filesystem::path PrepareNativeLuaProject(const TemporaryProject &project) {
        const std::filesystem::path source = project.root / "assets" / "scripts" / "Watched.horo_script";
        Write(source, Source(1));
        Write(source.string() + ".meta", R"({"schemaVersion":1,"runtime":"lua","behaviorTypeId":"game.tests.watched"})");
        WriteNativeManifest(project);
        return source;
    }
}  // namespace

TEST_CASE("project gameplay registry discovers and safely reloads compatible Lua source") {
    TemporaryProject project;
    const std::filesystem::path source = project.root / "assets" / "scripts" / "Watched.horo_script";
    Write(source, Source(1));
    Write(source.string() + ".meta", R"({"schemaVersion":1,"runtime":"lua","behaviorTypeId":"game.tests.watched"})");

    auto registry = Editor::ProjectGameplayRegistry::Discover(project.root);
    REQUIRE_FALSE(registry->HasBlockingDiagnostics());
    Tests::ActiveBehaviorRuntime active = Tests::ActivateBehaviorRuntime(Definition(Runtime::SceneDefinitionId{1}, "game.tests.watched"),
                                                                         Runtime::SceneRuntimeId{1}, registry->Registry());
    REQUIRE(Tests::FixedUpdateAndReadPosition(*active.scene, *active.runtime, Runtime::SceneObjectId{1}).x == 1.0F);

    Write(source, Source(3));
    std::error_code timeError;
    const auto newer = std::filesystem::last_write_time(source, timeError) + std::chrono::seconds(2);
    REQUIRE_FALSE(timeError);
    std::filesystem::last_write_time(source, newer, timeError);
    REQUIRE_FALSE(timeError);
    REQUIRE(registry->ReloadChangedLuaSources().empty());
    REQUIRE(Tests::FixedUpdateAndReadPosition(*active.scene, *active.runtime, Runtime::SceneObjectId{1}).x == 4.0F);
}

TEST_CASE("project gameplay registry merges a fingerprinted native module with Lua behaviors") {
    TemporaryProject project;
    PrepareNativeLuaProject(project);

    auto registry = Editor::ProjectGameplayRegistry::Discover(project.root);
    REQUIRE_FALSE(registry->HasBlockingDiagnostics());
    REQUIRE(registry->Registry().Find(Gameplay::BehaviorTypeId::Parse("game.tests.dynamic_mover").Value()) != nullptr);
    REQUIRE(registry->Registry().Find(Gameplay::BehaviorTypeId::Parse("game.tests.watched").Value()) != nullptr);
    REQUIRE(registry->Components().Find(Gameplay::ComponentTypeId::Parse("game.tests.movement_settings").Value()) != nullptr);
    REQUIRE(registry->Components().IsFrozen());
    REQUIRE(registry->AssetTypes().Find(Gameplay::GameAssetTypeId::Parse("game.tests.quest_definition").Value()) != nullptr);
    REQUIRE_FALSE(registry->ConsumeNativeArtifactChange());
    const std::filesystem::path manifestPath = project.root / ".horo" / "local" / "gameplay_module.json";
    std::error_code timeError;
    std::filesystem::last_write_time(manifestPath, std::filesystem::last_write_time(manifestPath, timeError) + std::chrono::seconds(2),
                                     timeError);
    REQUIRE_FALSE(timeError);
    REQUIRE(registry->ConsumeNativeArtifactChange());
    REQUIRE_FALSE(registry->ConsumeNativeArtifactChange());

    Tests::ActiveBehaviorRuntime active =
        Tests::ActivateBehaviorRuntime(Definition(Runtime::SceneDefinitionId{2}, "game.tests.dynamic_mover"), Runtime::SceneRuntimeId{2},
                                       registry->Registry());
    const auto blocked = registry->PrepareNativeReload();
    REQUIRE(blocked.HasError());
    CHECK(blocked.ErrorValue().code.Value() == Gameplay::GameplayErrors::GameplayReloadRestartRequired.code.Value());
    const auto rejected = Gameplay::BehaviorRuntime::Create(*active.scene, registry->Registry());
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == Gameplay::GameplayErrors::GameplayReloadRestartRequired.code.Value());
    active.runtime.reset();
    CHECK(registry->PrepareNativeReload().HasValue());
}

TEST_CASE("project gameplay registry reports native sources without a published successful artifact") {
    TemporaryProject project;
    Write(project.root / "source" / "gameplay" / "Player.cpp", "// native behavior source\n");
    auto registry = Editor::ProjectGameplayRegistry::Discover(project.root);
    REQUIRE(registry->HasBlockingDiagnostics());
    REQUIRE(registry->Diagnostics().front().source == project.root / ".horo" / "local" / "gameplay_module.json");
    REQUIRE(registry->AssetTypes().IsFrozen());
    REQUIRE(registry->AssetTypes().Registrations().empty());
    REQUIRE(registry->Components().IsFrozen());
    REQUIRE(registry->Components().Descriptors().empty());
}

TEST_CASE("project gameplay registry preserves and restores an unloaded native generation") {
    TemporaryProject project;
    const std::filesystem::path source = PrepareNativeLuaProject(project);

    auto registry = Editor::ProjectGameplayRegistry::Discover(project.root);
    REQUIRE_FALSE(registry->HasBlockingDiagnostics());
    auto preserved = registry->PreserveNativeArtifactForRollback(project.root / ".horo" / "local" / "rollback");
    REQUIRE(preserved.HasValue());
    REQUIRE(std::filesystem::is_regular_file(preserved.Value().path));
    auto luaGeneration = registry->CaptureLuaGeneration();
    REQUIRE(luaGeneration.HasValue());
    auto snapshot = registry->PrepareNativeReload();
    REQUIRE(snapshot.HasValue());
    registry.reset();
    Write(source, Source(9));

    auto rollback = Editor::ProjectGameplayRegistry::DiscoverRollback(project.root, preserved.Value(), luaGeneration.Value());
    REQUIRE_FALSE(rollback->HasBlockingDiagnostics());
    REQUIRE(rollback->HasNativeModule());
    REQUIRE(rollback->RestoreNativeReload(snapshot.Value()).HasValue());
    Tests::ActiveBehaviorRuntime active = Tests::ActivateBehaviorRuntime(Definition(Runtime::SceneDefinitionId{1}, "game.tests.watched"),
                                                                         Runtime::SceneRuntimeId{19}, rollback->Registry());
    CHECK(Tests::FixedUpdateAndReadPosition(*active.scene, *active.runtime, Runtime::SceneObjectId{1}).x == 1.0F);
}

TEST_CASE("native gameplay rollback artifact cleanup ownership follows moves") {
    TemporaryProject project;
    WriteNativeManifest(project);
    auto registry = Editor::ProjectGameplayRegistry::Discover(project.root);
    REQUIRE_FALSE(registry->HasBlockingDiagnostics());

    std::filesystem::path artifactPath;
    {
        auto preserved = registry->PreserveNativeArtifactForRollback(project.root / ".horo" / "local" / "rollback");
        REQUIRE(preserved.HasValue());
        artifactPath = preserved.Value().path;
        Editor::NativeGameplayRollbackArtifact firstOwner = std::move(preserved).Value();
        Editor::NativeGameplayRollbackArtifact finalOwner;
        finalOwner = std::move(firstOwner);
        CHECK(std::filesystem::is_regular_file(artifactPath));
    }
    CHECK_FALSE(std::filesystem::exists(artifactPath));
}
