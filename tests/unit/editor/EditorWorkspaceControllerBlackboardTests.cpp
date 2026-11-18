#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Editor/ProjectMutation.h"
#include "editor/screens/workspace/BlackboardScalarInput.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    /** @brief Registered source and native publication for the actual workspace admission path. */
    struct BlackboardWorkspaceFixture final {
        static constexpr auto Path = "guard.horo_bb";
        const Assets::AssetId asset = Assets::AssetId::FromBytes({7});
        const AI::BlackboardKeyId key = AI::BlackboardKeyId::Create(4).Value();
        std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            ("horo-blackboard-editor-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Runtime::RuntimeSceneService runtime;
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations{files};
        Assets::AssetRegistry registry;

        BlackboardWorkspaceFixture() {
            std::filesystem::create_directories(root);
            root = std::filesystem::canonical(root);
            const auto type = Assets::AssetTypeId::Parse(AI::BlackboardSchemaAssetType).Value();
            REQUIRE(registry
                        .Publish(std::vector<Assets::AssetRecord>{
                            {asset, type, ProjectPath::Parse(Path).Value(), ProjectPath::Parse("guard.horo_bb.horo").Value()}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            Write("Initial");
        }

        ~BlackboardWorkspaceFixture() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        void Write(const char *label) const {
            const std::array keys{AI::BlackboardKeyDescriptor{.key = key}};
            const auto schema = AI::BlackboardSchema::Capture({.identity = AI::BlackboardSchemaId::Create(3).Value(), .keys = keys});
            REQUIRE(schema.HasValue());
            const std::array rows{AI::BlackboardSchemaAuthoringRow{key, label}};
            const auto encoded = AI::BlackboardSchemaSourceCodec::Encode(schema.Value(), rows);
            REQUIRE(encoded.HasValue());
            std::ofstream output(root / Path, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char *>(encoded.Value().data()), static_cast<std::streamsize>(encoded.Value().size()));
            REQUIRE(static_cast<bool>(output));
        }

        [[nodiscard]] static EditorWorkspaceViewCommandData Open() {
            return {.command = EditorWorkspaceViewCommand::OpenSourceFile,
                    .sourceOpenRequest = SourceOpenRequest{.path = Path, .origin = SourceOpenOrigin::AssetActivation}};
        }

        [[nodiscard]] static BlackboardDocumentFence Current(const EditorWorkspaceController &controller) {
            const auto &documents = controller.ViewModel().blackboardDocuments;
            REQUIRE(documents.size() == 1);
            const auto &snapshot = documents.front().snapshot;
            return {snapshot.document.instance, snapshot.revision};
        }

        void Rename(EditorWorkspaceController &controller, BlackboardDocumentFence fence, const char *name) const {
            controller.ProcessCommand({.command = EditorWorkspaceViewCommand::BlackboardDocument,
                                       .blackboard = BlackboardWorkspaceRequest{.fence = fence, .edit = RenameBlackboardKey{key, name}}});
        }

        [[nodiscard]] static const auto &Snapshot(const EditorWorkspaceController &controller) {
            const auto &documents = controller.ViewModel().blackboardDocuments;
            REQUIRE(documents.size() == 1);
            return documents.front().snapshot;
        }
    };
}  // namespace

TEST_CASE("Blackboard asset activation saves, reloads and reopens its real workspace tab", "[unit][editor][blackboard][workspace]") {
    BlackboardWorkspaceFixture project;
    EditorWorkspaceController controller(project.root, project.runtime, project.registry.Snapshot(),
                                         {.mutations = &project.mutations, .durableFiles = &project.files});
    controller.ProcessCommand(project.Open());
    const auto initial = project.Current(controller);
    CHECK(controller.ViewModel().workspacePanelHost.ActiveDocument() == initial.instance);
    REQUIRE(controller.ViewModel().workspacePanelHost.DocumentTabs().size() == 1);
    CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().front().identity.key.kind == DocumentKind::BlackboardSchema);

    project.Rename(controller, initial, "Saved");
    const auto edited = project.Current(controller);
    REQUIRE(controller.HasDirtySourceDocuments());
    controller.ProcessCommand(project.Open());
    CHECK(project.Current(controller).instance == initial.instance);
    CHECK(project.Snapshot(controller).source->rows.front().displayName == "Saved");
    controller.ProcessCommand({.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = initial.instance});
    REQUIRE(controller.HasDirtySourceDocuments());
    CHECK(controller.ResolveBlackboardClose(initial, SourceCloseDecision::Discard).HasError());
    REQUIRE(controller.SaveSourceDocuments().HasValue());
    CHECK_FALSE(controller.HasDirtySourceDocuments());

    project.Write("External");
    controller.ProcessCommand(
        {.command = EditorWorkspaceViewCommand::BlackboardDocument,
         .blackboard = BlackboardWorkspaceRequest{.fence = edited, .operation = BlackboardDocumentOperation::Reload}});
    CHECK(project.Snapshot(controller).source->rows.front().displayName == "External");
    CHECK(project.Current(controller).revision > edited.revision);

    project.Rename(controller, project.Current(controller), "Saved on close");
    const auto closed = controller.ResolveBlackboardClose(project.Current(controller), SourceCloseDecision::Save);
    REQUIRE(closed.HasValue());
    REQUIRE(closed.Value());
    CHECK(controller.ViewModel().blackboardDocuments.empty());
    CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().empty());

    controller.ProcessCommand(project.Open());
    CHECK(project.Current(controller).instance != initial.instance);
    CHECK(project.Snapshot(controller).source->rows.front().displayName == "Saved on close");
    CHECK_FALSE(controller.HasDirtySourceDocuments());
}

TEST_CASE("Blackboard workspace numeric defaults preserve exact typed input", "[unit][editor][blackboard][workspace]") {
    using enum AI::BlackboardValueKind;
    const auto integer = ReadBlackboardNumericDefault(SignedInteger, "-9223372036854775808");
    REQUIRE(integer);
    CHECK(std::get<std::int64_t>(*integer) == std::numeric_limits<std::int64_t>::min());
    const auto decimal = ReadBlackboardNumericDefault(Scalar, "0.12345678901234568");
    REQUIRE(decimal);
    const auto shown = BlackboardNumericDefaultText(AI::BlackboardValue{*decimal});
    CHECK(ReadBlackboardNumericDefault(Scalar, shown) == decimal);

    CHECK_FALSE(ReadBlackboardNumericDefault(SignedInteger, "9223372036854775808"));
    CHECK_FALSE(ReadBlackboardNumericDefault(SignedInteger, "1.0"));
    CHECK_FALSE(ReadBlackboardNumericDefault(Scalar, "15x"));
    CHECK_FALSE(ReadBlackboardNumericDefault(Scalar, "nan"));
    CHECK_FALSE(ReadBlackboardNumericDefault(Scalar, "1e999"));
    CHECK_FALSE(ReadBlackboardNumericDefault(Boolean, "1"));
}

TEST_CASE("Blackboard workspace cannot shrink a typed collection below its authored default", "[unit][editor][blackboard][workspace]") {
    BlackboardWorkspaceFixture project;
    EditorWorkspaceController controller(project.root, project.runtime, project.registry.Snapshot(),
                                         {.mutations = &project.mutations, .durableFiles = &project.files});
    controller.ProcessCommand(project.Open());
    const auto next = project.Snapshot(controller).nextKey;
    REQUIRE(next);
    const auto edit = [&](BlackboardSchemaEdit command) {
        controller.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::BlackboardDocument,
             .blackboard = BlackboardWorkspaceRequest{.fence = project.Current(controller), .edit = std::move(command)}});
    };
    AI::BlackboardCollectionValue values{.elementKind = AI::BlackboardValueKind::SignedInteger,
                                         .elements = {std::int64_t{3}, std::int64_t{-5}},
                                         .count = 2};
    AI::BlackboardKeyDescriptor key{.key = *next,
                                    .kind = values.elementKind,
                                    .cardinality = AI::BlackboardValueCardinality::Collection,
                                    .maximumCollectionElements = 4,
                                    .defaultValue = values};
    edit(AddBlackboardKey{BlackboardSchemaRow{key, "Draft collection"}, 1});
    const auto admitted = project.Snapshot(controller);
    REQUIRE(admitted.source->rows.size() == 2);

    key.maximumCollectionElements = 1;
    edit(EditBlackboardKey{key});
    CHECK(project.Snapshot(controller).source == admitted.source);
    CHECK(project.Snapshot(controller).revision == admitted.revision);
    REQUIRE(controller.ViewModel().blackboardDocuments.front().diagnostic);

    key.maximumCollectionElements = 2;
    edit(EditBlackboardKey{key});
    const auto updated = project.Snapshot(controller);
    REQUIRE(updated.revision > admitted.revision);
    CHECK(updated.source->rows.back().descriptor == key);
    REQUIRE(controller.SaveSourceDocuments().HasValue());
    const auto closed = controller.ResolveBlackboardClose(project.Current(controller), SourceCloseDecision::Save);
    REQUIRE(closed.HasValue());
    REQUIRE(closed.Value());
    controller.ProcessCommand(project.Open());
    CHECK(project.Snapshot(controller).source->rows.back().descriptor == key);
}
