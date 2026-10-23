#include "GameplayModuleTestSupport.h"
#include "GameplayRetirementTestSupport.h"
#include "GameplayRuntimeTestSupport.h"
#include "Horo/Application/ProjectSourceDocument.h"
#include "Horo/Gameplay/BehaviorRuntime.h"
#include "Horo/Gameplay/GameModuleRetirement.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "ProjectReplicationTestSupport.h"
#include "editor/gameplay/ProjectGameplayRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
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
            Tests::WriteReplicationProject(root);
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

    void RequireValidRegistry(const Editor::ProjectGameplayRegistry &registry) {
        for (const auto &diagnostic : registry.Diagnostics())
            UNSCOPED_INFO(diagnostic.source.string() << ": " << diagnostic.error.domain.Value() << ":" << diagnostic.error.code.Value()
                                                     << ": " << diagnostic.error.message);
        REQUIRE_FALSE(registry.HasBlockingDiagnostics());
    }

    Network::ReplicationSchemaDescriptor ScriptSchema() {
        using namespace Network;
        return {.id = ReplicationSchemaId::Create(42).Value(),
                .version = {1, 0},
                .compatibility = {{1, 0}, {1, 0}},
                .owner = ModuleId{"game.script"},
                .fields = {{.id = FieldId::Create(7).Value(),
                            .valueType = ReplicationValueTypeId::Create(1).Value(),
                            .codec = ReplicationCodecId::Create(2).Value(),
                            .introducedVersion = {1, 0},
                            .limits = {8, 1}}}};
    }

    std::filesystem::path WriteReplicatedScript(const TemporaryProject &project) {
        const auto source = project.root / "assets/scripts/Replicated.horo_script";
        Write(source, R"(return horo.behavior { display_name="Replicated", replication={major=1, minor=0,
            minimum_minor=0, maximum_minor=0, capture_phase="gameplay", apply_phase="pre_physics",
            fields={{id=7, value_type=1, codec=2, kind="number", introduced_major=1, introduced_minor=0,
                     condition="always", requirement="required", maximum_bytes=8, maximum_elements=1}}, tombstones={}} })");
        Write(source.string() + ".meta", R"({"schemaVersion":1,"runtime":"lua","behaviorTypeId":"game.script.replicated",
            "replicationSchemaId":42,"replicationModuleId":"game.script"})");
        return source;
    }
}  // namespace

