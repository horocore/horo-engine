#include "Horo/AI/EnvironmentQueryExecution.h"
#include "Horo/AI/EnvironmentQueryGenerators.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "OwnedTestDirectory.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/WorkspacePathText.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserInteractionSession.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <format>
#include <fstream>
#include <thread>

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

        explicit Project(bool existingQuery = true) {
            REQUIRE(std::filesystem::create_directories(root / "assets"));
            if (existingQuery) {
                WriteQuery("Saved query");
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
            WriteAsset(query.Value());
        }

        void WriteAsset(const EnvironmentQueryAsset &query) const {
            const auto encoded = EnvironmentQuerySourceCodec::Encode(query);
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
    CHECK(documents.front().descriptors == QueryDescriptorAvailability::RegistryUnavailable);
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

TEST_CASE("Environment Query workspace uses the composed native descriptor registry without hiding unknown source",
          "[unit][editor][eqs][workspace]") {
    Project project;
    const auto registry = CaptureBuiltinQuerySchema();
    REQUIRE(registry.HasValue());
    EditorWorkspaceController controller(project.root, project.runtime, project.registry.Snapshot(), {.querySchema = &registry.Value()});
    Project::Open(controller, Project::Path);
    const auto &documents = controller.ViewModel().environmentQueryDocuments;
    REQUIRE(documents.size() == 1);
    CHECK(documents.front().descriptors == QueryDescriptorAvailability::Incompatible);
    CHECK(documents.front().query->Stages().front().kind == QueryStageKind::Unknown);

    const auto query =
        CreateBuiltinLatticeQuery(QueryId::Create(9).Value(), QueryStageId::Create(10).Value(), QueryResultSchemaId::Create(11).Value(),
                                  "Local points", {.stepMillimeters = 750, .outerSteps = 2}, 25);
    REQUIRE(query.HasValue());
    project.WriteAsset(query.Value());
    Project::Open(controller, Project::Path);
    REQUIRE(documents.size() == 1);
    CHECK(documents.front().query->Id() == query.Value().Id());
    CHECK(documents.front().descriptors == QueryDescriptorAvailability::Validated);
}

TEST_CASE("Environment Query workspace previews the saved native lattice from an exact authoring Scene", "[unit][editor][eqs][workspace]") {
    Project project;
    const auto registry = CaptureBuiltinQuerySchema();
    REQUIRE(registry.HasValue());
    const auto query =
        CreateBuiltinLatticeQuery(QueryId::Create(9).Value(), QueryStageId::Create(10).Value(), QueryResultSchemaId::Create(11).Value(),
                                  "Local points", {.stepMillimeters = 500, .outerSteps = MaximumQueryLatticeOuterSteps}, 100);
    REQUIRE(query.HasValue());
    project.WriteAsset(query.Value());

    CancellationSource cancellation;
    REQUIRE(project.runtime.Startup(cancellation.Token()).HasValue());
    JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 8}};
    EditorWorkspaceController controller(project.root, project.runtime, project.registry.Snapshot(),
                                         {.assets = {.mutableAssetRegistry = &project.registry},
                                          .mutations = &project.mutations,
                                          .durableFiles = &project.files,
                                          .jobs = &jobs,
                                          .querySchema = &registry.Value()});
    REQUIRE_FALSE(controller.InitializationError());
    const auto &model = controller.ViewModel();
    REQUIRE(model.objects.size() == 1);
    const auto object = model.objects.front().id;
    auto transform = model.objects.front().localTransform;
    transform.translation = {2, 3, -4};
    controller.ProcessCommand({.command = EditorWorkspaceViewCommand::SelectObject, .objectPayload = object});
    controller.ProcessCommand(
        {.command = EditorWorkspaceViewCommand::CommitObjectTransform, .objectPayload = object, .transformPayload = transform});
    for (int frame = 0; frame < 2000; ++frame) {
        REQUIRE(project.runtime
                    .OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges,
                             {static_cast<std::uint64_t>(frame + 1), {}, 0.0, 0, {}, false, cancellation.Token()})
                    .HasValue());
        controller.SynchronizeRuntimeScenePreview();
        if (controller.ViewportScene().documentRevision == model.documentRevision && model.primarySelection == object)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(controller.ViewportScene().documentRevision == model.documentRevision);
    REQUIRE(model.primarySelection == object);
    const auto active = project.runtime.ActiveScene();
    REQUIRE(active);

    Project::Open(controller, Project::Path);
    REQUIRE(model.environmentQueryDocuments.size() == 1);
    const auto &document = model.environmentQueryDocuments.front();
    REQUIRE(document.descriptors == QueryDescriptorAvailability::Validated);
    const auto pinned = EnvironmentQueryPreviewRequest{document.identity.instance, document.source, model.documentRevision, object};
    const auto submit = [&](EnvironmentQueryPreviewRequest request) {
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::PreviewEnvironmentQuery, .environmentQueryPreview = request});
    };

    using enum EnvironmentQueryPreviewStatus;
    auto expected = Ready;
    std::optional<QueryStageId> distanceStage;
    SECTION("the saved native test filters and reports the actual distance contribution") {
        const auto small = CreateBuiltinLatticeQuery(query.Value().Id(), query.Value().Stages().front().id, query.Value().ResultSchema().id,
                                                     "Distance points", {.stepMillimeters = 1000, .outerSteps = 2}, 7);
        REQUIRE(small.HasValue());
        distanceStage = QueryStageId::Create(12).Value();
        const auto scored = AddBuiltinPointDistanceTest(small.Value(), *distanceStage, {1500, true});
        REQUIRE(scored.HasValue());
        project.WriteAsset(scored.Value());
        Project::Open(controller, Project::Path);
        auto edited = pinned;
        edited.source = document.source;
        submit(edited);
        REQUIRE(document.preview.status == Running);
    }
    SECTION("the retained preview is bounded, stable and leaves the Scene owner unchanged") {
        submit(pinned);
        REQUIRE(document.preview.status == Running);
    }
    SECTION("a legitimate Scene edit revokes an outstanding preview") {
        submit(pinned);
        REQUIRE(document.preview.status == Running);
        transform.translation.x += 1;
        controller.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::CommitObjectTransform, .objectPayload = object, .transformPayload = transform});
        REQUIRE(model.documentRevision != pinned.scene);
        expected = Stale;
    }
    SECTION("an explicit Cancel prevents publication of a retained worker result") {
        submit(pinned);
        REQUIRE(document.preview.status == Running);
        auto cancel = pinned;
        cancel.action = EnvironmentQueryPreviewAction::Cancel;
        submit(cancel);
        expected = Cancelled;
    }
    SECTION("stale saved query bytes are refused before a worker is accepted") {
        project.WriteQuery("Changed query");
        submit(pinned);
        expected = Stale;
    }
    jobs.Shutdown(ShutdownPolicy::Drain);
    controller.UpdateEnvironmentQueryPreview();
    REQUIRE(document.preview.status == expected);
    CHECK_FALSE(controller.HasDirtySourceDocuments());
    if (expected != Ready) {
        CHECK_FALSE(document.preview.snapshot);
        return;
    }
    REQUIRE(document.preview.snapshot);
    const auto &preview = *document.preview.snapshot;
    if (distanceStage) {
        REQUIRE(preview.candidates.size() == 25);
        REQUIRE(preview.stages.size() == 25);
        CHECK(preview.generated == 25);
        CHECK(preview.winnerCount == 7);
        CHECK(preview.stages[0].candidateIndex == 0);
        CHECK(preview.stages[0].score.stage == *distanceStage);
        CHECK(preview.stages[0].score.status == QueryScoreStatus::Filtered);
        CHECK(preview.stages[0].score.contribution == 0.0);
        CHECK(preview.stages[12].score.status == QueryScoreStatus::Eligible);
        CHECK(preview.stages[12].score.contribution == 1.0);
        CHECK(preview.stages[13].score.status == QueryScoreStatus::Eligible);
        CHECK(preview.stages[13].score.contribution > 0.3);
        CHECK(preview.stages[13].score.contribution < 0.4);
        REQUIRE_FALSE(preview.winners.empty());
        CHECK(preview.winners.front().itemIndex == 12);
        CHECK(preview.winners.front().scoreKey == QueryScoringPlan::ScoreKeyScale);
        return;
    }
    constexpr std::size_t width = MaximumQueryLatticeOuterSteps * 2 + 1;
    CHECK(preview.query == query.Value().Id());
    CHECK(preview.generated == width * width);
    CHECK(preview.evaluated == preview.generated);
    CHECK(preview.invalidCoordinates == 0);
    CHECK(preview.winnerCount == 100);
    CHECK(preview.winners.size() == QueryDebugCaptureSettings::MaximumSamples);
    CHECK(preview.stages.empty());
    REQUIRE(preview.candidates.size() == QueryDebugCaptureSettings::MaximumSamples);
    CHECK(preview.candidates.front().index == 0);
    CHECK(preview.candidates.back().index == preview.generated - 1);
    CHECK(preview.candidates.front().point == Math::WorldCoordinate64::FromMillimeters(-13500, 3000, -19500));
    CHECK(preview.candidates.back().point == Math::WorldCoordinate64::FromMillimeters(17500, 3000, 11500));
    REQUIRE(preview.candidates.back().score);
    CHECK(preview.candidates.back().score->status == QueryScoreStatus::Eligible);
    CHECK(preview.candidates.back().score->scoreKey == 0);
    CHECK(model.documentRevision == pinned.scene);
    CHECK(active->IsCurrent());
    CHECK(document.preview.scene.runtime == active->RuntimeId());
    CHECK(document.preview.scene.structure == active->StructuralRevision());
    CHECK(model.playState == EditorPlayState::Idle);

    transform.translation.x += 1;
    controller.ProcessCommand(
        {.command = EditorWorkspaceViewCommand::CommitObjectTransform, .objectPayload = object, .transformPayload = transform});
    controller.UpdateEnvironmentQueryPreview();
    CHECK(document.preview.status == Stale);
    CHECK_FALSE(document.preview.snapshot);
}

