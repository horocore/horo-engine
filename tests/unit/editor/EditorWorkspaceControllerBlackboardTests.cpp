#include "DecisionGraphCompilerTestSupport.h"
#include "GameplayModuleTestSupport.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Application/BlackboardUsageReview.h"
#include "Horo/Application/DecisionAssetCookHost.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Gameplay/GameModule.h"
#include "ProjectReplicationTestSupport.h"
#include "editor/screens/workspace/AiAssetPresentation.h"
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

        void Write(const char *label, AI::BlackboardKeyPresence presence = AI::BlackboardKeyPresence::Required,
                   AI::BlackboardValueKind kind = AI::BlackboardValueKind::Boolean) const {
            const std::array keys{AI::BlackboardKeyDescriptor{.key = key, .kind = kind, .presence = presence}};
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

TEST_CASE("Recent AI assets resolve current registered identity after an ordinary paired rename and move",
          "[unit][editor][blackboard][workspace]") {
    BlackboardWorkspaceFixture project;
    EditorWorkspaceController controller(project.root, project.runtime, project.registry.Snapshot(),
                                         {.assets = {.mutableAssetRegistry = &project.registry},
                                          .mutations = &project.mutations,
                                          .durableFiles = &project.files});
    const EditorWorkspaceViewCommandData recent{
        .command = EditorWorkspaceViewCommand::OpenRecentAiAsset,
        .recentAiAsset = project.asset,
    };
    controller.ProcessCommand(recent);
    CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.source_open.unavailable");
    REQUIRE(controller.ViewModel().blackboardDocuments.empty());
    controller.ProcessCommand(project.Open());
    auto &history = controller.ViewModel().recentAiAssets;
    REQUIRE(history.size() == 1);
    CHECK(history.front().asset == project.asset);
    CHECK(history.front().path == project.Path);
    const auto instance = project.Current(controller).instance;

    SECTION("an open entry focuses its existing instance and does not duplicate history") {
        controller.ProcessCommand(recent);
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        CHECK(controller.ViewModel().workspacePanelHost.ActiveDocument() == instance);
        CHECK(history.size() == 1);
        CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().size() == 1);
    }

    controller.ProcessCommand({.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = instance});
    REQUIRE(controller.ViewModel().blackboardDocuments.empty());
    SECTION("moving the pair reopens its current path and the same typed source") {
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::RenameContentBrowserEntry,
                                   .stringPayload = WorkspacePathText(project.root / project.Path),
                                   .secondaryStringPayload = "renamed.horo_bb"});
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        REQUIRE(history.front().path == "assets/renamed.horo_bb");
        const auto target = project.root / "assets/moved";
        REQUIRE(std::filesystem::create_directories(target));
        controller.ProcessCommand({
            .command = EditorWorkspaceViewCommand::TransferContentBrowserAsset,
            .contentBrowserTransfer =
                ContentBrowserAssetTransferRequest{
                    .absoluteSourcePath = WorkspacePathText(project.root / "assets/renamed.horo_bb"),
                    .absoluteDestinationDirectory = WorkspacePathText(target),
                    .mode = ContentBrowserTransferMode::Move,
                },
        });
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        REQUIRE(history.size() == 1);
        CHECK(history.front().path == "assets/moved/renamed.horo_bb");
        CHECK(history.front().available);
        CHECK_FALSE(std::filesystem::exists(project.root / "assets/renamed.horo_bb.horo"));
        REQUIRE(std::filesystem::is_regular_file(target / "renamed.horo_bb.horo"));
        controller.ProcessCommand(recent);
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        const auto &reopened = project.Snapshot(controller);
        CHECK(reopened.document.key.source.Value() == "assets/moved/renamed.horo_bb");
        CHECK(reopened.source->asset == project.asset);
        CHECK(reopened.source->schema == project.schema);
        CHECK(reopened.source->rows.front().descriptor.key == project.key);
        CHECK(history.size() == 1);
    }
    SECTION("the registry cache cannot authorize a replaced live identity sidecar") {
        project.WriteSidecar(project.Path, Assets::AssetId::FromBytes({9}), AI::BlackboardSchemaAssetType);
        REQUIRE(project.registry.Snapshot().Find(project.asset));
        controller.ProcessCommand(recent);
        CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.source_open.unavailable");
        CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().empty());
        CHECK(controller.ViewModel().blackboardDocuments.empty());
    }
    SECTION("the registry cache cannot authorize a removed live identity sidecar") {
        REQUIRE(std::filesystem::remove(project.root / (std::string{project.Path} + ".horo")));
        REQUIRE(project.registry.Snapshot().Find(project.asset));
        controller.ProcessCommand(recent);
        CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.source_open.unavailable");
        CHECK(controller.ViewModel().blackboardDocuments.empty());
        const auto rebuilt = Assets::RebuildAssetRegistry(project.registry, project.root, Assets::AssetRegistryOpenMode::ReadOnly);
        REQUIRE(rebuilt.HasValue());
        REQUIRE(rebuilt.Value().status == Assets::AssetRegistryBuildStatus::Degraded);
        controller.RefreshAssets(project.registry.Snapshot());
        CHECK_FALSE(history.front().available);
    }
}