TEST_CASE("project gameplay registry discovers and safely reloads compatible Lua source") {
    TemporaryProject project;
    const std::filesystem::path source = project.root / "assets" / "scripts" / "Watched.horo_script";
    Write(source, Source(1));
    Write(source.string() + ".meta", R"({"schemaVersion":1,"runtime":"lua","behaviorTypeId":"game.tests.watched"})");

    auto registry = Editor::ProjectGameplayRegistry::Discover(project.root);
    RequireValidRegistry(*registry);
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
    RequireValidRegistry(*registry);
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
    RequireValidRegistry(*registry);
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
    RequireValidRegistry(*registry);

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

TEST_CASE("Editor discovery reserves application rollback ownership before native factories", "[editor][gameplay][retirement]") {
    TemporaryProject project;
    WriteNativeManifest(project);
    Horo::Tests::NativeGameplayFailureControl native{2};
    auto queue = std::make_shared<Gameplay::GameModuleRetirementQueue>(1);
    auto reserved = queue->Reserve();
    REQUIRE(reserved.HasValue());
    Gameplay::GameModuleHost loader;
    auto loaded = loader.Load(HORO_TEST_GAME_MODULE_PATH, Horo::Tests::NativeRetirementExpectation());
    REQUIRE(loaded.HasValue());
    auto candidate = std::move(loaded).Value();
    REQUIRE(candidate.diagnostic);
    REQUIRE(native.callback);
    auto admission = std::move(reserved).Value();
    admission.Retain(std::move(candidate.owner));
    auto blocked = Editor::ProjectGameplayRegistry::Discover(project.root, queue);
    REQUIRE(blocked->HasBlockingDiagnostics());
    CHECK_FALSE(blocked->HasNativeModule());
    CHECK(native.control.created == 1);
    blocked.reset();
    CHECK(queue->Close().HasError());
    std::async(std::launch::async, [callback = std::move(native.callback)]() mutable {
        callback.reset();
    }).get();
    REQUIRE(queue->Close().HasValue());
    native.configure(nullptr);
    auto healthy = Editor::ProjectGameplayRegistry::Discover(project.root, queue);
    REQUIRE_FALSE(healthy->HasBlockingDiagnostics());
    REQUIRE(healthy->HasNativeModule());
    healthy.reset();
    REQUIRE(queue->Close().HasValue());
}

TEST_CASE("actual native and Lua discovery publishes deep inert replication metadata", "[editor][replication]") {
    TemporaryProject project;
    WriteNativeManifest(project);
    WriteReplicatedScript(project);
    Network::NetworkReplicationInventory inventory{Network::NetworkReplicationInventoryCompleteness::Complete, {{ScriptSchema()}}};
    Tests::WriteReplicationProject(project.root, inventory);
    auto registry = Editor::ProjectGameplayRegistry::Discover(project.root);
    RequireValidRegistry(*registry);
    const auto metadata = registry->ReplicationMetadata();
    REQUIRE(metadata);
    REQUIRE(metadata->Registered()->Schemas().size() == 2);
    CHECK(metadata->Registered()->Find(Network::ReplicationSchemaId::Create(1001).Value()).HasValue());
    CHECK(metadata->Registered()->Find(Network::ReplicationSchemaId::Create(42).Value()).HasValue());
    REQUIRE(metadata->Modules().size() == 2);
    CHECK(metadata->Modules()[0].value == "game.script");
    CHECK(metadata->Modules()[1].value == "game.tests");
    REQUIRE(registry->ReplicationInspection());
    CHECK_FALSE(registry->ReplicationInspection()->Admitted());
    CHECK(registry->ReplicationInspection()->Diagnostics().empty());
    CHECK(metadata->Assess(Network::ReplicationDeclarationUse::Cook).Value().Admitted());
    registry.reset();
    CHECK(metadata->Registered()->Schemas().size() == 2);
    CHECK(metadata->Inventory()->declarations.front().schema == ScriptSchema());
}

TEST_CASE("editor inspection retains opaque missing code without inventing loaded module identities", "[editor][replication]") {
    TemporaryProject project;
    Network::NetworkReplicationInventory inventory{Network::NetworkReplicationInventoryCompleteness::Complete, {{ScriptSchema()}}};
    Tests::WriteReplicationProject(project.root, inventory);
    auto required = Editor::ProjectGameplayRegistry::Discover(project.root);
    REQUIRE(required->HasBlockingDiagnostics());
    REQUIRE(required->ReplicationInspection());
    REQUIRE(required->ReplicationInspection()->OpaqueDeclarations().size() == 1);
    CHECK(required->ReplicationMetadata()->Modules().empty());
    CHECK(required->ReplicationInspection()->Diagnostics().front().problem == Network::ReplicationDeclarationProblem::MissingModule);
    inventory.declarations.front().requirement = Network::ReplicationDeclarationRequirement::Optional;
    Tests::WriteReplicationProject(project.root, inventory);
    auto optional = Editor::ProjectGameplayRegistry::Discover(project.root);
    REQUIRE_FALSE(optional->HasBlockingDiagnostics());
    CHECK_FALSE(optional->ReplicationInspection()->Admitted());
    CHECK(optional->ReplicationMetadata()->Assess(Network::ReplicationDeclarationUse::Cook).Value().Admitted());
}

TEST_CASE("editor source watch retains last good replication metadata on invalid policy and recovers once", "[editor][replication]") {
    TemporaryProject project;
    WriteReplicatedScript(project);
    auto registry = Editor::ProjectGameplayRegistry::Discover(project.root);
    RequireValidRegistry(*registry);
    const auto previous = registry->ReplicationMetadata();
    REQUIRE(previous);
    Write(project.root / ".horo/project.json", "{invalid}");
    REQUIRE_FALSE(registry->ReloadChangedLuaSources().empty());
    REQUIRE(registry->ReplicationMetadataError());
    CHECK(registry->HasBlockingDiagnostics());
    CHECK(registry->ReplicationMetadata() == previous);
    REQUIRE(registry->ReplicationInspection());
    Network::NetworkReplicationInventory complete{Network::NetworkReplicationInventoryCompleteness::Complete, {{ScriptSchema()}}};
    Tests::WriteReplicationProject(project.root, complete);
    REQUIRE(registry->ReloadChangedLuaSources().empty());
    CHECK_FALSE(registry->HasBlockingDiagnostics());
    CHECK_FALSE(registry->ReplicationMetadataError());
    const auto recovered = registry->ReplicationMetadata();
    CHECK(recovered->Fingerprint() != previous->Fingerprint());
    REQUIRE(registry->ReloadChangedLuaSources().empty());
    CHECK(registry->ReplicationMetadata() == recovered);
}

TEST_CASE("actual duplicate native Lua schema generation is rejected without partial metadata", "[editor][replication]") {
    TemporaryProject project;
    WriteNativeManifest(project);
    const auto source = WriteReplicatedScript(project);
    Write(source.string() + ".meta", R"({"schemaVersion":1,"runtime":"lua","behaviorTypeId":"game.script.replicated",
        "replicationSchemaId":1001,"replicationModuleId":"game.script"})");
    auto rejected = Editor::ProjectGameplayRegistry::Discover(project.root);
    REQUIRE(rejected->HasBlockingDiagnostics());
    CHECK_FALSE(rejected->ReplicationMetadata());
    REQUIRE(rejected->ReplicationMetadataError());
    CHECK(rejected->ReplicationMetadataError()->code.Value() == Network::NetworkErrors::ReplicationDescriptorConflict.code.Value());
}