TEST_CASE("Registered Query settings use shared history and tracked source publication", "[unit][editor][eqs][workspace]") {
    Project project;
    const auto registry = CaptureBuiltinQuerySchema();
    REQUIRE(registry.HasValue());
    const auto initial =
        CreateBuiltinLatticeQuery(QueryId::Create(14).Value(), QueryStageId::Create(41).Value(), QueryResultSchemaId::Create(8).Value(),
                                  "Editable points", {.stepMillimeters = 500, .outerSteps = 3}, 9);
    REQUIRE(initial.HasValue());
    const auto native = AddBuiltinPointDistanceTest(initial.Value(), QueryStageId::Create(52).Value(), {1500, true});
    REQUIRE(native.HasValue());
    project.WriteAsset(native.Value());
    EditorWorkspaceController controller(project.root, project.runtime, project.registry.Snapshot(),
                                         {.assets = {.mutableAssetRegistry = &project.registry},
                                          .mutations = &project.mutations,
                                          .durableFiles = &project.files,
                                          .querySchema = &registry.Value()});
    const auto disk = [&] {
        return Assets::CaptureAssetSource(project.root, project.root / Project::Path, EnvironmentQuerySourceCodec::MaximumBytes);
    };
    const auto before = disk();
    REQUIRE(before.HasValue());
    Project::Open(controller, Project::Path);
    const auto &documents = controller.ViewModel().environmentQueryDocuments;
    REQUIRE(documents.size() == 1);
    const auto &document = documents.front();
    REQUIRE(document.settings && document.settings->distance);
    const auto original = document.query;
    const auto instance = document.identity.instance;
    using enum EnvironmentQueryEditAction;

    auto changed = *document.settings;
    const auto issue = [&](EnvironmentQueryEditAction action, std::optional<BuiltinPointQuerySettings> values = {}) {
        controller.ProcessCommand(MakeEnvironmentQueryEditCommand(document, action, values));
    };
    changed.lattice.stepMillimeters = 0;
    issue(Apply, changed);
    REQUIRE(document.diagnostic);
    CHECK(document.query == original);
    CHECK_FALSE(document.canUndo);
    changed.lattice = {.stepMillimeters = 800, .outerSteps = 4, .innerSteps = 1};
    changed.maximumResults = 7;
    changed.distance = QueryPointDistanceTest{2400, false};
    const auto oldRequest = MakeEnvironmentQueryEditCommand(document, Apply, changed);
    controller.ProcessCommand(oldRequest);
    REQUIRE_FALSE(document.diagnostic);
    CHECK(document.query != original);
    CHECK(controller.HasDirtySourceDocuments());
    CHECK(document.canUndo);
    REQUIRE(document.query->Stages().size() == original->Stages().size());
    for (std::size_t index = 0; index < document.query->Stages().size(); ++index)
        CHECK(document.query->Stages()[index].id == original->Stages()[index].id);
    CHECK(document.query->Id() == original->Id());
    CHECK(document.query->ResultSchema().id == original->ResultSchema().id);
    const auto draft = document.query;
    controller.ProcessCommand(oldRequest);
    REQUIRE(document.diagnostic);
    CHECK(document.query == draft);

    SECTION("undo, redo, Save All and reopen retain exact identities and edited values") {
        issue(Undo);
        REQUIRE_FALSE(document.diagnostic);
        CHECK(document.query == original);
        CHECK_FALSE(document.dirty);
        CHECK(document.canRedo);
        issue(Redo);
        REQUIRE_FALSE(document.diagnostic);
        CHECK(document.query == draft);
        const auto uncommitted = disk();
        REQUIRE(uncommitted.HasValue());
        CHECK(uncommitted.Value().identity == before.Value().identity);
        REQUIRE(controller.SaveSourceDocuments().HasValue());
        CHECK_FALSE(controller.HasDirtySourceDocuments());
        const auto committed = disk();
        REQUIRE(committed.HasValue());
        CHECK(committed.Value().identity.digest == document.source);
        CHECK(committed.Value().identity.digest != before.Value().identity.digest);
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = instance});
        REQUIRE(documents.empty());
        Project::Open(controller, Project::Path);
        REQUIRE(documents.size() == 1);
        const auto &reopened = documents.front();
        REQUIRE(reopened.settings && reopened.settings->distance);
        CHECK(reopened.identity.instance != instance);
        CHECK(reopened.asset == project.asset);
        CHECK(reopened.query->Id() == original->Id());
        CHECK(reopened.query->Stages().back().id == native.Value().Stages().back().id);
        CHECK(reopened.settings->lattice.stepMillimeters == 800);
        CHECK(reopened.settings->lattice.innerSteps == 1);
        CHECK(reopened.settings->maximumResults == 7);
        CHECK(reopened.settings->distance->radiusMillimeters == 2400);
        CHECK_FALSE(reopened.settings->distance->preferNearer);
        CHECK_FALSE(reopened.canUndo);
    }
    SECTION("changing the sidecar bytes refuses the source write even when the stable ID still matches") {
        const auto sidecar = project.root / (std::string{Project::Path} + ".horo");
        {
            std::ofstream output(sidecar, std::ios::binary | std::ios::app);
            output << '\n';
            REQUIRE(static_cast<bool>(output));
        }
        issue(Save);
        CHECK(document.conflict);
        CHECK(document.dirty);
        CHECK(document.query == draft);
        const auto unchanged = disk();
        REQUIRE(unchanged.HasValue());
        CHECK(unchanged.Value().identity == before.Value().identity);
    }
    SECTION("external bytes cannot displace the draft, and stale close cannot discard it") {
        project.WriteQuery("External");
        Project::Open(controller, Project::Path);
        CHECK(document.query == draft);
        CHECK(document.conflict);
        issue(Save);
        CHECK(document.dirty);
        REQUIRE(document.diagnostic);
        const auto denied = controller.ResolveEnvironmentQueryClose({instance, document.revision - 1}, SourceCloseDecision::Discard);
        CHECK(denied.HasError());
        REQUIRE(documents.size() == 1);
        const auto closed = controller.ResolveEnvironmentQueryClose({instance, document.revision}, SourceCloseDecision::Discard);
        REQUIRE(closed.HasValue());
        CHECK(closed.Value());
        Project::Open(controller, Project::Path);
        REQUIRE(documents.size() == 1);
        CHECK(documents.front().query->DisplayName() == "External");
        CHECK_FALSE(documents.front().settings);
    }
}

