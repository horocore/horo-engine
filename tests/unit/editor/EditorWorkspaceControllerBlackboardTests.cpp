#include "DecisionGraphCompilerTestSupport.h"
#include "GameplayModuleTestSupport.h"
#include "Horo/AI/BlackboardUsageInspection.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/AI/EnvironmentQueryGenerators.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"
#include "Horo/Application/BlackboardUsageReview.h"
#include "Horo/Application/DecisionAssetCookHost.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Gameplay/GameModule.h"
#include "NativePublicationFiles.h"
#include "ProjectReplicationTestSupport.h"
#include "editor/screens/workspace/AiAssetPresentation.h"
#include "editor/screens/workspace/BlackboardScalarInput.h"
#include "editor/screens/workspace/DecisionGraphCanvas.h"
#include "editor/screens/workspace/DecisionGraphCanvasModel.h"
#include "editor/screens/workspace/DecisionGraphDocumentHistory.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/WorkspacePathText.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserInteractionSession.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    TEST_CASE("Decision document reserves a complete exact root before touching its history", "[editor][decision-graph][history]") {
        DecisionGraphDocumentHistory history;
        const DecisionGraphDocumentRoot original{std::make_shared<const AI::DecisionGraphSource>(),
                                                 std::make_shared<const DecisionGraphLayout>(),
                                                 {}};
        const DecisionGraphHistoryFence current{{DocumentInstanceId::Create(1).Value(), 3}, {}, original};
        auto candidate = original;
        candidate.graph = std::make_shared<const AI::DecisionGraphSource>();
        candidate.digest.bytes.front() = 1;

        CHECK(history.PrepareChange(current, candidate, 8 * 1024 * 1024).HasError());
        auto pending = history.PrepareChange(current, candidate, 32);
        REQUIRE(pending.HasValue());
        auto prepared = std::move(pending).Value();
        CHECK_FALSE(history.HasReplay(false));
        CHECK(prepared.Candidate() == candidate);

        auto stale = current;
        ++stale.document.revision;
        CHECK_FALSE(history.CanPublish(prepared, stale));
        stale = current;
        stale.document.instance = DocumentInstanceId::Create(2).Value();
        CHECK_FALSE(history.CanPublish(prepared, stale));
        stale = current;
        stale.acceptedSource.bytes.front() = 2;
        CHECK_FALSE(history.CanPublish(prepared, stale));
        stale = current;
        stale.root.layout = std::make_shared<const DecisionGraphLayout>();
        CHECK_FALSE(history.CanPublish(prepared, stale));
        stale = current;
        stale.conflict = true;
        CHECK_FALSE(history.CanPublish(prepared, stale));
        const DecisionGraphDocumentHistory foreign;
        CHECK_FALSE(foreign.CanPublish(prepared, current));

        auto alsoPending = history.PrepareChange(current, candidate, 32);
        REQUIRE(alsoPending.HasValue());
        CHECK_FALSE(history.Publish(prepared, stale));
        CHECK_FALSE(history.HasReplay(false));
        REQUIRE(history.Publish(prepared, current));
        CHECK(history.HasReplay(false));
        CHECK_FALSE(history.Publish(prepared, current));
        CHECK_FALSE(history.CanPublish(alsoPending.Value(), current));
    }

    TEST_CASE("Decision coordinated history requires the exact group for undo and redo", "[editor][decision-graph][history]") {
        DecisionGraphDocumentHistory history;
        constexpr std::uint64_t group = 7;
        const DecisionGraphDocumentRoot original{std::make_shared<const AI::DecisionGraphSource>(),
                                                 std::make_shared<const DecisionGraphLayout>(),
                                                 {}};
        DecisionGraphHistoryFence current{{DocumentInstanceId::Create(1).Value(), 1}, {}, original};
        auto candidate = original;
        candidate.graph = std::make_shared<const AI::DecisionGraphSource>();
        candidate.digest.bytes.front() = 1;
        auto pending = history.PrepareChange(current, candidate, 48, group);
        REQUIRE(pending.HasValue());
        auto prepared = std::move(pending).Value();
        REQUIRE(history.Publish(prepared, current));
        current.root = prepared.Candidate();
        ++current.document.revision;

        CHECK(history.NextReferenceGroup(false) == group);
        CHECK(history.PrepareReplay(current, false).HasError());
        CHECK(history.PrepareReplay(current, false, group + 1).HasError());
        auto next = history.PrepareReplay(current, false, group);
        REQUIRE(next.HasValue());
        auto pendingUndo = std::move(next).Value();
        auto undo = std::move(pendingUndo);
        CHECK_FALSE(history.Publish(pendingUndo, current));
        CHECK(undo.Candidate() == original);
        CHECK(history.NextReferenceGroup(false) == group);
        CHECK_FALSE(history.HasReplay(true));
        REQUIRE(history.Publish(undo, current));
        current.root = undo.Candidate();
        ++current.document.revision;

        CHECK(history.NextReferenceGroup(true) == group);
        CHECK(history.PrepareReplay(current, true).HasError());
        auto nextRedo = history.PrepareReplay(current, true, group);
        REQUIRE(nextRedo.HasValue());
        auto redo = std::move(nextRedo).Value();
        CHECK(redo.Candidate() == candidate);
        REQUIRE(history.Publish(redo, current));
        CHECK(history.NextReferenceGroup(false) == group);
        CHECK_FALSE(history.HasReplay(true));
    }

    /** @brief Injects one layout failure while keeping Project leases and source publication native. */
    class DecisionSaveFaultFiles final : public TestSupport::NativePublicationFiles {
    public:
        enum class Failure {
            None,
            BeforeLayoutReplace,
            AfterLayoutReplace
        };
        static constexpr auto BeforeMessage = "Injected layout replacement refusal";
        static constexpr auto AfterMessage = "Injected layout durability failure";
        std::filesystem::path graph;
        std::filesystem::path layout;
        Failure failure{};
        std::vector<std::filesystem::path> leases;
        std::size_t graphWrites{};
        std::size_t layoutWrites{};

        Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path, std::string_view owner) override {
            leases.push_back(path);
            return native.TryAcquireExclusive(path, owner);
        }

        Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                         std::filesystem::perms mode) override {
            return native.WritePrivateDurable(path, bytes, created, mode);
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                          AtomicFileReplacementReceipt &receipt) override {
            if (destination == graph)
                ++graphWrites;
            if (destination != layout)
                return native.AtomicReplaceTracked(prepared, destination, receipt);
            ++layoutWrites;
            if (failure == Failure::BeforeLayoutReplace)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed, BeforeMessage));
            auto result = native.AtomicReplaceTracked(prepared, destination, receipt);
            if (result.HasValue() && receipt.WasCommitted() && failure == Failure::AfterLayoutReplace)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed, AfterMessage));
            return result;
        }
    };

    /** @brief Drives the production canvas through headless ImGui frames and its rendered viewport bounds. */
    struct DecisionCanvasWorkspaceFrames final {
        Horo::Editor::Tests::EditorGuiContextFixture gui{{1000, 740}};
        DecisionGraphCanvas canvas;
        ImVec2 viewportMin;
        ImVec2 viewportMax;

        DecisionCanvasWorkspaceFrames() {
            ImGui::GetIO().IniFilename = nullptr;
            ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
            Theme::ApplyCurrentTheme();
        }

        DecisionCanvasInteraction Draw(const EditorWorkspaceController &controller, const DecisionGraphWorkspaceDocument &document) {
            gui.imgui.BeginFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({920, 650});
            ImGui::Begin("Decision gesture", nullptr, ImGuiWindowFlags_NoSavedSettings);
            const auto gesture = canvas.Draw(document, gui.context, controller.ViewModel().decisionProviderMetadata);
            viewportMin = ImGui::GetItemRectMin();
            viewportMax = ImGui::GetItemRectMax();
            ImGui::End();
            gui.imgui.EndFrame();
            return gesture;
        }

        /** @brief Captures the bounds and sample count of a native viewport's rendered color. */
        struct ColorBounds final {
            ImVec2 low;
            ImVec2 high;
            std::size_t samples{};
        };

        ColorBounds FindColor(ImU32 color) const {
            const auto *draw = ImGui::GetDrawData();
            REQUIRE(draw);
            ColorBounds match{viewportMax, viewportMin};
            for (int list = 0; list < draw->CmdListsCount; ++list) {
                for (const auto &vertex : draw->CmdLists[list]->VtxBuffer) {
                    const auto &pos = vertex.pos;
                    if (vertex.col != color || pos.x <= viewportMin.x || pos.x >= viewportMax.x || pos.y <= viewportMin.y ||
                        pos.y >= viewportMax.y)
                        continue;
                    match.low = {std::min(match.low.x, pos.x), std::min(match.low.y, pos.y)};
                    match.high = {std::max(match.high.x, pos.x), std::max(match.high.y, pos.y)};
                    ++match.samples;
                }
            }
            return match;
        }

        /** @brief Locates one visible colored pin before a one-node fixture grows any edges or selection. */
        ImVec2 PinCenter(ImU32 color) const {
            const auto [low, high, samples] = FindColor(color);
            REQUIRE(samples >= 4);
            const auto width = high.x - low.x;
            const auto height = high.y - low.y;
            REQUIRE(width > 0.0F);
            REQUIRE(height > 0.0F);
            REQUIRE(width < height * 2.0F);
            REQUIRE(height < width * 2.0F);
            return {(low.x + high.x) * 0.5F, (low.y + high.y) * 0.5F};
        }
    };

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
            const std::array rows{AI::BlackboardSchemaAuthoringRow{key, label}};
            WriteSchema(keys, rows);
        }

        void WriteSchema(std::span<const AI::BlackboardKeyDescriptor> keys, std::span<const AI::BlackboardSchemaAuthoringRow> rows) const {
            const auto captured = AI::BlackboardSchema::Capture({.identity = schema, .keys = keys});
            REQUIRE(captured.HasValue());
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
    SECTION("missing identity leaves the visible untyped source unchanged") {
        controller.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::NavigateContentBrowser, .stringPayload = WorkspacePathText(project.root / "assets")});
        const auto sourcePath = (project.root / project.Path).lexically_normal();
        const auto &directory = controller.ViewModel().contentBrowser;
        REQUIRE(WorkspaceNativePath(directory.absoluteCurrentPath).lexically_normal() == sourcePath.parent_path());
        const auto isSource = [&](const ContentBrowserEntry &entry) {
            return WorkspaceNativePath(entry.absolutePath).lexically_normal() == sourcePath;
        };
        const auto &registeredEntries = controller.ViewModel().contentBrowser.entries;
        const auto registered = std::ranges::find_if(registeredEntries, isSource);
        REQUIRE(registered != registeredEntries.end());
        REQUIRE(registered->registered);
        const auto original = Assets::CaptureAssetSource(project.root, sourcePath, AI::BlackboardSchemaSourceCodec::MaximumBytes);
        REQUIRE(original.HasValue());
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
        const auto &entries = controller.ViewModel().contentBrowser.entries;
        const auto visible = std::ranges::find_if(entries, isSource);
        REQUIRE(visible != entries.end());
        CHECK_FALSE(visible->registered);
        CHECK_FALSE(visible->registeredType.has_value());

        controller.ProcessCommand(project.Open());
        CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.blackboard.invalid");
        CHECK(controller.ViewModel().blackboardDocuments.empty());
        const auto preserved = Assets::CaptureAssetSource(project.root, sourcePath, AI::BlackboardSchemaSourceCodec::MaximumBytes);
        REQUIRE(preserved.HasValue());
        CHECK(preserved.Value().bytes == original.Value().bytes);
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

    SECTION("intrinsic topology commands validate complete drafts before undo, redo or durable save") {
        controller.ProcessCommand(*command);
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        const auto &documents = controller.ViewModel().decisionGraphDocuments;
        REQUIRE(documents.size() == 1);
        REQUIRE(documents.front().editable);
        const auto instance = documents.front().identity.instance;
        const auto asset = documents.front().asset;
        const auto original = documents.front().graph;
        const auto initial = std::get<AI::StateMachineAssetDescriptor>(original->body).initial;
        const auto edit = [&](StateMachineEditOperation operation, AI::DecisionNodeId node = {}, AI::DecisionNodeId target = {},
                              AI::DecisionStateTerminal terminal = AI::DecisionStateTerminal::None,
                              std::optional<std::uint64_t> revision = {}) {
            controller.ProcessCommand({
                .command = EditorWorkspaceViewCommand::EditDecisionStateMachine,
                .stateMachineEdit =
                    StateMachineEditRequest{instance, revision.value_or(documents.front().revision), operation, node, target, terminal},
            });
        };
        const auto &view = documents.front();
        using enum StateMachineEditOperation;
        constexpr auto bound = AI::DecisionCodecLimits{}.maximumBytes;
        const auto initialDisk = Assets::CaptureAssetSource(project.root, project.root / path, bound);
        REQUIRE(initialDisk.HasValue());
        const auto startRevision = view.revision;
        edit(AddState, initial, {}, AI::DecisionStateTerminal::Succeeded, startRevision + 1);
        CHECK(view.diagnostic.has_value());
        edit(AddState, initial, {}, AI::DecisionStateTerminal::Succeeded);
        CHECK(view.diagnostic.has_value());
        CHECK(view.graph == original);
        CHECK(view.revision == startRevision);
        CHECK_FALSE(view.canUndo);

        edit(SetTerminal, initial);
        REQUIRE_FALSE(view.diagnostic.has_value());
        const auto running = view.graph;
        const auto revision = view.revision;
        edit(SetTerminal, initial);
        CHECK_FALSE(view.diagnostic.has_value());
        CHECK(view.revision == revision);
        edit(AddState);
        CHECK(view.diagnostic.has_value());
        CHECK(view.graph == running);
        CHECK(view.revision == revision);

        edit(AddState, initial, {}, AI::DecisionStateTerminal::Succeeded);
        REQUIRE_FALSE(view.diagnostic.has_value());
        const auto connected = view.graph;
        const auto &withEdge = std::get<AI::StateMachineAssetDescriptor>(connected->body);
        REQUIRE(withEdge.states.size() == 2);
        REQUIRE(withEdge.transitions.size() == 1);
        const auto addedId = withEdge.states.back().id;
        const auto edge = withEdge.transitions.front().id;
        CHECK(edge != addedId);
        CHECK(withEdge.transitions.front().source == initial);
        CHECK(withEdge.transitions.front().target == addedId);

        edit(Undo);
        REQUIRE_FALSE(view.diagnostic.has_value());
        CHECK(view.graph == running);
        CHECK(view.canRedo);
        edit(Redo);
        REQUIRE_FALSE(view.diagnostic.has_value());
        CHECK(view.graph == connected);

        const auto connectedRevision = view.revision;
        edit(Connect, addedId, initial);
        CHECK(view.diagnostic.has_value());
        const DecisionGraphDocumentFence canvasFence{instance, view.revision};
        const auto projection = ProjectDecisionCanvas(*view.graph, canvasFence, view.editable);
        REQUIRE(projection.family == AI::DecisionPlanKind::StateMachine);
        REQUIRE(projection.nodes.size() == 2);
        REQUIRE(projection.links.size() == 1);
        CHECK(projection.links.front().authoredId == edge.Value());
        const auto from = std::ranges::find(projection.pins, GraphPinView{initial, 0, true});
        const auto to = std::ranges::find(projection.pins, GraphPinView{addedId, 0, false});
        REQUIRE(from != projection.pins.end());
        REQUIRE(to != projection.pins.end());
        const auto newConnection = ResolveDecisionCanvasEdit(projection, {canvasFence, GraphConnectIntent{*to, *from}});
        REQUIRE(newConnection.has_value());
        CHECK(newConnection->operation == Connect);
        CHECK(newConnection->node == initial);
        CHECK(newConnection->target == addedId);
        CHECK_FALSE(ResolveDecisionCanvasEdit(projection, {canvasFence, GraphConnectIntent{*from, *from}}));
        CHECK_FALSE(ResolveDecisionCanvasEdit(projection, {canvasFence, GraphConnectIntent{GraphPinView{addedId, 0, true}, *to}}));
        CHECK_FALSE(ResolveDecisionCanvasEdit(projection, {{instance, view.revision + 1},
                                                           GraphRemoveLinkIntent{GraphLinkRole::Transition, edge.Value()}}));
        CHECK_FALSE(ResolveDecisionCanvasEdit(projection, {canvasFence, GraphRemoveLinkIntent{GraphLinkRole::Hierarchy, edge.Value()}}));
        const auto remove =
            ResolveDecisionCanvasEdit(projection, {canvasFence, GraphRemoveLinkIntent{GraphLinkRole::Transition, edge.Value()}});
        REQUIRE(remove.has_value());
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::EditDecisionStateMachine, .stateMachineEdit = remove});
        CHECK(view.diagnostic.has_value());
        edit(RemoveState, initial);
        CHECK(view.diagnostic.has_value());
        CHECK(view.revision == connectedRevision);
        CHECK(view.graph == connected);

        edit(RemoveState, addedId);
        REQUIRE_FALSE(view.diagnostic.has_value());
        const auto &removed = std::get<AI::StateMachineAssetDescriptor>(view.graph->body);
        CHECK(removed.states.size() == 1);
        CHECK(removed.transitions.empty());
        edit(Undo);
        REQUIRE_FALSE(view.diagnostic.has_value());
        CHECK(view.graph == connected);
        edit(Redo);
        REQUIRE_FALSE(view.diagnostic.has_value());
        CHECK(std::get<AI::StateMachineAssetDescriptor>(view.graph->body).transitions.empty());
        edit(Undo);
        REQUIRE_FALSE(view.diagnostic.has_value());
        CHECK(view.graph == connected);
        CHECK(controller.HasDirtySourceDocuments());

        const auto unchanged = Assets::CaptureAssetSource(project.root, project.root / path, bound);
        REQUIRE(unchanged.HasValue());
        CHECK(unchanged.Value().identity == initialDisk.Value().identity);
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = instance});
        REQUIRE(documents.size() == 1);
        REQUIRE(view.diagnostic.has_value());
        CHECK(view.graph == connected);
        const auto staleClose = controller.ResolveDecisionGraphClose({instance, revision}, SourceCloseDecision::Discard);
        CHECK(staleClose.HasError());
        CHECK(documents.size() == 1);

        project.WriteSidecar(path, Assets::AssetId::FromBytes({9}), Application::DecisionStateMachineSourceType);
        const auto conflicted = controller.ResolveDecisionGraphClose({instance, view.revision}, SourceCloseDecision::Save);
        CHECK(conflicted.HasError());
        REQUIRE(documents.size() == 1);
        CHECK(view.conflict);
        CHECK(view.graph == connected);
        CHECK(view.dirty);
        project.WriteSidecar(path, asset, Application::DecisionStateMachineSourceType);

        const auto save = controller.SaveSourceDocuments();
        REQUIRE(save.HasValue());
        CHECK_FALSE(controller.HasDirtySourceDocuments());
        CHECK(view.canUndo);
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = instance});
        REQUIRE(documents.empty());
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::OpenSourceFile,
                                   .sourceOpenRequest = SourceOpenRequest{.path = path, .origin = SourceOpenOrigin::AssetActivation}});
        REQUIRE(documents.size() == 1);
        CHECK(documents.front().asset == asset);
        CHECK(documents.front().graph->asset == original->asset);
        const auto &saved = std::get<AI::StateMachineAssetDescriptor>(documents.front().graph->body);
        REQUIRE(saved.states.size() == 2);
        REQUIRE(saved.transitions.size() == 1);
        CHECK(saved.transitions.front().id == edge);
        CHECK(saved.transitions.front().source == initial);
        CHECK(saved.transitions.front().target == addedId);
        CHECK_FALSE(documents.front().canUndo);
    }

    SECTION("signed transition priority uses canonical history and survives an exact durable save") {
        controller.ProcessCommand(*command);
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        const auto &documents = controller.ViewModel().decisionGraphDocuments;
        REQUIRE(documents.size() == 1);
        const auto &view = documents.front();
        REQUIRE(view.editable);
        const auto instance = view.identity.instance;
        const auto machine = [](const auto &document) -> const AI::StateMachineAssetDescriptor & {
            return std::get<AI::StateMachineAssetDescriptor>(document.graph->body);
        };
        const auto initial = machine(view).initial;
        using enum StateMachineEditOperation;
        const auto topology = [&](StateMachineEditOperation operation, AI::DecisionStateTerminal terminal) {
            controller.ProcessCommand(
                {.command = EditorWorkspaceViewCommand::EditDecisionStateMachine,
                 .stateMachineEdit = StateMachineEditRequest{instance, view.revision, operation, initial, {}, terminal}});
            REQUIRE_FALSE(view.diagnostic);
        };
        const auto history = [&](DecisionHistoryAction action) {
            controller.ProcessCommand(MakeDecisionHistoryCommand(view, action));
            REQUIRE_FALSE(view.diagnostic);
        };
        topology(SetTerminal, AI::DecisionStateTerminal::None);
        topology(AddState, AI::DecisionStateTerminal::Succeeded);
        topology(AddState, AI::DecisionStateTerminal::Failed);
        history(DecisionHistoryAction::Save);
        REQUIRE_FALSE(view.dirty);
        REQUIRE(machine(view).transitions.size() == 2);
        const auto edge = std::ranges::min_element(machine(view).transitions, {}, &AI::StateMachineTransition::id)->id;
        const auto other = std::ranges::max_element(machine(view).transitions, {}, &AI::StateMachineTransition::id)->id;
        REQUIRE(edge != other);
        const auto transition = [&](const auto &document, AI::DecisionNodeId id) -> const AI::StateMachineTransition & {
            const auto &transitions = machine(document).transitions;
            const auto found = std::ranges::find(transitions, id, &AI::StateMachineTransition::id);
            REQUIRE(found != transitions.end());
            return *found;
        };

        const auto baseline = view.graph;
        const auto layout = view.layout;
        const auto revision = view.revision;
        const auto disk = [&] {
            return Assets::CaptureAssetSource(project.root, project.root / path, AI::DecisionCodecLimits{}.maximumBytes);
        };
        const auto before = disk();
        REQUIRE(before.HasValue());
        const auto setPriority = [&](AI::DecisionNodeId id, std::optional<std::int32_t> value, std::uint64_t requestedRevision) {
            controller.ProcessCommand({.command = EditorWorkspaceViewCommand::EditDecisionStateMachine,
                                       .stateMachineEdit = StateMachineEditRequest{.instance = instance,
                                                                                   .revision = requestedRevision,
                                                                                   .operation = SetTransitionPriority,
                                                                                   .node = id,
                                                                                   .priority = value}});
        };
        constexpr auto minimum = std::numeric_limits<std::int32_t>::min();
        setPriority(edge, minimum, revision - 1);
        REQUIRE(view.diagnostic);
        CHECK(view.diagnostic->code.Value() == Application::DecisionCookErrors::Stale.code.Value());
        for (const auto invalid : {initial, AI::DecisionNodeId{}}) {
            setPriority(invalid, minimum, revision);
            REQUIRE(view.diagnostic);
            CHECK(view.diagnostic->code.Value() == Application::DecisionCookErrors::Invalid.code.Value());
        }
        setPriority(edge, std::nullopt, revision);
        REQUIRE(view.diagnostic);
        CHECK(view.diagnostic->code.Value() == Application::DecisionCookErrors::Invalid.code.Value());
        setPriority(edge, 0, revision);
        CHECK_FALSE(view.diagnostic);
        CHECK(view.revision == revision);
        CHECK(view.graph == baseline);
        CHECK_FALSE(view.dirty);

        const auto previous = transition(view, edge);
        REQUIRE(ProjectStateMachineTransitionOrder(machine(view), previous).rank == 1);
        setPriority(edge, minimum, revision);
        REQUIRE_FALSE(view.diagnostic);
        CHECK(view.revision == revision + 1);
        CHECK(view.graph != baseline);
        CHECK(view.layout == layout);
        CHECK(view.dirty);
        CHECK(transition(view, edge).priority == minimum);
        CHECK(transition(view, edge).source == previous.source);
        CHECK(transition(view, edge).target == previous.target);
        CHECK(transition(view, other).priority == 0);
        CHECK(ProjectStateMachineTransitionOrder(machine(view), transition(view, edge)).rank == 2);
        const auto uncommitted = disk();
        REQUIRE(uncommitted.HasValue());
        CHECK(uncommitted.Value().identity == before.Value().identity);

        history(DecisionHistoryAction::Undo);
        CHECK(view.graph == baseline);
        CHECK_FALSE(view.dirty);
        CHECK(view.canRedo);
        history(DecisionHistoryAction::Redo);
        CHECK(transition(view, edge).priority == minimum);
        CHECK(view.dirty);
        history(DecisionHistoryAction::Save);
        CHECK_FALSE(view.dirty);
        const auto saved = disk();
        REQUIRE(saved.HasValue());
        CHECK(saved.Value().identity.digest == view.source);
        CHECK(saved.Value().identity != before.Value().identity);

        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = instance});
        REQUIRE(documents.empty());
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::OpenSourceFile,
                                   .sourceOpenRequest = SourceOpenRequest{.path = path, .origin = SourceOpenOrigin::AssetActivation}});
        REQUIRE(documents.size() == 1);
        CHECK(documents.front().identity.instance != instance);
        CHECK(transition(documents.front(), edge).priority == minimum);
        CHECK(transition(documents.front(), other).priority == 0);
    }

    SECTION("a native canvas drag goes through workspace history and is fenced at mouse-down") {
        controller.ProcessCommand(*command);
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        const auto &documents = controller.ViewModel().decisionGraphDocuments;
        REQUIRE(documents.size() == 1);
        const auto &view = documents.front();
        REQUIRE(view.layoutWritable);
        const auto graph = view.graph;
        const auto instance = view.identity.instance;
        const auto state = std::get<AI::StateMachineAssetDescriptor>(graph->body).initial;

        DecisionCanvasWorkspaceFrames frames;
        const auto draw = [&] {
            return frames.Draw(controller, view);
        };
        for (int frame = 0; frame < 2; ++frame)
            CHECK_FALSE(draw().layout);
        frames.canvas.RequestFocus({instance, view.revision}, state);
        std::optional<AI::DecisionNodeId> revealed;
        for (int frame = 0; frame < 5; ++frame) {
            const auto event = draw();
            if (event.revealed)
                revealed = event.revealed;
            CHECK_FALSE(event.layout);
        }
        REQUIRE(revealed == state);

        auto &input = ImGui::GetIO();
        const ImVec2 start{(frames.viewportMin.x + frames.viewportMax.x) * 0.5F, (frames.viewportMin.y + frames.viewportMax.y) * 0.5F};
        input.AddMousePosEvent(start.x, start.y);
        CHECK_FALSE(draw().layout);
        const auto pressed = view.revision;
        input.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        CHECK_FALSE(draw().layout);
        for (const auto delta : {ImVec2{22, 16}, ImVec2{52, 35}}) {
            input.AddMousePosEvent(start.x + delta.x, start.y + delta.y);
            CHECK_FALSE(draw().layout);
        }
        CHECK(view.revision == pressed);
        CHECK_FALSE(view.dirty);

        const auto release = [&] {
            input.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            return draw();
        };
        const auto apply = [&](const DecisionGraphLayoutRequest &layout) {
            controller.ProcessCommand({.command = EditorWorkspaceViewCommand::EditDecisionLayout, .decisionLayoutEdit = layout});
            REQUIRE_FALSE(view.diagnostic);
        };
        SECTION("a release emits one Move accepted by normal undo and redo") {
            const auto event = release();
            REQUIRE(event.layout);
            CHECK(event.layout->fence.instance == instance);
            CHECK(event.layout->fence.revision == pressed);
            CHECK(event.layout->operation == DecisionLayoutOperation::Move);
            REQUIRE(event.layout->placements.size() == 1);
            CHECK(event.layout->placements.front().node == state);
            apply(*event.layout);
            CHECK(view.revision == pressed + 1);
            CHECK(view.graph == graph);
            REQUIRE(view.layout->size() == 1);
            CHECK(view.layout->front() == event.layout->placements.front());
            CHECK(view.dirty);
            CHECK(view.canUndo);
            CHECK_FALSE(draw().layout);
            const auto moved = view.layout;
            controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Undo));
            REQUIRE_FALSE(view.diagnostic);
            CHECK(view.layout->empty());
            CHECK_FALSE(view.dirty);
            controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Redo));
            REQUIRE_FALSE(view.diagnostic);
            CHECK(view.layout == moved);
            CHECK(view.dirty);
        }
        SECTION("publishing a different layout while held makes the old release stale") {
            const DecisionNodePlacement external{state, 245, 102};
            apply({{instance, pressed}, DecisionLayoutOperation::Move, {external}});
            REQUIRE(view.revision == pressed + 1);
            input.AddMousePosEvent(start.x + 76, start.y + 49);
            CHECK_FALSE(draw().layout);
            input.AddMousePosEvent(start.x + 103, start.y + 53);
            CHECK_FALSE(draw().layout);
            CHECK_FALSE(release().layout);
            CHECK_FALSE(draw().layout);
            CHECK(view.revision == pressed + 1);
            REQUIRE(view.layout->size() == 1);
            CHECK(view.layout->front() == external);
            controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Undo));
            REQUIRE_FALSE(view.diagnostic);
            CHECK(view.layout->empty());
            CHECK_FALSE(view.canUndo);
        }
    }

    SECTION("native pin drag admits valid connections and leaves rejected connections out of the graph") {
        controller.ProcessCommand(*command);
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        const auto &documents = controller.ViewModel().decisionGraphDocuments;
        REQUIRE(documents.size() == 1);
        const auto &view = documents.front();
        REQUIRE(view.editable);
        const auto instance = view.identity.instance;
        const auto machine = [&]() -> const AI::StateMachineAssetDescriptor & {
            return std::get<AI::StateMachineAssetDescriptor>(view.graph->body);
        };
        const auto state = machine().initial;
        bool allowed{};
        SECTION("a terminal state cannot create an outgoing link") {}
        SECTION("a running state can create a self-transition through shared history") {
            controller.ProcessCommand(
                {.command = EditorWorkspaceViewCommand::EditDecisionStateMachine,
                 .stateMachineEdit = StateMachineEditRequest{instance, view.revision, StateMachineEditOperation::SetTerminal, state}});
            REQUIRE_FALSE(view.diagnostic);
            controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Save));
            REQUIRE_FALSE(view.diagnostic);
            REQUIRE(machine().states.front().terminal == AI::DecisionStateTerminal::None);
            allowed = true;
        }
        REQUIRE(machine().transitions.empty());
        REQUIRE_FALSE(view.dirty);
        const auto before = view.graph;
        const auto revision = view.revision;

        DecisionCanvasWorkspaceFrames frames;
        const auto draw = [&] {
            return frames.Draw(controller, view);
        };
        for (int frame = 0; frame < 5; ++frame)
            CHECK_FALSE(draw().edit);
        const auto input = frames.PinCenter(Theme::U32(Theme::Muted()));
        const auto output = frames.PinCenter(Theme::U32(Theme::Accent()));
        const auto rejectionColor = ImGui::ColorConvertFloat4ToU32(Theme::Warn());
        REQUIRE(frames.FindColor(rejectionColor).samples == 0);
        REQUIRE(input.x < output.x);
        REQUIRE(output.y - 24.0F > frames.viewportMin.y);
        auto &mouse = ImGui::GetIO();
        mouse.AddMousePosEvent(output.x, output.y);
        CHECK_FALSE(draw().edit);
        mouse.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        CHECK_FALSE(draw().edit);

        mouse.AddMousePosEvent(output.x, output.y - 24);
        CHECK_FALSE(draw().edit);
        mouse.AddMousePosEvent(input.x, input.y);
        bool rejected{};
        for (int frame = 0; frame < 3; ++frame) {
            CHECK_FALSE(draw().edit);
            rejected = rejected || frames.FindColor(rejectionColor).samples > 0;
        }
        CHECK(rejected == !allowed);
        mouse.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        std::optional<StateMachineEditRequest> requested;
        for (int frame = 0; frame < 4; ++frame) {
            const auto event = draw();
            CHECK_FALSE(event.layout);
            if (event.edit) {
                REQUIRE_FALSE(requested);
                requested = event.edit;
            }
        }
        CHECK(view.revision == revision);
        CHECK(view.graph == before);
        CHECK_FALSE(view.dirty);

        if (allowed) {
            REQUIRE(requested);
            CHECK(requested->instance == instance);
            CHECK(requested->revision == revision);
            CHECK(requested->operation == StateMachineEditOperation::Connect);
            CHECK(requested->node == state);
            CHECK(requested->target == state);
            controller.ProcessCommand({.command = EditorWorkspaceViewCommand::EditDecisionStateMachine, .stateMachineEdit = requested});
            REQUIRE_FALSE(view.diagnostic);
            CHECK(view.revision == revision + 1);
            CHECK(view.dirty);
            REQUIRE(machine().transitions.size() == 1);
            CHECK(machine().transitions.front().source == state);
            CHECK(machine().transitions.front().target == state);
            const auto connected = view.graph;
            controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Undo));
            REQUIRE_FALSE(view.diagnostic);
            CHECK(view.graph == before);
            CHECK_FALSE(view.dirty);
            controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Redo));
            REQUIRE_FALSE(view.diagnostic);
            CHECK(view.graph == connected);
            CHECK(view.dirty);
        } else {
            CHECK_FALSE(requested);
            CHECK(machine().transitions.empty());
            CHECK_FALSE(view.canUndo);
        }
    }

    SECTION("canvas layout shares document history and saves separately across paired rename") {
        controller.ProcessCommand(*command);
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        const auto &documents = controller.ViewModel().decisionGraphDocuments;
        REQUIRE(documents.size() == 1);
        const auto &view = documents.front();
        REQUIRE(view.layoutWritable);
        REQUIRE(view.layout);
        CHECK(view.layout->empty());
        const auto instance = view.identity.instance;
        const auto asset = view.asset;
        const auto graph = view.graph;
        const auto node = std::get<AI::StateMachineAssetDescriptor>(graph->body).initial;
        const auto initial = view.revision;
        const auto sourcePath = project.root / path;
        const auto originalBytes = Assets::CaptureAssetSource(project.root, sourcePath, AI::DecisionCodecLimits{}.maximumBytes);
        REQUIRE(originalBytes.HasValue());
        const auto layout = [&](DecisionLayoutOperation operation, DecisionGraphLayout positions, std::uint64_t revision) {
            controller.ProcessCommand(
                {.command = EditorWorkspaceViewCommand::EditDecisionLayout,
                 .decisionLayoutEdit = DecisionGraphLayoutRequest{{instance, revision}, operation, std::move(positions)}});
        };
        const auto reject = [&](DecisionGraphLayout positions, std::uint64_t revision) {
            layout(DecisionLayoutOperation::Move, std::move(positions), revision);
            CHECK(view.diagnostic.has_value());
            CHECK(view.revision == initial);
            CHECK_FALSE(view.canUndo);
        };
        reject({{node, 24, 51}}, initial + 1);
        reject({{node, 1, 2}, {node, 4, 8}}, initial);
        reject({{node, std::numeric_limits<float>::quiet_NaN(), 0}}, initial);
        reject({{node, MaximumDecisionLayoutCoordinate + 1, 0}}, initial);
        reject({{AI::DecisionNodeId::Create(999).Value(), 0, 0}}, initial);
        reject(DecisionGraphLayout(MaximumDecisionLayoutNodes + 1, {node, 0, 0}), initial);

        layout(DecisionLayoutOperation::AutoLayout, {}, initial);
        REQUIRE_FALSE(view.diagnostic.has_value());
        const auto automatic = view.layout;
        REQUIRE(automatic->size() == 1);
        CHECK(automatic->front().node == node);
        const auto automaticRevision = view.revision;
        layout(DecisionLayoutOperation::AutoLayout, {}, automaticRevision);
        CHECK(view.revision == automaticRevision);
        const DecisionNodePlacement moved{node, -84.5F, 247.25F};
        layout(DecisionLayoutOperation::Move, {moved}, automaticRevision);
        REQUIRE_FALSE(view.diagnostic.has_value());
        const auto positions = view.layout;
        REQUIRE(positions->front() == moved);
        CHECK(view.graph == graph);

        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::EditDecisionStateMachine,
                                   .stateMachineEdit = StateMachineEditRequest{instance,
                                                                               view.revision,
                                                                               StateMachineEditOperation::SetTerminal,
                                                                               node,
                                                                               {},
                                                                               AI::DecisionStateTerminal::Succeeded}});
        REQUIRE_FALSE(view.diagnostic.has_value());
        CHECK(view.graph != graph);
        controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Undo));
        CHECK(view.graph == graph);
        CHECK(view.layout == positions);
        controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Undo));
        CHECK(view.layout == automatic);
        controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Redo));
        CHECK(view.layout == positions);
        CHECK(controller.HasDirtySourceDocuments());

        controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Save));
        REQUIRE_FALSE(view.diagnostic.has_value());
        CHECK_FALSE(controller.HasDirtySourceDocuments());
        const auto unchanged = Assets::CaptureAssetSource(project.root, sourcePath, AI::DecisionCodecLimits{}.maximumBytes);
        REQUIRE(unchanged.HasValue());
        CHECK(unchanged.Value().identity == originalBytes.Value().identity);
        const auto durable = CaptureDecisionLayout(project.root, sourcePath, asset, graph->asset);
        REQUIRE(durable.HasValue());
        CHECK(durable.Value().accepted.exists);
        CHECK(durable.Value().layout == *positions);
        CHECK(CaptureDecisionLayout(project.root, sourcePath, Assets::AssetId::FromBytes({9}), graph->asset).HasError());

        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = instance});
        REQUIRE(documents.empty());
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::RenameContentBrowserEntry,
                                   .stringPayload = WorkspacePathText(sourcePath),
                                   .secondaryStringPayload = "Canvas moved.horo_sm"});
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        CHECK_FALSE(std::filesystem::exists(DecisionLayoutPath(sourcePath)));
        controller.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::OpenSourceFile,
             .sourceOpenRequest = SourceOpenRequest{.path = "assets/Canvas moved.horo_sm", .origin = SourceOpenOrigin::AssetActivation}});
        REQUIRE(documents.size() == 1);
        CHECK(documents.front().asset == asset);
        CHECK(documents.front().layoutWritable);
        CHECK(*documents.front().layout == *positions);
        CHECK_FALSE(documents.front().canUndo);
    }

    SECTION("layout publication rejects a new external sidecar without dropping the draft") {
        controller.ProcessCommand(*command);
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        const auto &view = controller.ViewModel().decisionGraphDocuments.front();
        const auto node = std::get<AI::StateMachineAssetDescriptor>(view.graph->body).initial;
        const auto instance = view.identity.instance;
        const auto metadata = DecisionLayoutPath(project.root / path);
        controller.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::EditDecisionLayout,
             .decisionLayoutEdit = DecisionGraphLayoutRequest{{instance, view.revision}, DecisionLayoutOperation::Move, {{node, 4, 8}}}});
        REQUIRE(view.dirty);
        const auto local = view.layout;
        {
            std::ofstream file(metadata, std::ios::binary);
            file << "{}";
            REQUIRE(static_cast<bool>(file));
        }
        const auto external = Assets::CaptureAssetSource(project.root, metadata, MaximumDecisionLayoutBytes);
        REQUIRE(external.HasValue());
        controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Save));
        CHECK(view.conflict);
        CHECK(view.dirty);
        CHECK(view.layout == local);
        const auto preserved = Assets::CaptureAssetSource(project.root, metadata, MaximumDecisionLayoutBytes);
        REQUIRE(preserved.HasValue());
        CHECK(preserved.Value().identity == external.Value().identity);
    }

    SECTION("tracked save receipts retain the last durable graph when layout recording fails") {
        controller.ProcessCommand(*command);
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        REQUIRE(controller.ViewModel().decisionGraphDocuments.size() == 1);
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument,
                                   .documentInstance = controller.ViewModel().decisionGraphDocuments.front().identity.instance});
        REQUIRE(controller.ViewModel().decisionGraphDocuments.empty());

        DecisionSaveFaultFiles files;
        files.graph = project.root / path;
        files.layout = DecisionLayoutPath(files.graph);
        ProjectMutationCoordinator mutations{files};
        EditorWorkspaceController faulted(project.root, project.runtime, project.registry.Snapshot(),
                                          {.assets = {.mutableAssetRegistry = &project.registry},
                                           .mutations = &mutations,
                                           .durableFiles = &files});
        faulted.ProcessCommand({.command = EditorWorkspaceViewCommand::OpenSourceFile,
                                .sourceOpenRequest = SourceOpenRequest{.path = path, .origin = SourceOpenOrigin::AssetActivation}});
        REQUIRE(faulted.ViewModel().decisionGraphDocuments.size() == 1);
        const auto &view = faulted.ViewModel().decisionGraphDocuments.front();
        REQUIRE(view.editable);
        REQUIRE(view.layoutWritable);
        const auto original = view.source;
        const auto initial = std::get<AI::StateMachineAssetDescriptor>(view.graph->body).initial;
        const auto save = [&] {
            faulted.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Save));
        };
        files.leases.clear();
        save();
        CHECK(files.leases.empty());
        CHECK(files.graphWrites == 0);
        CHECK(files.layoutWrites == 0);

        faulted.ProcessCommand({.command = EditorWorkspaceViewCommand::EditDecisionStateMachine,
                                .stateMachineEdit = StateMachineEditRequest{view.identity.instance,
                                                                            view.revision,
                                                                            StateMachineEditOperation::SetTerminal,
                                                                            initial,
                                                                            {},
                                                                            AI::DecisionStateTerminal::Succeeded}});
        REQUIRE_FALSE(view.diagnostic.has_value());
        faulted.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::EditDecisionLayout,
             .decisionLayoutEdit =
                 DecisionGraphLayoutRequest{{view.identity.instance, view.revision}, DecisionLayoutOperation::Move, {{initial, 12, 18}}}});
        REQUIRE_FALSE(view.diagnostic.has_value());
        REQUIRE(view.dirty);
        const auto positions = view.layout;

        SECTION("precommit layout refusal is retryable without replacing the accepted graph again") {
            files.failure = DecisionSaveFaultFiles::Failure::BeforeLayoutReplace;
        }
        SECTION("a committed layout with unconfirmed durability keeps its error and requires reconciliation") {
            files.failure = DecisionSaveFaultFiles::Failure::AfterLayoutReplace;
        }
        const bool committed = files.failure == DecisionSaveFaultFiles::Failure::AfterLayoutReplace;
        files.leases.clear();
        save();
        const auto sourceLock = Assets::AssetSourcePublicationLockPath(project.root, files.graph);
        const auto layoutLock = Assets::AssetSourcePublicationLockPath(project.root, files.layout);
        REQUIRE(sourceLock.HasValue());
        REQUIRE(layoutLock.HasValue());
        REQUIRE(files.leases.size() == 3);
        CHECK(files.leases[0] == project.root / ".horo/local/project-mutation.lock");
        CHECK(files.leases[1] == sourceLock.Value());
        CHECK(files.leases[2] == layoutLock.Value());
        REQUIRE(view.diagnostic.has_value());
        const auto &expected = committed ? Assets::AssetSourceErrors::DurabilityUnconfirmed : Assets::AssetSourceErrors::ReadFailed;
        CHECK(view.diagnostic->code.Value() == expected.code.Value());
        CHECK(view.diagnostic->message == (committed ? DecisionSaveFaultFiles::AfterMessage : DecisionSaveFaultFiles::BeforeMessage));
        CHECK(view.conflict == committed);
        CHECK(view.dirty);
        CHECK(faulted.HasDirtySourceDocuments());
        CHECK(view.layout == positions);
        CHECK(files.graphWrites == 1);
        CHECK(files.layoutWrites == 1);
        const auto graph = Assets::CaptureAssetSource(project.root, files.graph, AI::DecisionCodecLimits{}.maximumBytes);
        REQUIRE(graph.HasValue());
        CHECK(graph.Value().identity.digest == view.source);
        CHECK(view.source != original);
        CHECK(std::filesystem::exists(files.layout) == committed);

        files.failure = DecisionSaveFaultFiles::Failure::None;
        save();
        CHECK(files.graphWrites == 1);
        if (committed) {
            REQUIRE(view.diagnostic.has_value());
            CHECK(view.diagnostic->code.Value() == Assets::AssetSourceErrors::Conflict.code.Value());
            CHECK(view.conflict);
            CHECK(view.dirty);
            CHECK(files.layoutWrites == 1);
        } else {
            CHECK_FALSE(view.diagnostic.has_value());
            CHECK_FALSE(view.conflict);
            CHECK_FALSE(view.dirty);
            CHECK_FALSE(faulted.HasDirtySourceDocuments());
            CHECK(files.layoutWrites == 2);
        }
        const auto durable = CaptureDecisionLayout(project.root, files.graph, view.asset, view.graph->asset);
        REQUIRE(durable.HasValue());
        CHECK(durable.Value().accepted.exists);
        CHECK(durable.Value().layout == *positions);
    }

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

