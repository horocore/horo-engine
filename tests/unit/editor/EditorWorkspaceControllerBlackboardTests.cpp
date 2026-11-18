#include "DecisionGraphCompilerTestSupport.h"
#include "GameplayModuleTestSupport.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Application/BlackboardUsageReview.h"
#include "Horo/Application/DecisionAssetCookHost.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Gameplay/GameModule.h"
#include "ProjectReplicationTestSupport.h"
#include "editor/screens/workspace/BlackboardScalarInput.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/WorkspacePathText.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserInteractionSession.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    /** @brief Registered source and native publication for the actual workspace admission path. */
    struct BlackboardWorkspaceFixture final {
        static constexpr auto Path = "assets/guard.horo_bb";
        const Assets::AssetId asset = Assets::AssetId::FromBytes({7});
        const AI::BlackboardKeyId key = AI::BlackboardKeyId::Create(4).Value();
        const AI::BlackboardSchemaId schema = AI::BlackboardSchemaId::Create(3).Value();
        std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            ("horo-blackboard-editor-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Runtime::RuntimeSceneService runtime;
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations{files};
        Assets::AssetRegistry registry;

        explicit BlackboardWorkspaceFixture(bool existing = true) {
            std::filesystem::create_directories(root / "assets");
            root = std::filesystem::canonical(root);
            if (existing) {
                Write("Initial");
                WriteSidecar(Path, asset, AI::BlackboardSchemaAssetType);
            }
            RebuildRegistry();
            if (existing)
                REQUIRE(registry.Snapshot().Find(asset));
        }

        ~BlackboardWorkspaceFixture() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        void WriteSidecar(const std::string &path, const Assets::AssetId identity, std::string_view type) const {
            std::ofstream sidecar(root / (path + ".horo"), std::ios::binary);
            sidecar << std::format(R"({{"schemaVersion":1,"assetId":"{}","assetType":"{}"}})", identity.ToString(), type);
            REQUIRE(static_cast<bool>(sidecar));
        }

        void RebuildRegistry() {
            const auto rebuilt = Assets::RebuildAssetRegistry(registry, root, Assets::AssetRegistryOpenMode::ReadOnly);
            REQUIRE(rebuilt.HasValue());
            REQUIRE(rebuilt.Value().status == Assets::AssetRegistryBuildStatus::Complete);
        }

        void Write(const char *label) const {
            const std::array keys{AI::BlackboardKeyDescriptor{.key = key}};
            const auto captured = AI::BlackboardSchema::Capture({.identity = schema, .keys = keys});
            REQUIRE(captured.HasValue());
            const std::array rows{AI::BlackboardSchemaAuthoringRow{key, label}};
            const auto encoded = AI::BlackboardSchemaSourceCodec::Encode(captured.Value(), rows);
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

TEST_CASE("Blackboard New atomically creates and opens a schema with an authored initial key", "[unit][editor][blackboard][workspace]") {
    BlackboardWorkspaceFixture project{false};
    EditorWorkspaceController controller(project.root, project.runtime, project.registry.Snapshot(),
                                         {.assets = {.mutableAssetRegistry = &project.registry},
                                          .mutations = &project.mutations,
                                          .durableFiles = &project.files});
    REQUIRE(controller.ViewModel().blackboardCreationAvailable);
    AssetBrowserInteractionSession browser;
    browser.OpenCreateBlackboard(WorkspacePathText(project.root / "assets"));
    auto &draft = browser.State().createBlackboard;
    CHECK_FALSE(AssetBrowserInteractionSession::CanConfirmCreateBlackboard(draft));
    draft.request.name = "New guard";
    draft.request.keyName = "Patrol radius";
    draft.request.key.kind = AI::BlackboardValueKind::SignedInteger;
    draft.request.key.presence = AI::BlackboardKeyPresence::Required;
    draft.request.key.access = AI::BlackboardKeyAccess::ReadOnly;
    CHECK_FALSE(AssetBrowserInteractionSession::ConfirmCreateBlackboard(draft).has_value());
    draft.request.key.defaultValue = AI::BlackboardScalarValue{std::int64_t{42}};
    auto command = AssetBrowserInteractionSession::ConfirmCreateBlackboard(draft);
    REQUIRE(command.has_value());

    constexpr auto path = "assets/New guard.horo_bb";
    SECTION("normal creation retains assigned identity and complete typed data across close and rename") {
        controller.ProcessCommand(*command);
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        const auto registry = project.registry.Snapshot();
        const auto *record = registry.FindByPath(path);
        REQUIRE(record);
        CHECK(record->type.Value() == AI::BlackboardSchemaAssetType);
        CHECK(std::filesystem::is_regular_file(project.root / path));
        CHECK(std::filesystem::is_regular_file(project.root / (std::string{path} + ".horo")));
        const auto &opened = project.Snapshot(controller);
        REQUIRE(opened.source->rows.size() == 1);
        CHECK(opened.source->asset == record->id);
        CHECK(opened.source->rows.front().displayName == std::optional<std::string>{"Patrol radius"});
        CHECK(opened.source->rows.front().descriptor.kind == AI::BlackboardValueKind::SignedInteger);
        CHECK(opened.source->rows.front().descriptor.access == AI::BlackboardKeyAccess::ReadOnly);
        CHECK(opened.source->rows.front().descriptor.presence == AI::BlackboardKeyPresence::Required);
        CHECK(opened.source->rows.front().descriptor.defaultValue == draft.request.key.defaultValue);
        CHECK_FALSE(controller.HasDirtySourceDocuments());
        const auto identity = opened.source->schema;
        const auto firstKey = opened.source->rows.front().descriptor.key;
        const auto instance = opened.document.instance;

        controller.ProcessCommand(*command);
        CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.name_exists");
        CHECK(project.registry.Snapshot().Revision() == registry.Revision());

        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = instance});
        REQUIRE(controller.ViewModel().blackboardDocuments.empty());
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::RenameContentBrowserEntry,
                                   .stringPayload = WorkspacePathText(project.root / path),
                                   .secondaryStringPayload = "Relocated.horo_bb"});
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        controller.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::OpenSourceFile,
             .sourceOpenRequest = SourceOpenRequest{.path = "assets/Relocated.horo_bb", .origin = SourceOpenOrigin::AssetActivation}});
        const auto &relocated = project.Snapshot(controller);
        CHECK(relocated.source->asset == record->id);
        CHECK(relocated.source->schema == identity);
        CHECK(relocated.source->rows.front().descriptor.key == firstKey);
    }
    SECTION("synthetic requests cannot bypass key and destination validation") {
        command->newBlackboard->key.defaultValue = AI::BlackboardScalarValue{true};
        controller.ProcessCommand(*command);
        CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.blackboard.create_failed");
        CHECK_FALSE(std::filesystem::exists(project.root / path));
        CHECK_FALSE(std::filesystem::exists(project.root / (std::string{path} + ".horo")));
        CHECK(project.registry.Snapshot().Records().empty());
        CHECK(controller.ViewModel().blackboardDocuments.empty());
    }
    SECTION("malformed saved schema leaves the semantic namespace unavailable without replacing it") {
        {
            std::ofstream bad(project.root / project.Path, std::ios::binary);
            bad << "bad source";
            REQUIRE(static_cast<bool>(bad));
        }
        project.WriteSidecar(project.Path, project.asset, AI::BlackboardSchemaAssetType);
        controller.ProcessCommand(*command);
        CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.blackboard.create_failed");
        CHECK_FALSE(std::filesystem::exists(project.root / path));
        CHECK_FALSE(std::filesystem::exists(project.root / (std::string{path} + ".horo")));
        CHECK(std::filesystem::is_regular_file(project.root / project.Path));
    }
}

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