TEST_CASE("State Machine New publishes a canonical Decision source bound to the selected saved Blackboard",
          "[unit][editor][decision][workspace]") {
    BlackboardWorkspaceFixture project;
    project.Write("Observer", AI::BlackboardKeyPresence::Optional);
    EditorWorkspaceController controller(project.root, project.runtime, project.registry.Snapshot(),
                                         {.assets = {.mutableAssetRegistry = &project.registry},
                                          .mutations = &project.mutations,
                                          .durableFiles = &project.files});
    REQUIRE(controller.ViewModel().decisionSchemaChoices.size() == 1);
    CHECK(controller.ViewModel().decisionSchemaChoices.front().asset == project.asset);
    AssetBrowserInteractionSession browser;
    browser.OpenCreateStateMachine(WorkspacePathText(project.root / "assets"));
    auto &draft = browser.State().createStateMachine;
    draft.name = "Terminal state";
    CHECK_FALSE(AssetBrowserInteractionSession::ConfirmStateMachine(draft).has_value());
    draft.schema = project.asset;
    draft.outcome = AI::DecisionStateTerminal::Failed;
    auto command = AssetBrowserInteractionSession::ConfirmStateMachine(draft);
    REQUIRE(command.has_value());
    constexpr auto path = "assets/Terminal state.horo_sm";

    SECTION("the durable source and sidecar open the same stable Decision document") {
        controller.ProcessCommand(BlackboardWorkspaceFixture::Open());
        REQUIRE(controller.ViewModel().blackboardDocuments.size() == 1);
        controller.ProcessCommand(*command);
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        REQUIRE(controller.ViewModel().workspacePanelHost.DocumentTabs().size() == 2);
        const auto registry = project.registry.Snapshot();
        const auto *record = registry.FindByPath(path);
        REQUIRE(record);
        CHECK(record->type.Value() == Application::DecisionStateMachineSourceType);
        CHECK(std::filesystem::is_regular_file(project.root / (std::string{path} + ".horo")));
        REQUIRE(controller.ViewModel().decisionGraphDocuments.size() == 1);
        const auto &document = controller.ViewModel().decisionGraphDocuments.front();
        CHECK(document.asset == record->id);
        CHECK(document.identity.key.kind == DocumentKind::DecisionGraph);
        CHECK(ParseDocumentKind("decision_graph").Value() == DocumentKind::DecisionGraph);
        CHECK(document.graph->blackboard == project.schema);
        CHECK(document.graph->blackboardVersion.Contains(1));
        CHECK_FALSE(document.graph->blackboardVersion.Contains(2));
        CHECK(document.graph->executionTask.IsValid());
        const auto &machine = std::get<AI::StateMachineAssetDescriptor>(document.graph->body);
        REQUIRE(machine.states.size() == 1);
        CHECK(machine.initial == machine.states.front().id);
        CHECK(machine.states.front().terminal == AI::DecisionStateTerminal::Failed);
        CHECK(document.graph->location.source == path);
        const auto graph = document.graph->asset;

        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::RenameContentBrowserEntry,
                                   .stringPayload = WorkspacePathText(project.root / path),
                                   .secondaryStringPayload = "Relocated.horo_sm"});
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        controller.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::OpenSourceFile,
             .sourceOpenRequest = SourceOpenRequest{.path = "assets/Relocated.horo_sm", .origin = SourceOpenOrigin::AssetActivation}});
        REQUIRE(controller.ViewModel().decisionGraphDocuments.size() == 1);
        REQUIRE(controller.ViewModel().workspacePanelHost.DocumentTabs().size() == 2);
        const auto &moved = controller.ViewModel().decisionGraphDocuments.front();
        CHECK(moved.asset == record->id);
        CHECK(moved.graph->asset == graph);
        CHECK(moved.graph->location.source == "assets/Relocated.horo_sm");
        controller.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = moved.identity.instance});
        CHECK(controller.ViewModel().decisionGraphDocuments.empty());
        CHECK(controller.ViewModel().blackboardDocuments.size() == 1);
    }
    SECTION("stale synthetic schema selections never publish") {
        command->newStateMachine->schema = Assets::AssetId::FromBytes({0x99});
        controller.ProcessCommand(*command);
        CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.decision_graph.create_failed");
        CHECK_FALSE(std::filesystem::exists(project.root / path));
        CHECK_FALSE(std::filesystem::exists(project.root / (std::string{path} + ".horo")));
        CHECK(project.registry.Snapshot().FindByPath(path) == nullptr);
    }
    SECTION("a retained schema that cannot materialize is refused") {
        project.Write("Missing required default");
        controller.ProcessCommand(*command);
        CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.decision_graph.create_failed");
        CHECK_FALSE(std::filesystem::exists(project.root / path));
    }
}