TEST_CASE("Decision query output review fences writable keys and saved source before appending or editing a Task",
          "[unit][editor][decision][eqs][workspace]") {
    using namespace AI;
    BlackboardWorkspaceFixture project;
    Horo::Tests::WriteReplicationProject(project.root);
    const auto ungranted = BlackboardKeyId::Create(5).Value();
    const std::array keys{BlackboardKeyDescriptor{.key = project.key,
                                                  .kind = BlackboardValueKind::WorldCoordinate,
                                                  .presence = BlackboardKeyPresence::Optional},
                          BlackboardKeyDescriptor{.key = ungranted,
                                                  .kind = BlackboardValueKind::WorldCoordinate,
                                                  .presence = BlackboardKeyPresence::Optional}};
    const std::array rows{BlackboardSchemaAuthoringRow{project.key, "Cover position"}, BlackboardSchemaAuthoringRow{ungranted, "Other"}};
    project.WriteSchema(keys, rows);
    const auto manifest = Horo::Tests::WriteGameplayModuleManifest(project.root, HORO_TEST_AI_MODULE_PATH, "game.ai.tests",
                                                                   Gameplay::CurrentGameplayBuildFingerprint(),
                                                                   Horo::Tests::ReadDescriptorRevision(HORO_TEST_AI_MODULE_REVISION_PATH));
    REQUIRE(std::filesystem::is_regular_file(manifest));

    constexpr auto path = "assets/cover.horo_eqs";
    const auto asset = Assets::AssetId::FromBytes({8});
    const auto queryId = QueryId::Create(21).Value();
    const auto resultId = QueryResultSchemaId::Create(23).Value();
    const auto write = [&](const char *name) {
        const auto query = CreateBuiltinLatticeQuery(queryId, QueryStageId::Create(22).Value(), resultId, name,
                                                     {.stepMillimeters = 500, .outerSteps = 1}, 10);
        REQUIRE(query.HasValue());
        const auto bytes = EnvironmentQuerySourceCodec::Encode(query.Value());
        REQUIRE(bytes.HasValue());
        std::ofstream output(project.root / path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char *>(bytes.Value().data()), static_cast<std::streamsize>(bytes.Value().size()));
        REQUIRE(static_cast<bool>(output));
    };
    write("Cover");
    project.WriteSidecar(path, asset, EnvironmentQuerySourceAssetType);
    project.RebuildRegistry();
    const auto schema = CaptureBuiltinQuerySchema();
    REQUIRE(schema.HasValue());
    EditorWorkspaceController controller(project.root, project.runtime, project.registry.Snapshot(),
                                         {.assets = {.mutableAssetRegistry = &project.registry},
                                          .mutations = &project.mutations,
                                          .durableFiles = &project.files,
                                          .querySchema = &schema.Value()});
    const auto &model = controller.ViewModel();
    REQUIRE(model.decisionTaskChoices.size() == 1);
    const auto starter = model.decisionTaskChoices.front();
    controller.ProcessCommand(
        {.command = EditorWorkspaceViewCommand::CreateTaskDecision,
         .newTaskDecision = TaskDecisionCreationRequest{WorkspacePathText(project.root / "assets"), "Working query tree", project.asset,
                                                        DecisionPlanKind::BehaviorTree, starter.type, starter.provider, starter.version,
                                                        model.availableBehaviorsRevision}});
    REQUIRE(model.contentBrowserOperationError.empty());
    REQUIRE(model.decisionGraphDocuments.size() == 1);
    const auto &document = model.decisionGraphDocuments.front();
    REQUIRE(document.editable);
    const auto tree = [&]() -> const BehaviorTreeAsset & {
        return *std::get<std::shared_ptr<const BehaviorTreeAsset>>(document.graph->body);
    };
    const auto edit = [&](BehaviorTreeEditRequest request) {
        request.instance = document.identity.instance;
        request.revision = document.revision;
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::EditDecisionBehaviorTree, .behaviorTreeEdit = request});
    };
    using enum BehaviorTreeEditOperation;
    edit({.operation = WrapSequence, .node = DecisionNodeId::Create(tree().Edges().front().targetNode.Value()).Value()});
    REQUIRE_FALSE(document.diagnostic);
    const auto group = std::ranges::find(tree().Nodes(), BehaviorTreeNodeKind::Composite, &BehaviorTreeNode::kind);
    REQUIRE(group != tree().Nodes().end());
    const auto parent = DecisionNodeId::Create(group->id.Value()).Value();
    const auto choice = std::ranges::find(model.behaviorTreeTaskChoices, true, &DecisionTaskChoice::queryOutput);
    REQUIRE(choice != model.behaviorTreeTaskChoices.end());
    const DecisionTaskSelection selected{choice->type, choice->provider, choice->version, model.availableBehaviorsRevision};
    const auto review = [&](DecisionNodeId node, std::optional<DecisionTaskSelection> task = {}) {
        edit({.operation = ReviewQueryOutput, .node = node, .task = task, .queryOutput = BehaviorTreeQueryOutputEdit{asset}});
        REQUIRE_FALSE(document.diagnostic);
        REQUIRE(document.queryReview);
        REQUIRE(document.queryReview->keys.size() == 1);
        CHECK(document.queryReview->keys.front().first == project.key);
        CHECK(document.queryReview->keys.front().second == "Cover position");
    };
    review(parent, selected);
    const auto append = [&](BlackboardKeyId key) {
        edit({.operation = AppendTask, .node = parent, .task = selected, .queryOutput = BehaviorTreeQueryOutputEdit{asset, key, 12}});
    };
    const auto before = document.graph;
    append(ungranted);
    REQUIRE(document.diagnostic);
    CHECK(document.graph == before);
    write("Replaced cover");
    append(project.key);
    REQUIRE(document.diagnostic);
    CHECK(document.graph == before);
    review(parent, selected);
    append(project.key);
    REQUIRE_FALSE(document.diagnostic);
    const auto appended = std::ranges::find_if(tree().Nodes(), [&](const auto &node) {
        return node.type.Value() == selected.type.Value();
    });
    REQUIRE(appended != tree().Nodes().end());
    const auto node = appended->id;
    const auto output = [&] {
        const auto *task = tree().FindNode(node);
        REQUIRE(task);
        REQUIRE(task->properties.size() == 1);
        return std::get<BehaviorTreeQueryOutput>(task->properties.front().value);
    };
    CHECK(output().query == queryId);
    CHECK(output().result == resultId);
    CHECK(output().timeoutTicks == 12);
    const auto selectedNode = DecisionNodeId::Create(node.Value()).Value();
    review(selectedNode);
    edit({.operation = SetQueryOutput, .node = selectedNode, .queryOutput = BehaviorTreeQueryOutputEdit{asset, project.key, 18}});
    REQUIRE_FALSE(document.diagnostic);
    CHECK(output().timeoutTicks == 18);
    edit({.operation = Undo});
    CHECK(output().timeoutTicks == 12);
    edit({.operation = Redo});
    CHECK(output().timeoutTicks == 18);
    edit({.operation = Save});
    REQUIRE_FALSE(document.diagnostic);
    CHECK_FALSE(document.dirty);
    const auto expected = output();
    const auto savedPath = document.graph->location.source;
    controller.ProcessCommand(
        {.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = document.identity.instance});
    REQUIRE(model.decisionGraphDocuments.empty());
    controller.ProcessCommand({.command = EditorWorkspaceViewCommand::OpenSourceFile,
                               .sourceOpenRequest = SourceOpenRequest{.path = savedPath, .origin = SourceOpenOrigin::AssetActivation}});
    REQUIRE(model.decisionGraphDocuments.size() == 1);
    const auto &saved = *std::get<std::shared_ptr<const BehaviorTreeAsset>>(model.decisionGraphDocuments.front().graph->body);
    REQUIRE(saved.FindNode(node));
    CHECK(std::get<BehaviorTreeQueryOutput>(saved.FindNode(node)->properties.front().value) == expected);
}