TEST_CASE("Lua replication replacement validates authored inventory before replacing live generation", "[editor][replication]") {
    TemporaryProject project;
    const auto source = WriteReplicatedScript(project);
    Tests::WriteReplicationProject(project.root,
                                   Network::NetworkReplicationInventory{Network::NetworkReplicationInventoryCompleteness::Complete,
                                                                        {{ScriptSchema()}}});
    auto registry = Editor::ProjectGameplayRegistry::Discover(project.root);
    RequireValidRegistry(*registry);
    const auto previous = registry->ReplicationMetadata();
    std::ifstream input{source};
    const std::string original{std::istreambuf_iterator<char>{input}, {}};
    auto changed = original;
    changed.replace(changed.find("maximum_bytes=8"), std::string_view{"maximum_bytes=8"}.size(), "maximum_bytes=16");
    Write(source, changed);
    REQUIRE_FALSE(registry->ReloadChangedLuaSources().empty());
    CHECK(registry->ReplicationMetadata() == previous);
    CHECK(registry->Registry().Find(Gameplay::BehaviorTypeId::Parse("game.script.replicated").Value()) != nullptr);
    Write(source, original + " ");
    REQUIRE(registry->ReloadChangedLuaSources().empty());
    CHECK(registry->ReplicationMetadata()->Fingerprint() == previous->Fingerprint());
}

TEST_CASE("actual Lua declaration rejects a foreign module namespace with the original typed failure", "[editor][replication]") {
    TemporaryProject project;
    const auto source = WriteReplicatedScript(project);
    Write(source.string() + ".meta", R"({"schemaVersion":1,"runtime":"lua","behaviorTypeId":"game.tests.replicated",
        "replicationSchemaId":42,"replicationModuleId":"game.script"})");
    const auto loaded = Gameplay::LuaBehaviorProgram::LoadFiles(source, source.string() + ".meta");
    REQUIRE(loaded.HasError());
    UNSCOPED_INFO(loaded.ErrorValue().domain.Value() << ":" << loaded.ErrorValue().code.Value() << ": " << loaded.ErrorValue().message);
    CHECK(loaded.ErrorValue().domain.Value() == Gameplay::GameplayErrors::InvalidReplicationRegistration.domain.Value());
    CHECK(loaded.ErrorValue().code.Value() == Gameplay::GameplayErrors::InvalidReplicationRegistration.code.Value());
    auto registry = Editor::ProjectGameplayRegistry::Discover(project.root);
    REQUIRE(registry->HasBlockingDiagnostics());
    REQUIRE_FALSE(registry->Diagnostics().empty());
    CHECK(registry->Diagnostics().front().error.domain.Value() == loaded.ErrorValue().domain.Value());
    CHECK(registry->Diagnostics().front().error.code.Value() == loaded.ErrorValue().code.Value());
}

TEST_CASE("editor replication capture preserves missing canonical project source failure", "[editor][replication]") {
    TemporaryProject project;
    REQUIRE(std::filesystem::remove(project.root / ".horo/project.json"));
    const auto expected = Application::LoadProjectSourceDocument(project.root);
    REQUIRE(expected.HasError());
    auto registry = Editor::ProjectGameplayRegistry::Discover(project.root);
    REQUIRE(registry->HasBlockingDiagnostics());
    REQUIRE(registry->ReplicationMetadataError());
    CHECK(registry->ReplicationMetadataError()->domain.Value() == expected.ErrorValue().domain.Value());
    CHECK(registry->ReplicationMetadataError()->code.Value() == expected.ErrorValue().code.Value());
    CHECK_FALSE(registry->ReplicationMetadata());
}