TEST_CASE("Behavior Tree and Utility New use the current registered native task and selected saved schema",
          "[unit][editor][decision][workspace]") {
    BlackboardWorkspaceFixture project;
    Tests::WriteReplicationProject(project.root);
    project.Write("Score source", AI::BlackboardKeyPresence::Optional, AI::BlackboardValueKind::Scalar);
    const auto manifest = Tests::WriteGameplayModuleManifest(project.root, HORO_TEST_AI_MODULE_PATH, "game.ai.tests",
                                                             Gameplay::CurrentGameplayBuildFingerprint(),
                                                             Tests::ReadDescriptorRevision(HORO_TEST_AI_MODULE_REVISION_PATH));
    REQUIRE(std::filesystem::is_regular_file(manifest));
    EditorWorkspaceController controller(project.root, project.runtime, project.registry.Snapshot(),
                                         {.assets = {.mutableAssetRegistry = &project.registry},
                                          .mutations = &project.mutations,
                                          .durableFiles = &project.files});
    controller.ProcessCommand(BlackboardWorkspaceFixture::Open());
    const auto &choices = controller.ViewModel().decisionTaskChoices;
    const auto providerType = AI::DecisionNodeTypeId::Create(101).Value();
    const auto provider = std::ranges::find(choices, providerType, &DecisionTaskChoice::type);
    REQUIRE(provider != choices.end());
    AssetBrowserInteractionSession browser;
    const auto make = [&](AI::DecisionPlanKind family, const char *name) {
        browser.OpenCreateTaskDecision(WorkspacePathText(project.root / "assets"), family,
                                       controller.ViewModel().availableBehaviorsRevision);
        auto &draft = browser.State().createTaskDecision;
        draft.name = name;
        draft.schema = project.asset;
        CHECK_FALSE(AssetBrowserInteractionSession::CanConfirmTaskDecision(draft));
        draft.task = provider->type;
        draft.provider = provider->provider;
        draft.version = provider->version;
        if (draft.score) {
            const auto &blackboard = project.Snapshot(controller);
            REQUIRE(blackboard.sourceDigest.has_value());
            *draft.score = {.key = project.key, .schemaSource = *blackboard.sourceDigest, .minimum = -3, .maximum = 9};
        }
        auto command = AssetBrowserInteractionSession::ConfirmTaskDecision(draft);
        REQUIRE(command.has_value());
        return *command;
    };

    SECTION("both graph kinds publish ordinary paired assets and coexist with the saved Blackboard") {
        controller.ProcessCommand(make(AI::DecisionPlanKind::BehaviorTree, "First tree"));
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        controller.ProcessCommand(make(AI::DecisionPlanKind::Utility, "First utility"));
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        const auto &graphs = controller.ViewModel().decisionGraphDocuments;
        REQUIRE(graphs.size() == 2);
        CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().size() == 3);
        const auto &tree = std::get<std::shared_ptr<const AI::BehaviorTreeAsset>>(graphs[0].graph->body);
        REQUIRE(tree->Nodes().size() == 2);
        const auto task = std::ranges::find(tree->Nodes(), AI::BehaviorTreeNodeKind::Task, &AI::BehaviorTreeNode::kind);
        REQUIRE(task != tree->Nodes().end());
        CHECK(task->type.Value() == providerType.Value());
        CHECK(task->typeVersion.major == provider->version);
        CHECK(graphs[0].graph->executionTask.IsValid());
        const auto &utility = std::get<AI::UtilityDecisionSource>(graphs[1].graph->body);
        REQUIRE(utility.options.size() == 1);
        CHECK(utility.options.front().provider.type == providerType);
        CHECK(utility.options.front().provider.descriptorVersion.Contains(provider->version));
        REQUIRE(utility.options.front().option.considerations.size() == 1);
        const auto &score = utility.options.front().option.considerations.front();
        CHECK(score.key == project.key);
        CHECK(score.minimum == -3);
        CHECK(score.maximum == 9);
        CHECK(graphs[0].graph->blackboard == project.schema);
        CHECK(graphs[1].graph->blackboard == project.schema);
        const auto live = project.registry.Snapshot();
        REQUIRE(live.FindByPath("assets/First tree.horo_bt"));
        REQUIRE(live.FindByPath("assets/First utility.horo_utility"));
        CHECK(std::filesystem::is_regular_file(project.root / "assets/First tree.horo_bt.horo"));
        CHECK(std::filesystem::is_regular_file(project.root / "assets/First utility.horo_utility.horo"));
    }
    SECTION("all five AI families share workspace tabs with Viewport and Game") {
        constexpr auto queryPath = "assets/Cover.horo_eqs";
        const auto queryAsset = Assets::AssetId::FromBytes({8});
        const auto query = AI::EnvironmentQueryAsset::Capture({
            .id = AI::QueryId::Create(2).Value(),
            .displayName = "Cover",
            .result = {.id = AI::QueryResultSchemaId::Create(3).Value(), .itemType = AI::QueryItemTypeId::Create(4).Value()},
            .stages = {{.id = AI::QueryStageId::Create(5).Value(),
                        .kind = AI::QueryStageKind::Unknown,
                        .displayName = "Contributed stage",
                        .descriptorVersion = {2, 3},
                        .unknownTypeId = 13,
                        .opaquePayload = {std::byte{0x27}}}},
        });
        REQUIRE(query.HasValue());
        const auto encoded = AI::EnvironmentQuerySourceCodec::Encode(query.Value());
        REQUIRE(encoded.HasValue());
        {
            std::ofstream source(project.root / queryPath, std::ios::binary);
            source.write(reinterpret_cast<const char *>(encoded.Value().data()), static_cast<std::streamsize>(encoded.Value().size()));
            REQUIRE(static_cast<bool>(source));
        }
        project.WriteSidecar(queryPath, queryAsset, AI::EnvironmentQuerySourceAssetType);
        project.RebuildRegistry();
        controller.RefreshAssets(project.registry.Snapshot());
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::ChangeActivePanel,
                                   .targetIndex = static_cast<int>(WorkspaceDockArea::Document),
                                   .stringPayload = "horo.game"});
        controller.ProcessCommand(make(AI::DecisionPlanKind::BehaviorTree, "First tree"));
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        controller.ProcessCommand(make(AI::DecisionPlanKind::Utility, "First utility"));
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        browser.OpenCreateStateMachine(WorkspacePathText(project.root / "assets"));
        auto &stateMachine = browser.State().createStateMachine;
        stateMachine.name = "First machine";
        stateMachine.schema = project.asset;
        stateMachine.outcome = AI::DecisionStateTerminal::Succeeded;
        const auto newMachine = AssetBrowserInteractionSession::ConfirmStateMachine(stateMachine);
        REQUIRE(newMachine.has_value());
        controller.ProcessCommand(*newMachine);
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::OpenSourceFile,
                                   .sourceOpenRequest = SourceOpenRequest{.path = queryPath, .origin = SourceOpenOrigin::AssetActivation}});
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());

        const auto &view = controller.ViewModel();
        CHECK(view.workspacePanelHost.DocumentTabs().size() == 5);
        CHECK(view.blackboardDocuments.size() == 1);
        CHECK(view.decisionGraphDocuments.size() == 3);
        REQUIRE(view.environmentQueryDocuments.size() == 1);
        CHECK(view.environmentQueryDocuments.front().asset == queryAsset);
        const auto *central = view.workspacePanelHost.Layout().FindTabStack("workspace.document");
        REQUIRE(central);
        CHECK(std::ranges::find(central->tabs, "horo.viewport") != central->tabs.end());
        CHECK(std::ranges::find(central->tabs, "horo.game") != central->tabs.end());

        const std::array expectedIcons{Ui::UiIcon::AiEnvironmentQuery, Ui::UiIcon::AiStateMachine, Ui::UiIcon::AiUtility,
                                       Ui::UiIcon::AiBehaviorTree, Ui::UiIcon::AiBlackboard};
        REQUIRE(view.recentAiAssets.size() == expectedIcons.size());
        for (std::size_t index = 0; index < expectedIcons.size(); ++index) {
            const auto *presentation = FindAiAssetPresentation(view.recentAiAssets[index].type.Value());
            REQUIRE(presentation);
            const auto icon = presentation->icon;
            CHECK(icon == expectedIcons[index]);
            const auto token = Ui::UiIconRegistry::Token(icon);
            CHECK(token.starts_with("asset.ai_"));
            CHECK(Ui::UiIconRegistry::Resolve(token) == icon);
        }
        CHECK_FALSE(FindAiAssetPresentation("game.custom_source"));
    }
    SECTION("wrong native generation and provider identity are refused before publishing") {
        auto command = make(AI::DecisionPlanKind::BehaviorTree, "First tree");
        ++command.newTaskDecision->catalogRevision;
        controller.ProcessCommand(command);
        CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.decision_graph.create_failed");
        --command.newTaskDecision->catalogRevision;
        command.newTaskDecision->provider = AI::DecisionProviderId::Create(987).Value();
        controller.ProcessCommand(command);
        CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.decision_graph.create_failed");
        CHECK_FALSE(std::filesystem::exists(project.root / "assets/First tree.horo_bt"));
        CHECK(controller.ViewModel().decisionGraphDocuments.empty());
    }
    SECTION("the score cannot silently bind to changed durable schema bytes") {
        const auto command = make(AI::DecisionPlanKind::Utility, "First utility");
        project.Write("Changed externally", AI::BlackboardKeyPresence::Optional, AI::BlackboardValueKind::Scalar);
        controller.ProcessCommand(command);
        CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.decision_graph.create_failed");
        CHECK_FALSE(std::filesystem::exists(project.root / "assets/First utility.horo_utility"));
    }
}