TEST_CASE("Behavior Tree and Utility New use the current registered native task and selected saved schema",
          "[unit][editor][decision][workspace]") {
    BlackboardWorkspaceFixture project;
    Horo::Tests::WriteReplicationProject(project.root);
    project.Write("Score source", AI::BlackboardKeyPresence::Optional, AI::BlackboardValueKind::Scalar);
    const auto manifest = Horo::Tests::WriteGameplayModuleManifest(project.root, HORO_TEST_AI_MODULE_PATH, "game.ai.tests",
                                                                   Gameplay::CurrentGameplayBuildFingerprint(),
                                                                   Horo::Tests::ReadDescriptorRevision(HORO_TEST_AI_MODULE_REVISION_PATH));
    REQUIRE(std::filesystem::is_regular_file(manifest));
    BuildOutputStore diagnosticOutput{32};
    EditorWorkspaceController controller(project.root, project.runtime, project.registry.Snapshot(),
                                         {.assets = {.mutableAssetRegistry = &project.registry},
                                          .mutations = &project.mutations,
                                          .durableFiles = &project.files,
                                          .sourceNavigation = {.diagnosticOutput = &diagnosticOutput}});
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
    SECTION("Behavior Tree applies an exact registered property override with durable undo and redo") {
        controller.ProcessCommand(make(AI::DecisionPlanKind::BehaviorTree, "Editable task properties"));
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        const auto &model = controller.ViewModel();
        REQUIRE(model.decisionGraphDocuments.size() == 1);
        const auto &document = model.decisionGraphDocuments.front();
        const auto initial = document.graph;
        const auto &tree = *std::get<std::shared_ptr<const AI::BehaviorTreeAsset>>(initial->body);
        const auto leaf = tree.Edges().front().targetNode;
        const auto node = AI::DecisionNodeId::Create(leaf.Value()).Value();
        REQUIRE(tree.FindNode(leaf));
        CHECK(tree.FindNode(leaf)->properties.empty());
        const auto task = std::ranges::find(model.behaviorTreeTaskChoices, providerType, &DecisionTaskChoice::type);
        REQUIRE(task != model.behaviorTreeTaskChoices.end());
        REQUIRE(task->editableProperties.size() == 1);
        const auto declared = task->editableProperties.front();
        CHECK(declared.value == AI::BehaviorTreePropertyValue{false});
        const DecisionTaskSelection selected{task->type, task->provider, task->version, model.availableBehaviorsRevision};
        const auto initialRevision = document.revision;
        const auto edit = [&](AI::BehaviorTreePropertyId property, AI::BehaviorTreePropertyValue value, DecisionTaskSelection exact,
                              std::uint64_t revision) {
            controller.ProcessCommand(
                {.command = EditorWorkspaceViewCommand::EditDecisionBehaviorTree,
                 .behaviorTreeEdit = BehaviorTreeEditRequest{.instance = document.identity.instance,
                                                             .revision = revision,
                                                             .operation = BehaviorTreeEditOperation::SetTaskProperty,
                                                             .node = node,
                                                             .task = exact,
                                                             .property = BehaviorTreePropertyEdit{property, std::move(value)}}});
        };
        edit(declared.id, false, selected, document.revision);
        CHECK(document.graph == initial);
        CHECK(document.revision == initialRevision);
        CHECK_FALSE(document.canUndo);

        auto stale = selected;
        ++stale.catalogRevision;
        edit(declared.id, true, stale, document.revision);
        REQUIRE(document.diagnostic);
        edit(AI::BehaviorTreePropertyId::Create(1001).Value(), true, selected, document.revision);
        REQUIRE(document.diagnostic);
        edit(declared.id, std::int64_t{14}, selected, document.revision);
        REQUIRE(document.diagnostic);
        CHECK(document.graph == initial);
        CHECK_FALSE(document.canUndo);

        // This fixture's boolean makes its factory throw. Compilation and authoring consume inert metadata.
        edit(declared.id, true, selected, document.revision);
        REQUIRE_FALSE(document.diagnostic);
        REQUIRE(document.dirty);
        const auto edited = document.graph;
        const auto checkOverride = [&](const DecisionGraphWorkspaceDocument &current) {
            const auto &body = *std::get<std::shared_ptr<const AI::BehaviorTreeAsset>>(current.graph->body);
            const auto *bound = body.FindNode(leaf);
            REQUIRE(bound);
            REQUIRE(bound->properties.size() == 1);
            CHECK(bound->properties.front().id == declared.id);
            CHECK(bound->properties.front().value == AI::BehaviorTreePropertyValue{true});
        };
        checkOverride(document);
        edit(declared.id, false, selected, initialRevision);
        REQUIRE(document.diagnostic);
        CHECK(document.graph == edited);
        controller.ProcessCommand(MakeDecisionHistoryCommand(document, DecisionHistoryAction::Undo));
        REQUIRE_FALSE(document.diagnostic);
        CHECK(document.graph == initial);
        CHECK_FALSE(document.dirty);
        controller.ProcessCommand(MakeDecisionHistoryCommand(document, DecisionHistoryAction::Redo));
        REQUIRE_FALSE(document.diagnostic);
        checkOverride(document);
        controller.ProcessCommand(MakeDecisionHistoryCommand(document, DecisionHistoryAction::Save));
        REQUIRE_FALSE(document.diagnostic);
        CHECK_FALSE(document.dirty);
        const auto path = document.graph->location.source;
        controller.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = document.identity.instance});
        REQUIRE(model.decisionGraphDocuments.empty());
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::OpenSourceFile,
                                   .sourceOpenRequest = SourceOpenRequest{.path = path, .origin = SourceOpenOrigin::AssetActivation}});
        REQUIRE(model.decisionGraphDocuments.size() == 1);
        checkOverride(model.decisionGraphDocuments.front());
    }
    SECTION("Behavior Tree edits preserve reachability, installed identity, sibling order and durable history") {
        controller.ProcessCommand(make(AI::DecisionPlanKind::BehaviorTree, "Working tree"));
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        const auto &documents = controller.ViewModel().decisionGraphDocuments;
        REQUIRE(documents.size() == 1);
        const auto &view = documents.front();
        REQUIRE(view.editable);
        CHECK(view.family == AI::DecisionPlanKind::BehaviorTree);
        const auto original = view.graph;
        const auto asset = view.asset;
        const auto instance = view.identity.instance;
        const auto &start = *std::get<std::shared_ptr<const AI::BehaviorTreeAsset>>(original->body);
        REQUIRE(start.Edges().size() == 1);
        const auto originalEdge = start.Edges().front();
        const auto originalTask = originalEdge.targetNode;
        const DecisionTaskSelection installed{provider->type, provider->provider, provider->version,
                                              controller.ViewModel().availableBehaviorsRevision};
        const auto edit = [&](BehaviorTreeEditOperation operation, AI::BehaviorTreeNodeId node,
                              std::optional<DecisionTaskSelection> task = {}) {
            controller.ProcessCommand(
                {.command = EditorWorkspaceViewCommand::EditDecisionBehaviorTree,
                 .behaviorTreeEdit =
                     BehaviorTreeEditRequest{instance, view.revision, operation, AI::DecisionNodeId::Create(node.Value()).Value(), task}});
        };
        const auto graph = [&]() -> const AI::BehaviorTreeAsset & {
            return *std::get<std::shared_ptr<const AI::BehaviorTreeAsset>>(view.graph->body);
        };
        using enum BehaviorTreeEditOperation;
        edit(WrapSelector, start.Root());
        CHECK(view.diagnostic.has_value());
        edit(DeleteTask, originalTask);
        CHECK(view.diagnostic.has_value());
        CHECK(view.graph == original);
        CHECK_FALSE(view.canUndo);

        edit(WrapSelector, originalTask);
        REQUIRE_FALSE(view.diagnostic.has_value());
        REQUIRE(graph().Nodes().size() == 3);
        CHECK(std::ranges::any_of(graph().Nodes(), [](const auto &node) {
            return node.control.kind == AI::DecisionControlKind::Selector;
        }));
        controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Undo));
        CHECK(view.graph == original);
        CHECK_FALSE(view.dirty);

        edit(WrapSequence, originalTask);
        REQUIRE_FALSE(view.diagnostic.has_value());
        const auto wrapped = view.graph;
        REQUIRE(graph().Nodes().size() == 3);
        const auto inbound = std::ranges::find(graph().Edges(), originalEdge.id, &AI::BehaviorTreeEdge::id);
        REQUIRE(inbound != graph().Edges().end());
        CHECK(inbound->order == originalEdge.order);
        const auto sequence = inbound->targetNode;
        REQUIRE(graph().FindNode(sequence));
        CHECK(graph().FindNode(sequence)->control.kind == AI::DecisionControlKind::Sequence);

        auto stale = installed;
        ++stale.catalogRevision;
        edit(AppendTask, sequence, stale);
        CHECK(view.diagnostic.has_value());
        CHECK(view.graph == wrapped);
        stale = installed;
        stale.provider = AI::DecisionProviderId::Create(987).Value();
        edit(AppendTask, sequence, stale);
        CHECK(view.diagnostic.has_value());
        CHECK(view.graph == wrapped);
        edit(AppendTask, sequence, installed);
        REQUIRE_FALSE(view.diagnostic.has_value());
        const auto appended = view.graph;
        REQUIRE(graph().Nodes().size() == 4);
        const auto extra = std::ranges::find_if(graph().Edges(), [&](const auto &edge) {
            return edge.sourceNode == sequence && edge.targetNode != originalTask;
        });
        REQUIRE(extra != graph().Edges().end());
        const auto task = extra->targetNode;
        const auto extraEdge = extra->id;
        REQUIRE(graph().FindNode(task));
        CHECK(graph().FindNode(task)->type.Value() == installed.type.Value());
        CHECK(graph().FindNode(task)->typeVersion.major == installed.version);
        CHECK(graph().FindNode(task)->task != graph().FindNode(originalTask)->task);
        CHECK(extra->order == 1);

        edit(MoveEarlier, task);
        REQUIRE_FALSE(view.diagnostic.has_value());
        const auto moved = view.graph;
        const auto first = std::ranges::find(graph().Edges(), sequence, &AI::BehaviorTreeEdge::sourceNode);
        REQUIRE(first != graph().Edges().end());
        CHECK(first->id == extraEdge);
        const DecisionGraphDocumentFence canvasFence{instance, view.revision};
        const auto projection = ProjectDecisionCanvas(*view.graph, canvasFence, view.editable);
        CHECK(projection.family == AI::DecisionPlanKind::BehaviorTree);
        CHECK(projection.nodes.size() == graph().Nodes().size());
        CHECK(projection.links.size() == graph().Edges().size());
        const auto link = std::ranges::find(projection.links, extraEdge.Value(), &GraphLinkView::authoredId);
        REQUIRE(link != projection.links.end());
        CHECK(link->role == GraphLinkRole::Child);
        CHECK(projection.pins[link->from].authoredPin == first->sourcePin.Value());
        CHECK(projection.pins[link->to].authoredPin == first->targetPin.Value());
        const auto node = std::ranges::find(projection.nodes, projection.pins[link->to].node, &GraphNodeView::id);
        REQUIRE(node != projection.nodes.end());
        CHECK(node->incomingOrder == 0);
        CHECK_FALSE(ResolveDecisionCanvasEdit(projection, {canvasFence, GraphRemoveLinkIntent{link->role, link->authoredId}}));
        const auto beforeNoop = view.revision;
        edit(MoveEarlier, task);
        CHECK(view.graph == moved);
        CHECK(view.revision == beforeNoop);
        controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Undo));
        REQUIRE_FALSE(view.diagnostic.has_value());
        CHECK(view.graph == appended);
        controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Redo));
        REQUIRE_FALSE(view.diagnostic.has_value());
        CHECK(view.graph == moved);

        edit(DeleteTask, originalTask);
        REQUIRE_FALSE(view.diagnostic.has_value());
        CHECK_FALSE(graph().FindNode(originalTask));
        edit(UnwrapComposite, sequence);
        REQUIRE_FALSE(view.diagnostic.has_value());
        REQUIRE(graph().Edges().size() == 1);
        CHECK(graph().Edges().front().id == originalEdge.id);
        CHECK(graph().Edges().front().targetNode == task);
        controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Undo));
        controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Undo));
        CHECK(view.graph == moved);
        REQUIRE(controller.HasDirtySourceDocuments());
        controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Save));
        REQUIRE_FALSE(view.diagnostic.has_value());
        CHECK_FALSE(controller.HasDirtySourceDocuments());
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = instance});
        REQUIRE(documents.empty());

        controller.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::OpenSourceFile,
             .sourceOpenRequest = SourceOpenRequest{.path = "assets/Working tree.horo_bt", .origin = SourceOpenOrigin::AssetActivation}});
        REQUIRE(documents.size() == 1);
        CHECK(documents.front().asset == asset);
        REQUIRE(documents.front().editable);
        CHECK(documents.front().family == AI::DecisionPlanKind::BehaviorTree);
        const auto &reopened = *std::get<std::shared_ptr<const AI::BehaviorTreeAsset>>(documents.front().graph->body);
        REQUIRE(reopened.FindNode(sequence));
        REQUIRE(reopened.FindNode(task));
        CHECK(reopened.FindNode(sequence)->control.kind == AI::DecisionControlKind::Sequence);
        const auto ordered = std::ranges::find(reopened.Edges(), sequence, &AI::BehaviorTreeEdge::sourceNode);
        REQUIRE(ordered != reopened.Edges().end());
        CHECK(ordered->id == extraEdge);
        CHECK(ordered->targetNode == task);
        CHECK_FALSE(documents.front().canUndo);
    }
    SECTION("subtree authoring reviews saved targets and compiles the actual reachable closure") {
        using enum BehaviorTreeEditOperation;
        const auto &documents = controller.ViewModel().decisionGraphDocuments;
        const auto active = [&]() -> const DecisionGraphWorkspaceDocument & {
            const auto selected = controller.ViewModel().workspacePanelHost.ActiveDocument();
            const auto result = std::ranges::find_if(documents, [&](const auto &doc) {
                return doc.identity.instance == selected;
            });
            REQUIRE(result != documents.end());
            return *result;
        };
        const auto create = [&](const char *name) {
            controller.ProcessCommand(make(AI::DecisionPlanKind::BehaviorTree, name));
            REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
            return active().asset;
        };
        const auto parent = create("Subtree caller");
        const auto child = create("Subtree target");
        const auto alternative = create("Subtree alternate");
        const auto open = [&](std::string_view path) {
            controller.ProcessCommand(
                {.command = EditorWorkspaceViewCommand::OpenSourceFile,
                 .sourceOpenRequest = SourceOpenRequest{.path = std::filesystem::path{path}, .origin = SourceOpenOrigin::AssetActivation}});
            REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        };
        const auto edit = [&](BehaviorTreeEditRequest request) {
            request.instance = active().identity.instance;
            request.revision = active().revision;
            controller.ProcessCommand({.command = EditorWorkspaceViewCommand::EditDecisionBehaviorTree, .behaviorTreeEdit = request});
        };
        const auto tree = [&]() -> const AI::BehaviorTreeAsset & {
            return *std::get<std::shared_ptr<const AI::BehaviorTreeAsset>>(active().graph->body);
        };
        const auto group = [&] {
            const auto root = tree().Root();
            const auto edge = std::ranges::find(tree().Edges(), root, &AI::BehaviorTreeEdge::sourceNode);
            REQUIRE(edge != tree().Edges().end());
            edit({.operation = WrapSequence, .node = AI::DecisionNodeId::Create(edge->targetNode.Value()).Value()});
            REQUIRE_FALSE(active().diagnostic.has_value());
            const auto first = std::ranges::find(tree().Edges(), root, &AI::BehaviorTreeEdge::sourceNode);
            REQUIRE(first != tree().Edges().end());
            return AI::DecisionNodeId::Create(first->targetNode.Value()).Value();
        };
        const auto review = [&](Assets::AssetId asset) {
            const auto revision = active().revision;
            edit({.operation = ReviewSubtree, .subtreeTarget = asset});
            REQUIRE_FALSE(active().diagnostic.has_value());
            REQUIRE(active().subtreeReview.has_value());
            CHECK(active().revision == revision);
            return *active().subtreeReview;
        };

        open("assets/Subtree caller.horo_bt");
        REQUIRE(active().asset == parent);
        const auto parentGroup = group();
        edit({.operation = ReviewSubtree, .subtreeTarget = Assets::AssetId::FromBytes({97})});
        CHECK(active().diagnostic.has_value());
        CHECK_FALSE(active().subtreeReview);
        edit({.operation = ReviewSubtree, .subtreeTarget = parent});
        CHECK(active().diagnostic.has_value());

        const auto savedTarget = review(child);
        const auto childFile = project.root / "assets/Subtree target.horo_bt";
        const auto captured = Assets::CaptureAssetSource(project.root, childFile, AI::DecisionCodecLimits{}.maximumBytes);
        REQUIRE(captured.HasValue());
        const auto write = [&](const std::filesystem::path &file, std::span<const std::byte> bytes) {
            std::ofstream output(file, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            REQUIRE(static_cast<bool>(output));
        };
        const auto targetDocument = std::ranges::find(documents, child, &DecisionGraphWorkspaceDocument::asset);
        REQUIRE(targetDocument != documents.end());
        auto changedSource = *targetDocument->graph;
        auto changedTree = std::get<std::shared_ptr<const AI::BehaviorTreeAsset>>(changedSource.body)->Data();
        changedTree.displayName = "Changed after review";
        auto rebound = AI::BehaviorTreeAsset::Capture(std::move(changedTree));
        REQUIRE(rebound.HasValue());
        changedSource.body = std::make_shared<const AI::BehaviorTreeAsset>(std::move(rebound).Value());
        const auto obsolete = AI::DecisionPlanCodec::EncodeSource(std::span{&changedSource, 1}, changedSource.asset);
        REQUIRE(obsolete.HasValue());
        write(childFile, std::as_bytes(std::span{obsolete.Value()}));
        const auto before = active().graph;
        const auto beforeRevision = active().revision;
        edit({.operation = AppendSubtree, .node = parentGroup, .subtree = savedTarget});
        REQUIRE(active().diagnostic.has_value());
        CHECK(active().diagnostic->code.Value() == Application::DecisionCookErrors::Stale.code.Value());
        CHECK(active().graph == before);
        CHECK(active().revision == beforeRevision);
        write(childFile, captured.Value().bytes);

        const auto fresh = review(child);
        const auto schemaFile = project.root / project.Path;
        const auto savedSchema = Assets::CaptureAssetSource(project.root, schemaFile, AI::BlackboardSchemaSourceCodec::MaximumBytes);
        REQUIRE(savedSchema.HasValue());
        project.Write("Changed Blackboard label", AI::BlackboardKeyPresence::Optional, AI::BlackboardValueKind::Scalar);
        edit({.operation = AppendSubtree, .node = parentGroup, .subtree = fresh});
        REQUIRE(active().diagnostic.has_value());
        CHECK(active().diagnostic->code.Value() == Application::DecisionCookErrors::Stale.code.Value());
        CHECK(active().graph == before);
        CHECK(active().revision == beforeRevision);
        write(schemaFile, savedSchema.Value().bytes);

        const auto confirmed = review(child);
        auto forged = confirmed;
        ++forged.catalogRevision;
        edit({.operation = AppendSubtree, .node = parentGroup, .subtree = forged});
        REQUIRE(active().diagnostic.has_value());
        CHECK(active().diagnostic->code.Value() == Application::DecisionCookErrors::Stale.code.Value());
        CHECK(active().graph == before);
        edit({.operation = AppendSubtree, .node = parentGroup, .subtree = confirmed});
        REQUIRE_FALSE(active().diagnostic.has_value());
        const auto added = active().graph;
        const auto call = std::ranges::find_if(tree().Nodes(), [](const auto &node) {
            return node.call.has_value();
        });
        REQUIRE(call != tree().Nodes().end());
        CHECK(call->call->asset == confirmed.graph);
        CHECK(call->call->schema.Contains(confirmed.graphVersion));
        const auto callId = call->id;
        const auto projection = ProjectDecisionCanvas(*active().graph, {active().identity.instance, active().revision}, true);
        const auto visible = std::ranges::find_if(projection.nodes, [&](const auto &node) {
            return node.id.Value() == callId.Value();
        });
        REQUIRE(visible != projection.nodes.end());
        CHECK(visible->role == GraphNodeRole::Subtree);

        const auto redirect = review(alternative);
        edit({.operation = RetargetSubtree, .node = AI::DecisionNodeId::Create(callId.Value()).Value(), .subtree = redirect});
        REQUIRE_FALSE(active().diagnostic.has_value());
        REQUIRE(tree().FindNode(callId)->call.has_value());
        CHECK(tree().FindNode(callId)->call->asset == redirect.graph);
        controller.ProcessCommand(MakeDecisionHistoryCommand(active(), DecisionHistoryAction::Undo));
        CHECK(active().graph == added);
        controller.ProcessCommand(MakeDecisionHistoryCommand(active(), DecisionHistoryAction::Undo));
        CHECK(active().graph == before);
        controller.ProcessCommand(MakeDecisionHistoryCommand(active(), DecisionHistoryAction::Redo));
        CHECK(active().graph == added);
        controller.ProcessCommand(MakeDecisionHistoryCommand(active(), DecisionHistoryAction::Save));
        REQUIRE_FALSE(active().diagnostic.has_value());
        CHECK_FALSE(active().dirty);

        open("assets/Subtree target.horo_bt");
        const auto childGroup = group();
        const auto reverse = review(parent);
        const auto noCycle = active().graph;
        edit({.operation = AppendSubtree, .node = childGroup, .subtree = reverse});
        REQUIRE(active().diagnostic.has_value());
        CHECK(active().diagnostic->code.Value() == AI::AIErrors::DecisionAssetDependencyCycle.code.Value());
        CHECK(active().graph == noCycle);
        const auto &findings = active().diagnostic->diagnostics;
        REQUIRE_FALSE(findings.empty());
        const auto output = diagnosticOutput.SnapshotIfChanged(0);
        REQUIRE(output);
        const auto record = std::ranges::find_if(output->records, [](const BuildOutputRecord &entry) {
            return entry.code.Value() == AI::AIErrors::DecisionAssetDependencyCycle.code.Value() && entry.source.has_value();
        });
        REQUIRE(record != output->records.end());
        CHECK(record->stage == "ai.decision_validation");
        CHECK(record->severity == DiagnosticSeverity::Error);
        const auto finding = std::ranges::find_if(findings, [&](const Diagnostic &entry) {
            return entry.code.Value() == record->code.Value() &&
                   WorkspacePathText(project.root / WorkspaceNativePath(entry.location.source)) == record->source->absolutePath;
        });
        REQUIRE(finding != findings.end());
        CHECK(record->message == finding->message);
        CHECK(record->source->line == finding->location.line);
        CHECK(record->source->column == finding->location.column);

        open("assets/Subtree caller.horo_bt");
        const auto callerInstance = active().identity.instance;
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = callerInstance});
        open("assets/Subtree caller.horo_bt");
        REQUIRE(active().editable);
        const auto *reopened = tree().FindNode(callId);
        REQUIRE(reopened);
        REQUIRE(reopened->call.has_value());
        CHECK(reopened->call->asset == confirmed.graph);
        CHECK_FALSE(active().canUndo);
        controller.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = active().identity.instance});
        REQUIRE(std::filesystem::remove(childFile));
        open("assets/Subtree caller.horo_bt");
        CHECK_FALSE(active().editable);
        CHECK(active().diagnostic.has_value());
        REQUIRE(tree().FindNode(callId));
    }
    SECTION("Utility authoring edits exact saved scalar scores and preserves canonical option identities") {
        const auto secondKey = AI::BlackboardKeyId::Create(5).Value();
        const auto wrongKind = AI::BlackboardKeyId::Create(6).Value();
        const std::array keys{
            AI::BlackboardKeyDescriptor{.key = project.key,
                                        .kind = AI::BlackboardValueKind::Scalar,
                                        .presence = AI::BlackboardKeyPresence::Optional},
            AI::BlackboardKeyDescriptor{.key = secondKey,
                                        .kind = AI::BlackboardValueKind::Scalar,
                                        .presence = AI::BlackboardKeyPresence::Optional},
            AI::BlackboardKeyDescriptor{.key = wrongKind,
                                        .kind = AI::BlackboardValueKind::Boolean,
                                        .presence = AI::BlackboardKeyPresence::Optional},
        };
        const std::array rows{AI::BlackboardSchemaAuthoringRow{project.key, "Range"}, AI::BlackboardSchemaAuthoringRow{secondKey, "Risk"},
                              AI::BlackboardSchemaAuthoringRow{wrongKind, "Flag"}};
        project.WriteSchema(keys, rows);
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::BlackboardDocument,
                                   .blackboard = BlackboardWorkspaceRequest{.fence = project.Current(controller),
                                                                            .operation = BlackboardDocumentOperation::Reload}});
        const auto &schema = project.Snapshot(controller);
        REQUIRE(schema.sourceDigest.has_value());
        REQUIRE(schema.source->rows.size() == keys.size());
        const auto digest = *schema.sourceDigest;
        controller.ProcessCommand(make(AI::DecisionPlanKind::Utility, "Working utility"));
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        const auto &documents = controller.ViewModel().decisionGraphDocuments;
        REQUIRE(documents.size() == 1);
        const auto &view = documents.front();
        REQUIRE(view.editable);
        CHECK(view.family == AI::DecisionPlanKind::Utility);
        const auto original = view.graph;
        const auto instance = view.identity.instance;
        const auto asset = view.asset;
        const auto utility = [&]() -> const AI::UtilityDecisionSource & {
            return std::get<AI::UtilityDecisionSource>(view.graph->body);
        };
        REQUIRE(utility().options.size() == 1);
        const auto firstId = utility().options.front().option.id;
        const auto firstTask = utility().options.front().option.action;
        const DecisionTaskSelection installed{provider->type, provider->provider, provider->version,
                                              controller.ViewModel().availableBehaviorsRevision};
        const auto edit = [&](UtilityGraphEditRequest request) {
            request.instance = instance;
            request.revision = view.revision;
            controller.ProcessCommand({.command = EditorWorkspaceViewCommand::EditDecisionUtilityGraph, .utilityGraphEdit = request});
        };
        const UtilityScoreEdit risk{{.key = secondKey,
                                     .minimum = -3,
                                     .maximum = 7,
                                     .weight = 0.625,
                                     .curve = AI::UtilityResponseCurve::Step,
                                     .parameter = 0.5,
                                     .invert = true},
                                    digest};
        using enum UtilityGraphEditOperation;
        edit({.operation = DeleteOption, .option = firstId});
        CHECK(view.diagnostic.has_value());
        edit({.operation = DeleteConsideration, .option = firstId, .slot = UtilityConsiderationSlot{0, project.key}});
        CHECK(view.diagnostic.has_value());
        auto invalid = risk;
        invalid.score.key = wrongKind;
        edit({.operation = AddConsideration, .option = firstId, .score = invalid});
        CHECK(view.diagnostic.has_value());
        REQUIRE(view.validation);
        const auto navigate = [&](std::uint64_t revision, AI::BlackboardKeyId key) {
            controller.ProcessCommand({.command = EditorWorkspaceViewCommand::OpenDecisionDiagnosticKey,
                                       .decisionDiagnosticKey = DecisionDiagnosticKeyRequest{{instance, revision}, project.schema, key}});
        };
        navigate(view.revision, project.key);
        CHECK(controller.ViewModel().workspacePanelHost.ActiveDocument() == instance);
        CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.decision_graph.diagnostic_key_unavailable");
        navigate(view.revision + 1, wrongKind);
        CHECK(controller.ViewModel().workspacePanelHost.ActiveDocument() == instance);
        navigate(view.revision, wrongKind);
        const auto &blackboard = controller.ViewModel().blackboardDocuments.front();
        CHECK(controller.ViewModel().contentBrowserOperationError.empty());
        CHECK(controller.ViewModel().workspacePanelHost.ActiveDocument() == blackboard.snapshot.document.instance);
        CHECK(blackboard.diagnosticKey == wrongKind);
        CHECK(blackboard.diagnosticKeyRequest == 1);
        CHECK(view.graph == original);
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::OpenSourceFile,
                                   .sourceOpenRequest = SourceOpenRequest{.path = "assets/Working utility.horo_utility",
                                                                          .origin = SourceOpenOrigin::AssetActivation}});
        REQUIRE(controller.ViewModel().workspacePanelHost.ActiveDocument() == instance);
        const auto missingKey = AI::BlackboardKeyId::Create(99).Value();
        invalid.score.key = missingKey;
        edit({.operation = AddConsideration, .option = firstId, .score = invalid});
        REQUIRE(view.validation);
        navigate(view.revision, missingKey);
        CHECK(controller.ViewModel().workspacePanelHost.ActiveDocument() == instance);
        CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.decision_graph.diagnostic_key_missing");
        invalid = risk;
        invalid.schemaSource = {};
        edit({.operation = AddConsideration, .option = firstId, .score = invalid});
        CHECK(view.diagnostic.has_value());
        CHECK(view.graph == original);
        CHECK_FALSE(view.canUndo);

        edit({.operation = AddConsideration, .option = firstId, .score = risk});
        REQUIRE_FALSE(view.diagnostic.has_value());
        REQUIRE(utility().options.front().option.considerations.size() == 2);
        REQUIRE(utility().requirements.size() == 2);
        CHECK(std::ranges::any_of(utility().requirements, [&](const auto &binding) {
            return binding.key == secondKey;
        }));
        const auto appended = view.graph;
        edit({.operation = SetConsideration, .option = firstId, .score = risk, .slot = UtilityConsiderationSlot{0, wrongKind}});
        CHECK(view.diagnostic.has_value());
        CHECK(view.graph == appended);
        edit({.operation = SetConsideration, .option = firstId, .score = risk, .slot = UtilityConsiderationSlot{0, project.key}});
        REQUIRE_FALSE(view.diagnostic.has_value());
        REQUIRE(utility().requirements.size() == 1);
        CHECK(utility().requirements.front().key == secondKey);

        auto staleTask = installed;
        ++staleTask.catalogRevision;
        const auto scoresEdited = view.graph;
        edit({.operation = AddOption, .task = staleTask, .score = risk});
        CHECK(view.diagnostic.has_value());
        CHECK(view.graph == scoresEdited);
        edit({.operation = AddOption, .task = installed, .score = risk, .priority = 0.75, .compensation = 0.25});
        REQUIRE_FALSE(view.diagnostic.has_value());
        REQUIRE(utility().options.size() == 2);
        const auto second = std::ranges::find_if(utility().options, [&](const auto &entry) {
            return entry.option.id != firstId;
        });
        REQUIRE(second != utility().options.end());
        const auto secondId = second->option.id;
        const auto secondTask = second->option.action;
        CHECK(secondTask != firstTask);
        CHECK(second->provider.id == secondId);
        CHECK(second->provider.type == installed.type);
        const DecisionGraphDocumentFence canvasFence{instance, view.revision};
        const auto projection = ProjectDecisionCanvas(*view.graph, canvasFence, view.editable);
        CHECK(projection.family == AI::DecisionPlanKind::Utility);
        REQUIRE(projection.nodes.size() == 3);
        REQUIRE(projection.links.size() == 2);
        const auto association = std::ranges::find(projection.links, secondId.Value(), &GraphLinkView::authoredId);
        REQUIRE(association != projection.links.end());
        CHECK(association->role == GraphLinkRole::UtilityOption);
        CHECK(projection.pins[association->from].node == utility().root);
        CHECK(projection.pins[association->to].node == secondId);
        CHECK_FALSE(
            ResolveDecisionCanvasEdit(projection, {canvasFence, GraphRemoveLinkIntent{association->role, association->authoredId}}));
        edit({.operation = SetOptionPolicy, .option = secondId, .priority = 1.25});
        CHECK(view.diagnostic.has_value());
        edit({.operation = SetOptionPolicy, .option = secondId, .priority = 0.5, .compensation = 0.125});
        REQUIRE_FALSE(view.diagnostic.has_value());
        const auto edited = view.graph;
        edit({.operation = DeleteOption, .option = secondId});
        REQUIRE_FALSE(view.diagnostic.has_value());
        CHECK(utility().options.size() == 1);
        controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Undo));
        CHECK(view.graph == edited);
        controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Redo));
        CHECK(utility().options.size() == 1);
        controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Undo));
        CHECK(view.graph == edited);
        edit({.operation = DeleteConsideration, .option = firstId, .slot = UtilityConsiderationSlot{1, secondKey}});
        REQUIRE_FALSE(view.diagnostic.has_value());
        REQUIRE(controller.HasDirtySourceDocuments());
        controller.ProcessCommand(MakeDecisionHistoryCommand(view, DecisionHistoryAction::Save));
        REQUIRE_FALSE(view.diagnostic.has_value());
        CHECK_FALSE(controller.HasDirtySourceDocuments());
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = instance});
        REQUIRE(documents.empty());

        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::OpenSourceFile,
                                   .sourceOpenRequest = SourceOpenRequest{.path = "assets/Working utility.horo_utility",
                                                                          .origin = SourceOpenOrigin::AssetActivation}});
        REQUIRE(documents.size() == 1);
        CHECK(documents.front().asset == asset);
        CHECK(documents.front().editable);
        const auto &reopened = std::get<AI::UtilityDecisionSource>(documents.front().graph->body);
        CHECK(reopened.root == std::get<AI::UtilityDecisionSource>(original->body).root);
        REQUIRE(reopened.options.size() == 2);
        const auto finished = std::ranges::find_if(reopened.options, [&](const auto &entry) {
            return entry.option.id == secondId;
        });
        REQUIRE(finished != reopened.options.end());
        CHECK(finished->option.action == secondTask);
        CHECK(finished->option.priority == 0.5);
        CHECK(finished->option.compensation == 0.125);
        REQUIRE(finished->option.considerations.size() == 1);
        const auto &score = finished->option.considerations.front();
        CHECK(score.key == secondKey);
        CHECK(score.curve == AI::UtilityResponseCurve::Step);
        CHECK(score.weight == 0.625);
        CHECK(score.invert);
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
    auto sequence = TreeNode(2, AI::DecisionControlKind::Parallel);
    sequence.control.kind = AI::DecisionControlKind::Sequence;
    auto compatible = TreeNode(3, AI::DecisionControlKind::Task);
    compatible.type = MakeIdentity<AI::BehaviorTreeNodeTypeId>(101);
    auto incompatible = TreeNode(4, AI::DecisionControlKind::Task);
    incompatible.type = compatible.type;
    incompatible.typeVersion = {2, 0};
    auto tree = Tree(601, {TreeNode(1, AI::DecisionControlKind::Root), sequence, compatible, incompatible},
                     {Edge(1, 1, 2), Edge(2, 2, 3), Edge(3, 2, 4, 1)});
    tree.blackboard = project.schema;
    const AI::DecisionGraphSource utility{Graph(602),
                                          1,
                                          project.schema,
                                          {1, 1},
                                          Location(),
                                          {},
                                          AI::UtilityDecisionSource{.root = Node(1),
                                                                    .options = {{.option = {Node(2), MakeIdentity<AI::TaskId>(701)},
                                                                                 .provider = {.id = Node(2), .type = Type(702)}}}}};
    auto machine = TerminalMachine(603);
    machine.blackboard = project.schema;
    auto &state = std::get<AI::StateMachineAssetDescriptor>(machine.body).states.front();
    state.entry = MakeIdentity<AI::TaskId>(704);
    state.entryProvider = AI::StateMachineProviderAction{{.id = Node(8), .type = Type(101), .descriptorVersion = {2, 2}}, {}};
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
    write(machine, "assets/priority.horo_sm", Assets::AssetId::FromBytes({0x24}), Application::DecisionStateMachineSourceType);
    write(tree, "assets/wrong.horo_sm", Assets::AssetId::FromBytes({0x23}), Application::DecisionStateMachineSourceType);
    project.RebuildRegistry();

    bool installed{};
    SECTION("Missing native authoring catalog is distinct from a missing declaration") {}
    SECTION("The installed catalog distinguishes a compatible task, wrong version, and missing declaration") {
        Horo::Tests::WriteReplicationProject(project.root);
        const auto manifest =
            Horo::Tests::WriteGameplayModuleManifest(project.root, HORO_TEST_AI_MODULE_PATH, "game.ai.tests",
                                                     Gameplay::CurrentGameplayBuildFingerprint(),
                                                     Horo::Tests::ReadDescriptorRevision(HORO_TEST_AI_MODULE_REVISION_PATH));
        REQUIRE(std::filesystem::is_regular_file(manifest));
        installed = true;
    }
    EditorWorkspaceController controller(project.root, project.runtime, project.registry.Snapshot(), {});
    const auto open = [&](const char *path) {
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::OpenSourceFile,
                                   .sourceOpenRequest = SourceOpenRequest{.path = path, .origin = SourceOpenOrigin::AssetActivation}});
    };
    open("assets/guard.horo_bt");
    open("assets/score.horo_utility");
    open("assets/priority.horo_sm");
    const auto &documents = controller.ViewModel().decisionGraphDocuments;
    REQUIRE(documents.size() == 3);
    CHECK(documents[0].graph->asset == tree.asset);
    CHECK(documents[1].graph->asset == utility.asset);
    CHECK(std::holds_alternative<std::shared_ptr<const AI::BehaviorTreeAsset>>(documents[0].graph->body));
    CHECK(std::holds_alternative<AI::UtilityDecisionSource>(documents[1].graph->body));
    const auto &catalog = controller.ViewModel().decisionProviderMetadata;
    CHECK(static_cast<bool>(catalog) == installed);
    const auto projectCanvas = [&](const auto &document) {
        auto projected = ProjectDecisionCanvas(*document.graph, {document.identity.instance, document.revision}, document.editable);
        ApplyDecisionCanvasProviderIssues(projected, *document.graph, catalog.get());
        return projected;
    };
    using Issue = AI::DecisionProviderBindingIssueKind;
    const auto treeView = projectCanvas(documents[0]);
    CHECK(treeView.links.size() == 3);
    REQUIRE(treeView.nodes.size() == 4);
    CHECK(treeView.nodes[0].id == Node(1));
    CHECK_FALSE(treeView.nodes[0].providerIssue);
    CHECK_FALSE(treeView.nodes[1].providerIssue);
    CHECK(treeView.nodes[2].id == Node(3));
    if (installed)
        CHECK_FALSE(treeView.nodes[2].providerIssue);
    else
        CHECK(treeView.nodes[2].providerIssue == Issue::CatalogUnavailable);
    CHECK(treeView.nodes[3].id == Node(4));
    CHECK(treeView.nodes[3].role == GraphNodeRole::Task);
    CHECK(treeView.nodes[3].providerIssue == (installed ? Issue::DescriptorVersionMismatch : Issue::CatalogUnavailable));
    const auto utilityView = projectCanvas(documents[1]);
    CHECK(utilityView.links.size() == 1);
    REQUIRE(utilityView.nodes.size() == 2);
    CHECK(utilityView.nodes[1].id == Node(2));
    CHECK(utilityView.nodes[1].providerIssue == (installed ? Issue::DescriptorMissing : Issue::CatalogUnavailable));
    const auto machineView = projectCanvas(documents[2]);
    REQUIRE(machineView.nodes.size() == 1);
    CHECK(machineView.nodes[0].id == Node(1));
    CHECK(machineView.nodes[0].providerIssue == (installed ? Issue::DescriptorVersionMismatch : Issue::CatalogUnavailable));
    CHECK_FALSE(controller.HasDirtySourceDocuments());
    open("assets/wrong.horo_sm");
    CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.decision_graph.invalid");
    CHECK(documents.size() == 3);
    CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().size() == 3);
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
    auto replacement = project.Snapshot(controller).schema->Keys().front();
    replacement.access = AI::BlackboardKeyAccess::ReadOnly;
    BlackboardWorkspaceRequest request{.fence = issued,
                                       .operation = BlackboardDocumentOperation::ReviewUsages,
                                       .edit = EditBlackboardKey{replacement}};
    controller.ProcessCommand({.command = EditorWorkspaceViewCommand::BlackboardDocument, .blackboard = std::move(request)});
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
    CHECK(view.referencePreview.has_value() == !changed);
    if (view.referencePreview) {
        CHECK(view.referencePreview->key == project.key);
        REQUIRE(view.referencePreview->schema->Keys().size() == 1);
        CHECK(view.referencePreview->schema->Keys().front() == replacement);
        CHECK(view.snapshot.schema->Keys().front() != replacement);
    }
    CHECK_FALSE(view.diagnostic);
}