TEST_CASE("Environment Query New creates the first registered source from explicit native lattice settings",
          "[unit][editor][eqs][workspace]") {
    Project project{false};
    const auto registry = CaptureBuiltinQuerySchema();
    REQUIRE(registry.HasValue());
    EditorWorkspaceController controller(project.root, project.runtime, project.registry.Snapshot(),
                                         {.assets = {.mutableAssetRegistry = &project.registry},
                                          .mutations = &project.mutations,
                                          .durableFiles = &project.files,
                                          .latticeQueryFactory = CreateBuiltinLatticeQuery,
                                          .querySchema = &registry.Value()});
    REQUIRE(controller.ViewModel().environmentQueryCreationAvailable);
    AssetBrowserInteractionSession browser;
    browser.OpenCreateQuery(WorkspacePathText(project.root / "assets"));
    auto &draft = browser.State().createQuery;
    CHECK_FALSE(AssetBrowserInteractionSession::CanConfirmCreateQuery(draft));
    constexpr std::string_view name = "First lattice";
    std::ranges::copy(name, draft.name.begin());
    draft.stepMillimeters = 250;
    draft.outerSteps = 2;
    draft.maximumResults = 7;
    draft.annulus = true;
    draft.innerSteps = 1;

    constexpr auto path = "assets/First lattice.horo_eqs";
    SECTION("the New route validates explicit distance settings before publishing the registered stage") {
        draft.distance = true;
        draft.radiusMillimeters = 0;
        CHECK_FALSE(AssetBrowserInteractionSession::ConfirmCreateQuery(draft));
        draft.radiusMillimeters = 1250;
        draft.preferNearer = false;
        const auto command = AssetBrowserInteractionSession::ConfirmCreateQuery(draft);
        REQUIRE(command);
        controller.ProcessCommand(*command);
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        const auto &documents = controller.ViewModel().environmentQueryDocuments;
        REQUIRE(documents.size() == 1);
        const auto document = documents.front().identity.instance;
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = document});
        REQUIRE(documents.empty());
        Project::Open(controller, path);
        REQUIRE(documents.size() == 1);
        const auto plan = EnvironmentQueryPlan::Compile(*documents.front().query, registry.Value());
        REQUIRE(plan.HasValue());
        REQUIRE(plan.Value().Stages().size() == 2);
        const auto stage = BuiltinPointDistanceQuery::Capture(plan.Value().Stages().back(), registry.Value());
        REQUIRE(stage.HasValue());
        CHECK_FALSE(stage.Value().Rule().score);
        const auto boundary = stage.Value().Evaluate({}, Math::WorldCoordinate64::FromMillimeters(1250, 0, 0));
        REQUIRE(boundary.HasValue());
        CHECK(boundary.Value().filter == true);
        const auto outside = stage.Value().Evaluate({}, Math::WorldCoordinate64::FromMillimeters(1251, 0, 0));
        REQUIRE(outside.HasValue());
        CHECK(outside.Value().filter == false);
    }
    SECTION("accepted workspace command stages the pair and opens the exact registered result") {
        const auto command = AssetBrowserInteractionSession::ConfirmCreateQuery(draft);
        REQUIRE(command.has_value());
        controller.ProcessCommand(*command);
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        const auto current = project.registry.Snapshot();
        const auto *record = current.FindByPath(path);
        REQUIRE(record);
        CHECK(record->type.Value() == EnvironmentQuerySourceAssetType);
        CHECK(std::filesystem::is_regular_file(project.root / path));
        CHECK(std::filesystem::is_regular_file(project.root / (std::string{path} + ".horo")));
        const auto &documents = controller.ViewModel().environmentQueryDocuments;
        REQUIRE(documents.size() == 1);
        CHECK(documents.front().asset == record->id);
        CHECK(documents.front().query->DisplayName() == name);
        CHECK(documents.front().descriptors == QueryDescriptorAvailability::Validated);
        CHECK(documents.front().query->ResultSchema().maximumResults == 7);
        REQUIRE(documents.front().query->Stages().size() == 1);
        const auto &stage = documents.front().query->Stages().front();
        CHECK(stage.kind == QueryStageKind::Generator);
        REQUIRE(registry.Value().Find(stage.generator));
        REQUIRE(stage.properties.size() == 3);
        for (const auto value : {250, 2, 1})
            CHECK(std::ranges::any_of(stage.properties, [value](const auto &property) {
                const auto *number = std::get_if<std::uint64_t>(&property.value);
                return number && *number == static_cast<std::uint64_t>(value);
            }));
        CHECK(controller.ViewModel().workspacePanelHost.ActiveDocument() == documents.front().identity.instance);

        controller.ProcessCommand(*command);
        CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.content_browser.operation.name_exists");
        CHECK(project.registry.Snapshot().Revision() == current.Revision());
    }
    SECTION("invalid fields are refused by the modal and stale or synthetic commands cannot write") {
        draft.innerSteps = 3;
        CHECK_FALSE(AssetBrowserInteractionSession::ConfirmCreateQuery(draft).has_value());
        draft.innerSteps = 1;
        auto command = AssetBrowserInteractionSession::ConfirmCreateQuery(draft);
        REQUIRE(command.has_value());
        command->newEnvironmentQuery->lattice.stepMillimeters = 0;
        controller.ProcessCommand(*command);
        CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.environment_query.create_failed");
        CHECK(project.registry.Snapshot().FindByPath(path) == nullptr);
        CHECK_FALSE(std::filesystem::exists(project.root / path));
        CHECK_FALSE(std::filesystem::exists(project.root / (std::string{path} + ".horo")));
    }
    SECTION("the existing project mutation owner excludes a concurrent first creation") {
        const auto lease = project.mutations.TryAcquire({project.root, ProjectMutationOwner::Asset, "other-author"});
        REQUIRE(lease.HasValue());
        const auto command = AssetBrowserInteractionSession::ConfirmCreateQuery(draft);
        REQUIRE(command.has_value());
        controller.ProcessCommand(*command);
        CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.environment_query.create_failed");
        CHECK(project.registry.Snapshot().FindByPath(path) == nullptr);
        CHECK_FALSE(std::filesystem::exists(project.root / path));
    }
}