TEST_CASE("Decision assets open canonical family tabs even with unavailable task providers", "[unit][editor][decision][workspace]") {
    using namespace AI::DecisionGraphTestSupport;
    BlackboardWorkspaceFixture project;
    const auto tree = TaskTree(601);
    const AI::DecisionGraphSource utility{Graph(602),
                                          1,
                                          project.schema,
                                          {1, 1},
                                          Location(),
                                          {},
                                          AI::UtilityDecisionSource{.root = Node(1),
                                                                    .options = {{.option = {Node(2), MakeIdentity<AI::TaskId>(701)},
                                                                                 .provider = {.id = Node(2), .type = Type(702)}}}}};
    const auto write = [&](const AI::DecisionGraphSource &source, const std::string &path, Assets::AssetId id, std::string_view type) {
        const auto encoded = AI::DecisionPlanCodec::EncodeSource(std::span{&source, 1}, source.asset);
        REQUIRE(encoded.HasValue());
        {
            std::ofstream file(project.root / path, std::ios::binary);
            file.write(reinterpret_cast<const char *>(encoded.Value().data()), static_cast<std::streamsize>(encoded.Value().size()));
            REQUIRE(static_cast<bool>(file));
        }
        project.WriteSidecar(path, id, type);
    };
    write(tree, "assets/guard.horo_bt", Assets::AssetId::FromBytes({0x21}), Application::DecisionBehaviorTreeSourceType);
    write(utility, "assets/score.horo_utility", Assets::AssetId::FromBytes({0x22}), Application::DecisionUtilitySourceType);
    write(tree, "assets/wrong.horo_sm", Assets::AssetId::FromBytes({0x23}), Application::DecisionStateMachineSourceType);
    project.RebuildRegistry();
    EditorWorkspaceController controller(project.root, project.runtime, project.registry.Snapshot(), {});
    const auto open = [&](const char *path) {
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::OpenSourceFile,
                                   .sourceOpenRequest = SourceOpenRequest{.path = path, .origin = SourceOpenOrigin::AssetActivation}});
    };
    open("assets/guard.horo_bt");
    open("assets/score.horo_utility");
    const auto &documents = controller.ViewModel().decisionGraphDocuments;
    REQUIRE(documents.size() == 2);
    CHECK(documents[0].graph->asset == tree.asset);
    CHECK(documents[1].graph->asset == utility.asset);
    CHECK(std::holds_alternative<std::shared_ptr<const AI::BehaviorTreeAsset>>(documents[0].graph->body));
    CHECK(std::holds_alternative<AI::UtilityDecisionSource>(documents[1].graph->body));
    open("assets/wrong.horo_sm");
    CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.decision_graph.invalid");
    CHECK(documents.size() == 2);
    CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().size() == 2);
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