TEST_CASE("Blackboard workspace pins the selected replacement to a removal proposal", "[unit][editor][blackboard][workspace]") {
    BlackboardWorkspaceFixture project;
    const auto replacement = AI::BlackboardKeyId::Create(project.key.Value() + 1).Value();
    const std::array keys{AI::BlackboardKeyDescriptor{.key = project.key}, AI::BlackboardKeyDescriptor{.key = replacement}};
    const std::array rows{AI::BlackboardSchemaAuthoringRow{project.key, "Original"},
                          AI::BlackboardSchemaAuthoringRow{replacement, "Replacement"}};
    project.WriteSchema(keys, rows);
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
    const auto issued = project.Current(controller);
    controller.ProcessCommand({.command = EditorWorkspaceViewCommand::BlackboardDocument,
                               .blackboard = BlackboardWorkspaceRequest{.fence = issued,
                                                                        .operation = BlackboardDocumentOperation::ReviewUsages,
                                                                        .edit = RemoveBlackboardKey{project.key},
                                                                        .replacement = replacement}});
    REQUIRE(controller.ViewModel().blackboardDocuments.front().reviewingUsages);
    jobs.Shutdown(ShutdownPolicy::Drain);
    controller.UpdateBlackboardUsageReview();
    const auto &view = controller.ViewModel().blackboardDocuments.front();
    REQUIRE(view.usageReview);
    CHECK(view.usageReview->selection.replacement == replacement);
    REQUIRE(view.usageReview->decisionValidation);
    CHECK(view.usageReview->decisionValidation->roots == 0);
    REQUIRE(view.referencePreview);
    REQUIRE(view.referencePreview->schema->Keys().size() == 1);
    CHECK(view.referencePreview->schema->Keys().front().key == replacement);
    CHECK(view.snapshot.schema->Keys().size() == 2);
    CHECK(view.snapshot.revision == issued.revision);
    REQUIRE(view.referenceEditReady);
    controller.ProcessCommand({.command = EditorWorkspaceViewCommand::BlackboardDocument,
                               .blackboard = BlackboardWorkspaceRequest{.fence = issued,
                                                                        .operation = BlackboardDocumentOperation::ApplyReviewedEdit,
                                                                        .expectedReview = view.usageReview}});
    REQUIRE(view.referenceEditStaged);
    CHECK(view.snapshot.schema->Keys().size() == 1);
    controller.ProcessCommand(
        {.command = EditorWorkspaceViewCommand::BlackboardDocument,
         .blackboard = BlackboardWorkspaceRequest{.fence = project.Current(controller), .operation = BlackboardDocumentOperation::Save}});
    REQUIRE_FALSE(view.diagnostic);
    CHECK_FALSE(view.snapshot.dirty);
    CHECK(view.snapshot.durableSchema->Keys().size() == 1);
}