TEST_CASE("Blackboard workspace publishes a Project usage review only for an unchanged document", "[unit][editor][blackboard][workspace]") {
    BlackboardWorkspaceFixture project;
    JobSystem jobs{JobSystemConfig{.ioWorkerCount = 1}};
    const auto registry = project.registry.Snapshot();
    CollisionInventoryEvidence inventory{registry.Revision(), Assets::AssetRegistryBuildStatus::Complete};
    EditorWorkspaceController controller(project.root, project.runtime, registry,
                                         {.assets = {.mutableAssetRegistry = &project.registry},
                                          .collision = {.inventory = &inventory},
                                          .mutations = &project.mutations,
                                          .durableFiles = &project.files,
                                          .jobs = &jobs});
    controller.ProcessCommand(project.Open());
    const auto issued = project.Current(controller);
    controller.ProcessCommand({.command = EditorWorkspaceViewCommand::BlackboardDocument,
                               .blackboard = BlackboardWorkspaceRequest{.fence = issued,
                                                                        .operation = BlackboardDocumentOperation::ReviewUsages,
                                                                        .reviewKey = project.key}});
    REQUIRE(controller.ViewModel().blackboardDocuments.front().reviewingUsages);
    bool changed = false;
    SECTION("review publishes on the normal workspace owner lane") {}
    SECTION("editing the source revokes the pending document review") {
        project.Rename(controller, issued, "Edited");
        changed = true;
    }
    SECTION("closing and reopening the tab cannot inherit the retired review") {
        const auto closed = controller.ResolveBlackboardClose(issued, SourceCloseDecision::Save);
        REQUIRE(closed.HasValue());
        REQUIRE(closed.Value());
        REQUIRE(controller.ViewModel().blackboardDocuments.empty());
        controller.ProcessCommand(project.Open());
        REQUIRE(project.Current(controller).instance != issued.instance);
        changed = true;
    }
    jobs.Shutdown(ShutdownPolicy::Drain);
    controller.UpdateBlackboardUsageReview();
    REQUIRE(controller.ViewModel().blackboardDocuments.size() == 1);
    const auto &view = controller.ViewModel().blackboardDocuments.front();
    CHECK_FALSE(view.reviewingUsages);
    const auto diagnostic = view.diagnostic ? std::format("{} {}", view.diagnostic->code.Value(), view.diagnostic->message) : std::string{};
    INFO(diagnostic);
    CHECK(static_cast<bool>(view.usageReview) == !changed);
    CHECK_FALSE(view.diagnostic);
}

