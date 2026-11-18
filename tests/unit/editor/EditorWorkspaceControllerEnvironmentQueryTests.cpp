#include "Horo/AI/EnvironmentQuerySourceCodec.h"
#include "OwnedTestDirectory.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/WorkspacePathText.h"

#include <catch2/catch_test_macros.hpp>
#include <format>
#include <fstream>

namespace {
    using namespace Horo;
    using namespace Horo::AI;
    using namespace Horo::Editor;

    /** @brief Installed registered query source for the normal content-browser command route. */
    struct Project final {
        static constexpr std::string_view Path = "assets/cover.horo_eqs";
        static constexpr std::string_view Copy = "assets/cover (1).horo_eqs";
        const Assets::AssetId asset = Assets::AssetId::FromBytes({7});
        Tests::OwnedTestDirectory directory{"editor-eqs"};
        std::filesystem::path root = std::filesystem::canonical(directory.Path());
        Runtime::RuntimeSceneService runtime;
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations{files};
        Assets::AssetRegistry registry;

        Project() {
            REQUIRE(std::filesystem::create_directories(root / "assets"));
            WriteQuery("Saved query");
            {
                std::ofstream sidecar(root / (std::string{Path} + ".horo"), std::ios::binary);
                sidecar << std::format(R"({{"schemaVersion":1,"assetId":"{}","assetType":"{}"}})", asset.ToString(),
                                       EnvironmentQuerySourceAssetType);
                REQUIRE(static_cast<bool>(sidecar));
            }
            const auto rebuilt = Assets::RebuildAssetRegistry(registry, root, Assets::AssetRegistryOpenMode::ReadOnly);
            REQUIRE(rebuilt.HasValue());
            REQUIRE(rebuilt.Value().status == Assets::AssetRegistryBuildStatus::Complete);
        }

        void WriteQuery(std::string name) const {
            const auto query = EnvironmentQueryAsset::Capture({
                .id = QueryId::Create(2).Value(),
                .displayName = std::move(name),
                .result = {.id = QueryResultSchemaId::Create(3).Value(), .itemType = QueryItemTypeId::Create(4).Value()},
                .stages = {{.id = QueryStageId::Create(5).Value(),
                            .kind = QueryStageKind::Unknown,
                            .displayName = "Contributed stage",
                            .descriptorVersion = {2, 3},
                            .unknownTypeId = 13,
                            .opaquePayload = {std::byte{0x27}}}},
            });
            REQUIRE(query.HasValue());
            const auto encoded = EnvironmentQuerySourceCodec::Encode(query.Value());
            REQUIRE(encoded.HasValue());
            std::ofstream source(root / Path, std::ios::binary | std::ios::trunc);
            source.write(reinterpret_cast<const char *>(encoded.Value().data()), static_cast<std::streamsize>(encoded.Value().size()));
            REQUIRE(static_cast<bool>(source));
        }

        static void Open(EditorWorkspaceController &controller, std::string_view path) {
            controller.ProcessCommand(
                {.command = EditorWorkspaceViewCommand::OpenSourceFile,
                 .sourceOpenRequest = SourceOpenRequest{.path = std::filesystem::path{path}, .origin = SourceOpenOrigin::AssetActivation}});
        }

        void Duplicate(EditorWorkspaceController &controller) const {
            controller.ProcessCommand(
                {.command = EditorWorkspaceViewCommand::DuplicateContentBrowserAsset, .stringPayload = WorkspacePathText(root / Path)});
        }
    };
}  // namespace

TEST_CASE("Environment Query sources open saved typed tabs and duplicate through the authored asset publisher",
          "[unit][editor][eqs][workspace]") {
    Project project;
    EditorWorkspaceController controller(project.root, project.runtime, project.registry.Snapshot(),
                                         {.assets = {.mutableAssetRegistry = &project.registry},
                                          .mutations = &project.mutations,
                                          .durableFiles = &project.files});
    Project::Open(controller, Project::Path);
    const auto &documents = controller.ViewModel().environmentQueryDocuments;
    REQUIRE(documents.size() == 1);
    const auto instance = documents.front().identity.instance;
    CHECK(documents.front().identity.key.kind == DocumentKind::EnvironmentQuery);
    CHECK(ParseDocumentKind("environment_query").Value() == DocumentKind::EnvironmentQuery);
    CHECK(documents.front().asset == project.asset);
    CHECK(documents.front().query->DisplayName() == "Saved query");
    CHECK(controller.ViewModel().workspacePanelHost.ActiveDocument() == instance);
    CHECK_FALSE(controller.HasDirtySourceDocuments());

    SECTION("reopening the path refreshes a complete immutable source and closes normally") {
        const auto first = documents.front().query;
        project.WriteQuery("Changed on disk");
        Project::Open(controller, Project::Path);
        REQUIRE(documents.size() == 1);
        CHECK(documents.front().identity.instance == instance);
        CHECK(documents.front().query->DisplayName() == "Changed on disk");
        CHECK(first->DisplayName() == "Saved query");
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = instance});
        CHECK(documents.empty());
        CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().empty());
    }
    SECTION("renaming the paired asset preserves its semantic identity and replaces its old path tab") {
        const auto semanticId = documents.front().query->Id();
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::RenameContentBrowserEntry,
                                   .stringPayload = WorkspacePathText(project.root / Project::Path),
                                   .secondaryStringPayload = "relocated.horo_eqs"});
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        Project::Open(controller, "assets/relocated.horo_eqs");
        REQUIRE(documents.size() == 1);
        REQUIRE(controller.ViewModel().workspacePanelHost.DocumentTabs().size() == 1);
        CHECK(documents.front().identity.key.source.Value() == "assets/relocated.horo_eqs");
        CHECK(documents.front().asset == project.asset);
        CHECK(documents.front().query->Id() == semanticId);
    }
    SECTION("duplicate reserves independent durable and semantic identities and preserves unknown stage bytes") {
        project.Duplicate(controller);
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        const auto registry = project.registry.Snapshot();
        const auto *record = registry.FindByPath(Project::Copy);
        REQUIRE(record);
        CHECK(record->id != project.asset);
        REQUIRE(std::filesystem::is_regular_file(project.root / (std::string{Project::Copy} + ".horo")));
        Project::Open(controller, Project::Copy);
        REQUIRE(documents.size() == 2);
        const auto &original = *documents.front().query;
        const auto &copied = *documents.back().query;
        CHECK(copied.Id() != original.Id());
        CHECK(copied.ResultSchema() == original.ResultSchema());
        REQUIRE(copied.Stages().size() == 1);
        CHECK(copied.Stages().front().id == original.Stages().front().id);
        CHECK(copied.Stages().front().opaquePayload == original.Stages().front().opaquePayload);
        CHECK(copied.Stages().front().descriptorVersion == original.Stages().front().descriptorVersion);
    }
    SECTION("invalid current query source never publishes a copied pair") {
        {
            std::ofstream broken(project.root / Project::Path, std::ios::binary | std::ios::trunc);
            broken << "broken";
            REQUIRE(static_cast<bool>(broken));
        }
        Project::Open(controller, Project::Path);
        CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.environment_query.invalid");
        CHECK(documents.front().query->DisplayName() == "Saved query");
        project.Duplicate(controller);
        CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.environment_query.copy_failed");
        CHECK(project.registry.Snapshot().FindByPath(Project::Copy) == nullptr);
        CHECK_FALSE(std::filesystem::exists(project.root / Project::Copy));
        CHECK_FALSE(std::filesystem::exists(project.root / (std::string{Project::Copy} + ".horo")));
    }
}