TEST_CASE("Blackboard workspace applies and saves an exact reviewed Decision migration", "[unit][editor][blackboard][workspace]") {
    using namespace AI::DecisionGraphTestSupport;
    BlackboardWorkspaceFixture project;
    const auto replacement = AI::BlackboardKeyId::Create(project.key.Value() + 1).Value();
    const std::array keys{AI::BlackboardKeyDescriptor{.key = project.key}, AI::BlackboardKeyDescriptor{.key = replacement}};
    const std::array rows{AI::BlackboardSchemaAuthoringRow{project.key, "Original"},
                          AI::BlackboardSchemaAuthoringRow{replacement, "Replacement"}};
    project.WriteSchema(keys, rows);
    constexpr auto graphPath = "assets/guard_migration.horo_sm";
    auto graph = TerminalMachine(620);
    graph.blackboard = project.schema;
    auto &body = std::get<AI::StateMachineAssetDescriptor>(graph.body);
    body.states.front().terminal = AI::DecisionStateTerminal::None;
    body.states.push_back({.id = Node(2), .terminal = AI::DecisionStateTerminal::Succeeded, .source = Location()});
    body.transitions.push_back({.id = Node(3),
                                .source = Node(1),
                                .target = Node(2),
                                .guards = {{.key = project.key, .operation = AI::StateMachineGuardOperator::Equal, .operand = true}},
                                .requirements = {{.key = project.key, .kind = AI::BlackboardValueKind::Boolean, .source = Location()}},
                                .location = Location()});
    const auto encoded = AI::DecisionPlanCodec::EncodeSource(std::span{&graph, 1}, graph.asset);
    REQUIRE(encoded.HasValue());
    {
        std::ofstream output(project.root / graphPath, std::ios::binary);
        output.write(reinterpret_cast<const char *>(encoded.Value().data()), static_cast<std::streamsize>(encoded.Value().size()));
        REQUIRE(static_cast<bool>(output));
    }
    project.WriteSidecar(graphPath, Assets::AssetId::FromBytes({17}), Application::DecisionStateMachineSourceType);
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
    const auto key = [&] {
        const auto &opened = controller.ViewModel().decisionGraphDocuments.front();
        return std::get<AI::StateMachineAssetDescriptor>(opened.graph->body).transitions.front().guards.front().key;
    };
    const auto selectGraph = [&] {
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::OpenSourceFile,
                                   .sourceOpenRequest = SourceOpenRequest{.path = graphPath, .origin = SourceOpenOrigin::AssetActivation}});
    };
    controller.ProcessCommand(project.Open());
    const auto issue = project.Current(controller);
    controller.ProcessCommand({.command = EditorWorkspaceViewCommand::BlackboardDocument,
                               .blackboard = BlackboardWorkspaceRequest{.fence = issue,
                                                                        .operation = BlackboardDocumentOperation::ReviewUsages,
                                                                        .edit = RemoveBlackboardKey{project.key},
                                                                        .replacement = replacement}});
    jobs.Shutdown(ShutdownPolicy::Drain);
    controller.UpdateBlackboardUsageReview();
    const auto &view = controller.ViewModel().blackboardDocuments.front();
    REQUIRE(view.usageReview);
    INFO(view.usageReview->gaps.size());
    REQUIRE(view.usageReview->decisionValidation);
    REQUIRE(view.usageReview->decisionRetargets.size() == 1);
    REQUIRE(view.usageReview->commitEvidence);
    const auto review = view.usageReview;
    const auto apply = [&] {
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::BlackboardDocument,
                                   .blackboard = BlackboardWorkspaceRequest{.fence = issue,
                                                                            .operation = BlackboardDocumentOperation::ApplyReviewedEdit,
                                                                            .expectedReview = review}});
    };
    CHECK_FALSE(view.referenceEditReady);
    apply();
    CHECK(view.snapshot.revision == issue.revision);
    selectGraph();
    REQUIRE(controller.ViewModel().decisionGraphDocuments.size() == 1);
    REQUIRE(controller.ViewModel().decisionGraphDocuments.front().editable);
    CHECK(key() == project.key);
    controller.ProcessCommand(project.Open());
    controller.UpdateBlackboardUsageReview();
    REQUIRE(view.referenceEditReady);
    SECTION("a source changed after review leaves both document roots untouched") {
        std::ofstream changed(project.root / (std::string{graphPath} + ".horo"), std::ios::binary | std::ios::app);
        changed << '\n';
        changed.close();
        apply();
        CHECK(view.snapshot.revision == issue.revision);
        CHECK(key() == project.key);
        CHECK_FALSE(view.referenceEditStaged);
    }
    SECTION("the matching group stages, replays, and persists through its schema command") {
        apply();
        REQUIRE(view.referenceEditStaged);
        CHECK(view.snapshot.schema->Keys().size() == 1);
        CHECK(key() == replacement);
        const auto command = [&](BlackboardDocumentOperation operation) {
            controller.ProcessCommand(
                {.command = EditorWorkspaceViewCommand::BlackboardDocument,
                 .blackboard = BlackboardWorkspaceRequest{.fence = project.Current(controller), .operation = operation}});
        };
        command(BlackboardDocumentOperation::Undo);
        REQUIRE_FALSE(view.referenceEditStaged);
        CHECK(view.snapshot.schema->Keys().size() == 2);
        CHECK(key() == project.key);
        command(BlackboardDocumentOperation::Redo);
        REQUIRE(view.referenceEditStaged);
        CHECK(key() == replacement);
        CHECK_FALSE(controller.ResolveBlackboardClose(project.Current(controller), SourceCloseDecision::Discard).HasValue());
        command(BlackboardDocumentOperation::Save);
        if (view.diagnostic)
            INFO(std::string{view.diagnostic->code.Value()} + " " + view.diagnostic->message);
        REQUIRE_FALSE(view.diagnostic);
        CHECK_FALSE(view.referenceEditStaged);
        CHECK_FALSE(view.snapshot.dirty);
        CHECK_FALSE(controller.ViewModel().decisionGraphDocuments.front().dirty);
        CHECK(view.snapshot.durableSchema->Keys().size() == 1);
        CHECK(key() == replacement);
    }
    SECTION("an external schema change blocks publication before any Decision write") {
        apply();
        REQUIRE(view.referenceEditStaged);
        auto differentRows = rows;
        differentRows.front().displayName = "Changed outside the editor";
        project.WriteSchema(keys, differentRows);
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::BlackboardDocument,
                                   .blackboard = BlackboardWorkspaceRequest{.fence = project.Current(controller),
                                                                            .operation = BlackboardDocumentOperation::Save}});
        REQUIRE(view.diagnostic);
        CHECK(view.diagnostic->code.Value() == Assets::AssetSourceErrors::Conflict.code.Value());
        const auto durable = Assets::CaptureAssetSource(project.root, project.root / graphPath, AI::DecisionCodecLimits{}.maximumBytes);
        REQUIRE(durable.HasValue());
        CHECK(durable.Value().identity.digest == ComputeSha256(std::as_bytes(std::span{encoded.Value()})));
        CHECK(view.snapshot.dirty);
        CHECK(controller.ViewModel().decisionGraphDocuments.front().dirty);
    }
}