TEST_CASE("Blackboard workspace reviews the loaded native Decision catalog and rejects its retired stamp",
          "[unit][editor][blackboard][workspace]") {
    BlackboardWorkspaceFixture project;
    Tests::WriteReplicationProject(project.root);
    const auto manifest = Tests::WriteGameplayModuleManifest(project.root, HORO_TEST_AI_MODULE_PATH, "game.ai.tests",
                                                             Gameplay::CurrentGameplayBuildFingerprint(),
                                                             Tests::ReadDescriptorRevision(HORO_TEST_AI_MODULE_REVISION_PATH));
    auto graph = AI::DecisionGraphTestSupport::TaskTree(2, 101);
    graph.blackboard = project.schema;
    const auto encoded = AI::DecisionPlanCodec::EncodeSource(std::span{&graph, 1}, graph.asset);
    REQUIRE(encoded.HasValue());
    const auto *bytes = reinterpret_cast<const char *>(encoded.Value().data());
    constexpr auto graphPath = "assets/provider_review.horo_bt";
    {
        std::ofstream file(project.root / graphPath, std::ios::binary);
        file.write(bytes, static_cast<std::streamsize>(encoded.Value().size()));
        REQUIRE(static_cast<bool>(file));
    }
    project.WriteSidecar(graphPath, Assets::AssetId::FromBytes({17}), AI::DecisionAssetType(AI::DecisionPlanKind::BehaviorTree));
    project.RebuildRegistry();

    JobSystem jobs{JobSystemConfig{.ioWorkerCount = 1}};
    const auto registry = project.registry.Snapshot();
    CollisionInventoryEvidence inventory{registry.Revision(), Assets::AssetRegistryBuildStatus::Complete};
    EditorWorkspaceController controller(project.root, project.runtime, registry,
                                         {.assets = {.mutableAssetRegistry = &project.registry},
                                          .collision = {.inventory = &inventory},
                                          .mutations = &project.mutations,
                                          .durableFiles = &project.files,
                                          .jobs = &jobs});
    controller.ProcessCommand(project.Open());
    controller.ProcessCommand({.command = EditorWorkspaceViewCommand::BlackboardDocument,
                               .blackboard = BlackboardWorkspaceRequest{.fence = project.Current(controller),
                                                                        .operation = BlackboardDocumentOperation::ReviewUsages,
                                                                        .reviewKey = project.key}});
    REQUIRE(controller.ViewModel().blackboardDocuments.front().reviewingUsages);

    bool replaced = false;
    SECTION("the declared node resolves through the real manifest-selected catalog") {}
    SECTION("replacement of the same manifest image still revokes older review output") {
        Tests::AdvanceLastWriteTime(manifest);
        controller.UpdateGameplaySources(1.0F);
        replaced = true;
    }
    jobs.Shutdown(ShutdownPolicy::Drain);
    controller.UpdateBlackboardUsageReview();
    const auto &view = controller.ViewModel().blackboardDocuments.front();
    CHECK_FALSE(view.reviewingUsages);
    if (replaced) {
        CHECK_FALSE(view.usageReview);
        REQUIRE(view.diagnostic);
        CHECK(view.diagnostic->code.Value() == Application::DecisionCookErrors::Stale.code.Value());
    } else {
        REQUIRE(view.usageReview);
        const auto providers = view.usageReview->providers.lock();
        REQUIRE(providers);
        REQUIRE(providers->Descriptors().size() == 1);
        CHECK(providers->Descriptors().front().type == AI::DecisionNodeTypeId::Create(101).Value());
        CHECK(std::ranges::none_of(view.usageReview->gaps, [](const Application::BlackboardUsageGap &gap) {
            return gap.kind == Application::BlackboardUsageGapKind::DecisionProvider;
        }));
    }
}
