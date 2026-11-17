#include "GameplayModuleTestSupport.h"
#include "Horo/Gameplay/GameModule.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "ProjectReplicationTestSupport.h"
#include "SceneDocumentPersistenceTestSupport.h"
#include "ScriptModuleRuntimeFixture.h"
#include "editor/gameplay/EditorPlaySessionController.h"
#include "editor/gameplay/ProjectGameplayRegistry.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace Horo::Editor::Tests {
    using namespace Extensions::Tests::ModuleSupport;

    namespace {
        void Write(const std::filesystem::path &path, std::string_view value) {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream stream{path, std::ios::binary};
            REQUIRE(stream);
            stream.write(value.data(), static_cast<std::streamsize>(value.size()));
            REQUIRE(stream);
        }

        std::string Read(const std::filesystem::path &path) {
            std::ifstream stream{path, std::ios::binary};
            REQUIRE(stream);
            return {std::istreambuf_iterator<char>{stream}, {}};
        }

        void PrepareProject(const PersistenceTestSupport::TemporaryProject &project) {
            project.PrepareEmptyScene();
            Horo::Tests::WriteReplicationProject(project.Root());
            const auto path = project.Root() / ".horo/project.json";
            auto metadata = nlohmann::json::parse(Read(path));
            metadata["settings"]["defaultScene"] = "assets/scenes/main.horo";
            Write(path, metadata.dump());
        }

        Gameplay::BehaviorComponent Payload() {
            return {.instanceId = {71},
                    .typeId = Take(Gameplay::BehaviorTypeId::Parse("game.recovery.actor")),
                    .schemaVersion = 1,
                    .enabled = false,
                    .fields = {{"speed", 3.25}}};
        }

        SceneDocumentSnapshot Authored() {
            SceneObjectComponentSet components;
            components.camera = Runtime::CameraComponent{};
            components.behaviors = {Payload()};
            return {DocumentRevision{7},
                    DocumentStateId{11},
                    {{SceneObjectId{1}, std::nullopt, "Authored", {}, std::nullopt, std::move(components)}}};
        }

        void Save(const PersistenceTestSupport::TemporaryProject &project, const SceneDocumentSnapshot &snapshot) {
            NativeDurableFileSystem files;
            ProjectMutationCoordinator mutations{files};
            auto expected = Take(InspectProjectSceneFingerprint(project.Root(), project.ScenePath()));
            const auto saved = Take(SaveProjectScene(project.Root(), project.ScenePath(), snapshot, expected, false, mutations, files));
            CHECK(saved.status == ProjectSceneSaveStatus::Saved);
        }

        SceneDocumentSnapshot Reopen(const PersistenceTestSupport::TemporaryProject &project) {
            auto loaded = Take(LoadProjectDefaultScene(project.Root()));
            REQUIRE(loaded);
            SceneDocument document;
            Accept(document.LoadSaved(std::move(loaded->objects), std::move(loaded->prefabInstances)));
            REQUIRE(document.Objects().size() == 1);
            CHECK(document.Objects().front().components.behaviors == std::vector{Payload()});
            EditorHistory history;
            SceneDocumentCommandExecutor commands{document, history};
            Take(commands.Execute(RenameSceneObjectCommand{SceneObjectId{1}, "Inspectable with missing provider"}));
            return document.Snapshot();
        }

        void RequiredImportDiagnostic() {
            Extensions::ExtensionManifest declaration;
            declaration.id = "horo.reference.package";
            declaration.modules = {Consumer(true)};
            auto missing = ResolveExtensionModules(declaration, Environment());
            REQUIRE(missing.HasError());
            CHECK(missing.ErrorValue().code.Value() == Extensions::ExtensionErrors::ModuleResolutionFailed.code.Value());
            CHECK(missing.ErrorValue().message.find("horo.script.reference") != std::string::npos);
        }
    }  // namespace

    TEST_CASE("Real project Scene payload and script source remain inspectable saveable and recoverable with a missing required module",
              "[Editor][ScriptModule][Recovery][Persistence]") {
        PersistenceTestSupport::TemporaryProject project;
        PrepareProject(project);
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations{files};
        Accept(SetProjectDefaultScenePath(project.Root(), project.ScenePath(), mutations, files));
        const auto source = project.Root() / "assets/scripts/Actor.horo_script";
        const std::string code =
            R"(return horo.behavior { type_id="game.recovery.actor", display_name="Actor", schema_version=1, fields={{name="speed",default=1.0}} })";
        const std::string metadata = R"({"schemaVersion":1,"runtime":"lua","behaviorTypeId":"game.recovery.actor"})";
        Write(source, code);
        Write(source.string() + ".meta", metadata);
        Write(project.Root() / "source/gameplay/Actor.cpp", "// actual native source without a published module artifact\n");
        Save(project, Authored());
        RequiredImportDiagnostic();
        auto unavailable = ProjectGameplayRegistry::Discover(project.Root());
        REQUIRE(unavailable->HasBlockingDiagnostics());
        REQUIRE_FALSE(unavailable->Diagnostics().empty());
        CHECK(unavailable->Diagnostics().front().error.code.Value() == Gameplay::GameplayErrors::InvalidBehaviorComponent.code.Value());
        CHECK(unavailable->Diagnostics().front().source == project.Root() / ".horo/local/gameplay_module.json");
        const auto editable = Reopen(project);
        Save(project, editable);
        CHECK(Read(source) == code);
        CHECK(Read(source.string() + ".meta") == metadata);
        unavailable.reset();
        Horo::Tests::WriteGameplayModuleManifest(project.Root(), HORO_TEST_GAME_MODULE_PATH, "game.tests",
                                                 Gameplay::CurrentGameplayBuildFingerprint(),
                                                 Horo::Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH));
        auto recovered = ProjectGameplayRegistry::Discover(project.Root());
        for (const auto &diagnostic : recovered->Diagnostics())
            UNSCOPED_INFO(diagnostic.error.code.Value() << ": " << diagnostic.error.message);
        REQUIRE_FALSE(recovered->HasBlockingDiagnostics());
        const auto reopened = Reopen(project);
        CHECK(reopened.objects.front().components.behaviors == std::vector{Payload()});
        EditorPlaySessionController play;
        Accept(play.Start(reopened, recovered->Registry()));
        CHECK(play.State() == EditorPlaySessionState::Playing);
        play.Stop();
        CHECK(Read(source) == code);
        CHECK(Read(source.string() + ".meta") == metadata);
    }
}  // namespace Horo::Editor::Tests