TEST_CASE("Blackboard workspace reviews the loaded native Decision catalog and rejects its retired stamp",
          "[unit][editor][blackboard][workspace]") {
    BlackboardWorkspaceFixture project;
    const auto replacement = AI::BlackboardKeyId::Create(project.key.Value() + 1).Value();
    const std::array keys{AI::BlackboardKeyDescriptor{.key = project.key}, AI::BlackboardKeyDescriptor{.key = replacement}};
    const std::array rows{AI::BlackboardSchemaAuthoringRow{project.key, "Original"},
                          AI::BlackboardSchemaAuthoringRow{replacement, "Replacement"}};
    project.WriteSchema(keys, rows);
    Horo::Tests::WriteReplicationProject(project.root);
    const auto manifest = Horo::Tests::WriteGameplayModuleManifest(project.root, HORO_TEST_AI_MODULE_PATH, "game.ai.tests",
                                                                   Gameplay::CurrentGameplayBuildFingerprint(),
                                                                   Horo::Tests::ReadDescriptorRevision(HORO_TEST_AI_MODULE_REVISION_PATH));
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
                                                                        .edit = RemoveBlackboardKey{project.key},
                                                                        .replacement = replacement}});
    REQUIRE(controller.ViewModel().blackboardDocuments.front().reviewingUsages);

    bool replaced = false;
    SECTION("the declared node resolves through the real manifest-selected catalog") {}
    SECTION("replacement of the same manifest image still revokes older review output") {
        Horo::Tests::AdvanceLastWriteTime(manifest);
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
        REQUIRE(view.usageReview->decisionValidation);
        CHECK(view.usageReview->decisionValidation->roots == 1);
        const auto generation = view.usageReview->decisionValidation->nodes.lock();
        REQUIRE(generation);
        CHECK(generation->CanUse());
        const auto providers = view.usageReview->providers.lock();
        REQUIRE(providers);
        REQUIRE(providers->Descriptors().size() == 2);
        CHECK(providers->Descriptors().front().type == AI::DecisionNodeTypeId::Create(101).Value());
        CHECK(providers->Descriptors().back().type == AI::DecisionNodeTypeId::Create(102).Value());
        CHECK(std::ranges::none_of(view.usageReview->gaps, [](const Application::BlackboardUsageGap &gap) {
            return gap.kind == Application::BlackboardUsageGapKind::DecisionProvider;
        }));
    }
}
