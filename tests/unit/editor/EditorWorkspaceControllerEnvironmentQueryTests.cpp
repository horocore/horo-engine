#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQueryExecution.h"
#include "Horo/AI/EnvironmentQueryGenerators.h"
#include "Horo/AI/EnvironmentQueryPerception.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Editor/DocumentRecoveryService.h"
#include "Horo/Scene/SceneSource.h"
#include "OwnedTestDirectory.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/EnvironmentQueryPreviewPhysics.h"
#include "editor/screens/workspace/WorkspacePathText.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserInteractionSession.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <format>
#include <fstream>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::AI;
    using namespace Horo::Editor;

    /** @brief Records the exact owner Scene seen by a test location provider. */
    struct PreviewLocationProvider final {
        Math::WorldCoordinate64 point = Math::WorldCoordinate64::FromMillimeters(8000, 2000, -1000);
        Runtime::SceneRuntimeId scene;
        std::size_t calls{};

        static Result<QueryContextValue> Capture(void *state, const Runtime::RuntimeSceneView &view) {
            auto &provider = *static_cast<PreviewLocationProvider *>(state);
            provider.scene = view.RuntimeId();
            ++provider.calls;
            return Result<QueryContextValue>::Success(provider.point);
        }
    };

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
    const auto location = QueryContextId::Create(31).Value();
    const auto restricted = QueryContextId::Create(32).Value();
    const QueryDescriptorOrigin origin{QueryDescriptorSourceKind::Native, QueryProviderId::Create(33).Value(), 2};
    const std::array choices{QueryContextDescriptor{.id = location, .origin = origin, .displayName = "Encounter origin"},
                             QueryContextDescriptor{.id = restricted, .origin = origin, .displayName = "Perceived origin"}};
    const auto registry = CaptureBuiltinQuerySchema(choices);
    REQUIRE(registry.HasValue());
    PreviewLocationProvider locationSource;
    const auto granted = AiCapabilitySet::Of(AiCapability::Behavior);
    const std::array registrations{QueryContextProviderRegistration{location, 2, granted, &locationSource,
                                                                    PreviewLocationProvider::Capture},
                                   QueryContextProviderRegistration{restricted, 2, AiCapabilitySet::Of(AiCapability::Perception),
                                                                    &locationSource, PreviewLocationProvider::Capture}};
    const auto providers = QueryContextProviderRegistry::Capture(registrations, registry.Value());
    REQUIRE(providers.HasValue());
    const auto query =
        CreateBuiltinLatticeQuery(QueryId::Create(9).Value(), QueryStageId::Create(10).Value(), QueryResultSchemaId::Create(11).Value(),
                                  "Local points", {.stepMillimeters = 500, .outerSteps = MaximumQueryLatticeOuterSteps}, 100);
    REQUIRE(query.HasValue());
    project.WriteAsset(query.Value());

    CancellationSource cancellation;
    REQUIRE(project.runtime.Startup(cancellation.Token()).HasValue());
    JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 8}};
    const auto owner = std::this_thread::get_id();
    std::size_t physicsCaptures{};
    const EnvironmentQueryPreviewPhysics physics{[&](const Runtime::RuntimeSceneView &scene, const QueryPhysicsVisibilityRequest &request) {
        CHECK(std::this_thread::get_id() == owner);
        CHECK(scene.IsCurrent());
        CHECK(scene.Get(request.querier).HasValue());
        CHECK(request.eye == Math::WorldCoordinate64::FromMillimeters(2000, 3000, -4000));
        CHECK(request.points.size() == MaximumQueryPhysicsVisibilityRays);
        CHECK(request.channel == Physics::PhysicsQueryChannelId::FromBytes({1}));
        ++physicsCaptures;
        EnvironmentQueryPreviewPhysics::Visibility clear;
        clear.fill(true);
        clear.front() = false;
        return Result<EnvironmentQueryPreviewPhysics::Visibility>::Success(clear);
    }};
    EditorWorkspaceController controller(project.root, project.runtime, project.registry.Snapshot(),
                                         {.assets = {.mutableAssetRegistry = &project.registry},
                                          .mutations = &project.mutations,
                                          .durableFiles = &project.files,
                                          .jobs = &jobs,
                                          .querySchema = &registry.Value(),
                                          .queryContextAccess = {&providers.Value(), granted},
                                          .queryPreviewPhysics = &physics});
    REQUIRE_FALSE(controller.InitializationError());
    const auto &model = controller.ViewModel();
    REQUIRE(model.objects.size() == 1);
    const auto object = model.objects.front().id;
    controller.ProcessCommand(
        {.command = EditorWorkspaceViewCommand::CreatePrimitive, .primitivePayload = Runtime::PrimitiveId{"primitive.mesh.box"}});
    REQUIRE(model.objects.size() == 2);
    const auto target = model.objects.back().id;
    REQUIRE(target != object);
    auto targetTransform = model.objects.back().localTransform;
    targetTransform.translation = {-1, 0, 3};
    controller.ProcessCommand(
        {.command = EditorWorkspaceViewCommand::CommitObjectTransform, .objectPayload = target, .transformPayload = targetTransform});
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
    std::optional<QueryStageId> filterStage;
    std::optional<QueryStageId> visibilityStage;
    bool mixedContexts = false;
    bool usesTarget = false;
    bool usesProvider = false;
    bool framePoint = false;
    bool editBeforeFrame = false;
    SECTION("registered provider choices are named and used only with the host grants") {
        auto selection = location;
        SECTION("the owner grants the selected provider capability") {
            usesProvider = true;
        }
        SECTION("the owner withholds the selected provider capability") {
            selection = restricted;
            expected = Failed;
        }
        const auto lattice =
            CreateBuiltinLatticeQuery(query.Value().Id(), query.Value().Stages().front().id, query.Value().ResultSchema().id,
                                      "Provider points", {.stepMillimeters = 1000, .outerSteps = 1, .center = selection}, 9);
        REQUIRE(lattice.HasValue());
        const auto test = AddBuiltinPointDistanceTest(lattice.Value(), QueryStageId::Create(12).Value(), {1000, true, 1, selection});
        REQUIRE(test.HasValue());
        project.WriteAsset(test.Value());
        Project::Open(controller, Project::Path);
        REQUIRE(document.settings.has_value());
        const auto option = std::ranges::find(document.settings->latticeCenters, selection, &QueryPointContextOption::id);
        REQUIRE(option != document.settings->latticeCenters.end());
        CHECK(option->displayName == (usesProvider ? "Encounter origin" : "Perceived origin"));
        auto edited = pinned;
        edited.source = document.source;
        submit(edited);
        if (usesProvider) {
            REQUIRE(document.preview.status == Running);
            CHECK(locationSource.scene == active->RuntimeId());
            CHECK(locationSource.calls == 1);
        } else {
            REQUIRE(document.preview.status == Failed);
            REQUIRE(document.preview.diagnostic);
            CHECK(document.preview.diagnostic->code.Value() == AIErrors::EnvironmentQueryContextCapabilityUnavailable.code.Value());
            CHECK(locationSource.calls == 0);
        }
    }
    SECTION("target inputs capture the other selected object and never substitute the querier") {
        usesTarget = true;
        const auto center = BuiltinQueryContextId(BuiltinQueryContext::TargetLocation);
        const auto lattice =
            CreateBuiltinLatticeQuery(query.Value().Id(), query.Value().Stages().front().id, query.Value().ResultSchema().id,
                                      "Target points", {.stepMillimeters = 1000, .outerSteps = 1, .center = center}, 9);
        REQUIRE(lattice.HasValue());
        const auto staged = AddBuiltinPointDistanceTest(lattice.Value(), QueryStageId::Create(12).Value(), {1000, true, 1, center});
        REQUIRE(staged.HasValue());
        project.WriteAsset(staged.Value());
        Project::Open(controller, Project::Path);
        auto edited = pinned;
        edited.source = document.source;
        submit(edited);
        REQUIRE(document.preview.status == Failed);
        REQUIRE(document.preview.diagnostic);
        CHECK(document.preview.diagnostic->code.Value() == AIErrors::EnvironmentQueryContextMissing.code.Value());

        controller.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::SelectObject, .objectSelection = ObjectSelectionRequest{{target, object}, object}});
        edited.target = object;
        submit(edited);
        REQUIRE(document.preview.status == Stale);
        edited.target = target;
        submit(edited);
        REQUIRE(document.preview.status == Running);
        SECTION("the selected target stays live through publication") {
            CHECK(document.preview.scene.target == target);
        }
        SECTION("losing the secondary selection revokes an outstanding result") {
            controller.ProcessCommand({.command = EditorWorkspaceViewCommand::SelectObject, .objectPayload = object});
            CHECK(model.documentRevision == pinned.scene);
            expected = Stale;
        }
    }
    SECTION("generation and distance tests use the distinct captured locations") {
        mixedContexts = true;
        const auto origin = BuiltinQueryContextId(BuiltinQueryContext::WorldOrigin);
        const auto lattice =
            CreateBuiltinLatticeQuery(query.Value().Id(), query.Value().Stages().front().id, query.Value().ResultSchema().id,
                                      "World points", {.stepMillimeters = 1000, .outerSteps = 1, .center = origin}, 9);
        REQUIRE(lattice.HasValue());
        const auto staged = AddBuiltinPointDistanceTest(lattice.Value(), QueryStageId::Create(12).Value(), {1000, true});
        REQUIRE(staged.HasValue());
        project.WriteAsset(staged.Value());
        Project::Open(controller, Project::Path);
        auto edited = pinned;
        edited.source = document.source;
        submit(edited);
        REQUIRE(document.preview.status == Running);
    }
    SECTION("the saved native test filters and reports the actual distance contribution") {
        const auto small = CreateBuiltinLatticeQuery(query.Value().Id(), query.Value().Stages().front().id, query.Value().ResultSchema().id,
                                                     "Distance points", {.stepMillimeters = 1000, .outerSteps = 2}, 7);
        REQUIRE(small.HasValue());
        distanceStage = QueryStageId::Create(12).Value();
        auto scored = AddBuiltinPointDistanceTest(small.Value(), *distanceStage, {1500, true});
        REQUIRE(scored.HasValue());
        SECTION("one distance stage") {}
        SECTION("a second filter contributes independently to ranking and debug samples") {
            filterStage = QueryStageId::Create(13).Value();
            const auto filtered = AddBuiltinPointDistanceTest(small.Value(), *filterStage, {1100, true, 3});
            REQUIRE(filtered.HasValue());
            QueryAssetSource authored{.id = scored.Value().Id(),
                                      .displayName = scored.Value().DisplayName(),
                                      .result = scored.Value().ResultSchema(),
                                      .stages = {scored.Value().Stages().begin(), scored.Value().Stages().end()}};
            authored.stages.push_back(filtered.Value().Stages().back());
            authored.stages.back().executionOrder = 2;
            scored = EnvironmentQueryAsset::Capture(authored);
            REQUIRE(scored.HasValue());
        }
        project.WriteAsset(scored.Value());
        Project::Open(controller, Project::Path);
        auto edited = pinned;
        edited.source = document.source;
        submit(edited);
        REQUIRE(document.preview.status == Running);
    }
    SECTION("the saved Physics stage samples a large lattice without changing the authoring Scene") {
        distanceStage = QueryStageId::Create(12).Value();
        visibilityStage = QueryStageId::Create(13).Value();
        const auto distance = AddBuiltinPointDistanceTest(query.Value(), *distanceStage, {50000, true, 2});
        REQUIRE(distance.HasValue());
        const auto occlusion = AddBuiltinPointVisibilityTest(distance.Value(), *visibilityStage, {1});
        REQUIRE(occlusion.HasValue());
        project.WriteAsset(occlusion.Value());
        Project::Open(controller, Project::Path);
        auto edited = pinned;
        edited.source = document.source;
        submit(edited);
        REQUIRE(document.preview.status == Running);
        CHECK(physicsCaptures == 1);
        SECTION("the ranked sample is published") {}
        SECTION("a cancelled sample leaves no retained evidence") {
            edited.action = EnvironmentQueryPreviewAction::Cancel;
            submit(edited);
            expected = Cancelled;
        }
    }
    SECTION("the retained preview is bounded, stable and leaves the Scene owner unchanged") {
        submit(pinned);
        REQUIRE(document.preview.status == Running);
        SECTION("the current point can be framed in the viewport") {
            framePoint = true;
        }
        SECTION("a Scene edit revokes a delayed frame action") {
            framePoint = true;
            editBeforeFrame = true;
        }
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
    if (framePoint) {
        REQUIRE_FALSE(preview.candidates.empty());
        const auto &sample = preview.candidates.front();
        REQUIRE(sample.point);
        const auto millimeters = sample.point->Millimeters();
        const auto initialTarget = model.viewportCamera.target;
        auto navigation = pinned;
        navigation.action = EnvironmentQueryPreviewAction::FocusCandidate;
        navigation.previewEvidence = document.preview.snapshot;
        navigation.candidate = sample.index;
        if (editBeforeFrame) {
            transform.translation.x += 1;
            controller.ProcessCommand(
                {.command = EditorWorkspaceViewCommand::CommitObjectTransform, .objectPayload = object, .transformPayload = transform});
            REQUIRE(model.documentRevision != pinned.scene);
            submit(navigation);
            CHECK(model.viewportCamera.target == initialTarget);
            CHECK(document.preview.status == Stale);
            return;
        }
        submit(navigation);
        const Math::Vec3 target{static_cast<float>(millimeters[0]) / 1000.0F, static_cast<float>(millimeters[1]) / 1000.0F,
                                static_cast<float>(millimeters[2]) / 1000.0F};
        CHECK(model.viewportCamera.target == target);
        CHECK(model.documentRevision == pinned.scene);
        CHECK_FALSE(controller.HasDirtySourceDocuments());
    }
    if (usesProvider) {
        REQUIRE(preview.candidates.size() == 9);
        CHECK(preview.candidates[4].point == locationSource.point);
        CHECK(preview.winnerCount == 5);
        CHECK(locationSource.calls == 1);
        CHECK(active->IsCurrent());
        return;
    }
    if (usesTarget) {
        REQUIRE(preview.candidates.size() == 9);
        CHECK(preview.candidates[4].point == Math::WorldCoordinate64::FromMillimeters(-1000, 0, 3000));
        CHECK(preview.winnerCount == 5);
        REQUIRE_FALSE(preview.winners.empty());
        CHECK(preview.winners.front().itemIndex == 4);
        CHECK(active->IsCurrent());
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::SelectObject, .objectPayload = object});
        controller.UpdateEnvironmentQueryPreview();
        CHECK(document.preview.status == Stale);
        CHECK_FALSE(document.preview.snapshot);
        return;
    }
    if (mixedContexts) {
        REQUIRE(preview.candidates.size() == 9);
        CHECK(preview.candidates[4].point == Math::WorldCoordinate64{});
        CHECK(preview.winnerCount == 0);
        REQUIRE(preview.stages.size() == 9);
        CHECK(preview.stages[4].score.status == QueryScoreStatus::Filtered);
        CHECK(active->IsCurrent());
        return;
    }
    if (visibilityStage) {
        constexpr std::size_t width = MaximumQueryLatticeOuterSteps * 2 + 1;
        CHECK(preview.generated == width * width);
        CHECK(preview.evaluated == MaximumQueryPhysicsVisibilityRays);
        CHECK(preview.winnerCount == MaximumQueryPhysicsVisibilityRays - 1);
        REQUIRE(preview.stages.size() == preview.candidates.size() * 2);
        CHECK(preview.stages[0].score.stage == *distanceStage);
        CHECK(preview.stages[0].score.contribution > 0.0);
        CHECK(preview.stages[1].score.stage == *visibilityStage);
        CHECK(preview.stages[1].score.status == QueryScoreStatus::Filtered);
        CHECK(preview.stages.back().score.status == QueryScoreStatus::Eligible);
        CHECK(preview.candidates.back().index == preview.generated - 1);
        CHECK(model.documentRevision == pinned.scene);
        CHECK(active->IsCurrent());
        return;
    }
    if (filterStage) {
        REQUIRE(preview.candidates.size() == 25);
        REQUIRE(preview.stages.size() == 50);
        CHECK(preview.winnerCount == 5);
        const auto &diagonalScore = preview.stages[12];
        CHECK(diagonalScore.candidateIndex == 6);
        CHECK(diagonalScore.score.stage == *distanceStage);
        CHECK(diagonalScore.score.status == QueryScoreStatus::Eligible);
        const auto &diagonalFilter = preview.stages[13];
        CHECK(diagonalFilter.candidateIndex == 6);
        CHECK(diagonalFilter.score.stage == *filterStage);
        CHECK(diagonalFilter.score.status == QueryScoreStatus::Filtered);
        CHECK(preview.stages[24].score.contribution == 0.25);
        CHECK(preview.stages[25].score.contribution == 0.75);
        REQUIRE_FALSE(preview.winners.empty());
        CHECK(preview.winners.front().itemIndex == 12);
        CHECK(preview.winners.front().scoreKey == QueryScoringPlan::ScoreKeyScale);
        CHECK(model.documentRevision == pinned.scene);
        CHECK(active->IsCurrent());
        return;
    }
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

TEST_CASE("Authoring preview ranks mixed path, distance and visibility evidence for the same saved Scene",
          "[unit][editor][eqs][workspace]") {
    Project project;
    const auto sceneAsset = Assets::AssetId::FromBytes({8});
    const std::array objects{SceneSource::SceneObjectSnapshot{.id = {1}, .name = "Query anchor"}};
    REQUIRE(std::filesystem::create_directories(project.root / ".horo"));
    std::ofstream(project.root / ".horo/project.json") << R"({"settings":{"defaultScene":"assets/main.horo"}})";
    std::ofstream(project.root / "assets/main.horo") << SceneSource::EncodeSceneSource({objects, {}});
    std::ofstream(project.root / "assets/main.horo.horo")
        << std::format(R"({{"schemaVersion":1,"assetId":"{}","assetType":"core.scene"}})", sceneAsset.ToString());
    const auto rebuilt = Assets::RebuildAssetRegistry(project.registry, project.root, Assets::AssetRegistryOpenMode::ReadOnly);
    REQUIRE(rebuilt.HasValue());
    REQUIRE(rebuilt.Value().status == Assets::AssetRegistryBuildStatus::Complete);

    const auto outerSteps = GENERATE(1U, 4U);
    const auto side = 2 * outerSteps + 1;
    const auto expectedGenerated = side * side;
    const auto expectedEvaluated = std::min<std::size_t>(expectedGenerated, EnvironmentQueryPreviewNavigation::MaximumPaths);
    const auto queryId = QueryId::Create(21).Value();
    const auto distance = QueryStageId::Create(23).Value();
    const auto visibility = QueryStageId::Create(24).Value();
    const auto path = QueryStageId::Create(25).Value();
    const auto lattice = CreateBuiltinLatticeQuery(queryId, QueryStageId::Create(22).Value(), QueryResultSchemaId::Create(26).Value(),
                                                   "Mixed preview", {.stepMillimeters = 500, .outerSteps = outerSteps}, 9);
    REQUIRE(lattice.HasValue());
    const auto ranged = AddBuiltinPointDistanceTest(lattice.Value(), distance, {4000, true, 2});
    REQUIRE(ranged.HasValue());
    const auto occlusion = AddBuiltinPointVisibilityTest(ranged.Value(), visibility, {1});
    REQUIRE(occlusion.HasValue());
    const auto query = AddBuiltinPointNavigationTest(occlusion.Value(), path, 10.0, 8);
    REQUIRE(query.HasValue());
    project.WriteAsset(query.Value());
    const auto registry = CaptureBuiltinQuerySchema();
    REQUIRE(registry.HasValue());

    const auto owner = std::this_thread::get_id();
    std::thread::id pathWorker;
    std::vector<Math::WorldCoordinate64> rayPoints;
    std::vector<Math::WorldCoordinate64> pathPoints;
    const EnvironmentQueryPreviewPhysics physics{[&](const Runtime::RuntimeSceneView &scene, const QueryPhysicsVisibilityRequest &request) {
        CHECK(scene.IsCurrent());
        CHECK(request.stage == visibility);
        REQUIRE_FALSE(request.points.empty());
        rayPoints.assign(request.points.begin(), request.points.end());
        EnvironmentQueryPreviewPhysics::Visibility clear;
        clear.fill(true);
        clear[request.points.size() - 1] = false;
        return Result<EnvironmentQueryPreviewPhysics::Visibility>::Success(clear);
    }};
    const EnvironmentQueryPreviewNavigation navigation{[&](const EnvironmentQueryPreviewNavigationRequest &request) {
        CHECK(std::this_thread::get_id() == owner);
        CHECK(request.sceneAsset == sceneAsset);
        CHECK(request.scene.Get(request.querier).HasValue());
        CHECK(request.definition.Revision() == request.scene.DefinitionRevision());
        CHECK(request.stage == path);
        return Result<EnvironmentQueryPreviewNavigation::Evaluate>::Success(
            [&](std::span<const Math::WorldCoordinate64> points, const CancellationToken &) {
            pathWorker = std::this_thread::get_id();
            pathPoints.assign(points.begin(), points.end());
            std::vector<QueryTestSample> samples;
            samples.reserve(points.size());
            for (std::size_t index = 0; index < points.size(); ++index)
                samples.push_back({.stage = path, .filter = index != 0, .metric = index == 1 ? 1.0 : 9.0});
            return Result<std::vector<QueryTestSample>>::Success(std::move(samples));
        });
    }};
    const CancellationToken token;
    REQUIRE(project.runtime.Startup(token).HasValue());
    JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 8}};
    EditorWorkspaceController controller(project.root, project.runtime, project.registry.Snapshot(),
                                         {.assets = {.mutableAssetRegistry = &project.registry},
                                          .mutations = &project.mutations,
                                          .durableFiles = &project.files,
                                          .jobs = &jobs,
                                          .querySchema = &registry.Value(),
                                          .queryPreviewPhysics = &physics,
                                          .queryPreviewNavigation = &navigation});
    REQUIRE_FALSE(controller.InitializationError());
    const auto &model = controller.ViewModel();
    REQUIRE(model.objects.size() == 1);
    const auto anchor = model.objects.front().id;
    controller.ProcessCommand({.command = EditorWorkspaceViewCommand::SelectObject, .objectPayload = anchor});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    for (std::uint64_t frame = 1; std::chrono::steady_clock::now() < deadline; ++frame) {
        REQUIRE(project.runtime.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, {frame, {}, 0.0, 0, {}, false, token})
                    .HasValue());
        controller.SynchronizeRuntimeScenePreview();
        if (project.runtime.ActiveScene() && controller.ViewportScene().documentRevision == model.documentRevision)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(project.runtime.ActiveScene());
    REQUIRE(controller.ViewportScene().documentRevision == model.documentRevision);
    const auto sceneRevision = model.documentRevision;
    const auto structureRevision = project.runtime.ActiveScene()->StructuralRevision();
    Project::Open(controller, Project::Path);
    REQUIRE(model.environmentQueryDocuments.size() == 1);
    const auto &document = model.environmentQueryDocuments.front();
    auto request = EnvironmentQueryPreviewRequest{document.identity.instance, document.source, model.documentRevision, anchor};
    controller.ProcessCommand({.command = EditorWorkspaceViewCommand::PreviewEnvironmentQuery, .environmentQueryPreview = request});
    using enum EnvironmentQueryPreviewStatus;
    INFO((document.preview.diagnostic ? document.preview.diagnostic->code.Value() : std::string{}));
    REQUIRE(document.preview.status == Running);
    jobs.Shutdown(ShutdownPolicy::Drain);
    controller.UpdateEnvironmentQueryPreview();
    REQUIRE(document.preview.status == Ready);
    CHECK(pathWorker != owner);
    CHECK(pathPoints == rayPoints);
    REQUIRE(document.preview.snapshot);
    const auto &snapshot = *document.preview.snapshot;
    CHECK(model.documentRevision == sceneRevision);
    CHECK(project.runtime.ActiveScene()->StructuralRevision() == structureRevision);
    CHECK(snapshot.generated == expectedGenerated);
    CHECK(snapshot.evaluated == expectedEvaluated);
    REQUIRE(snapshot.candidates.size() == expectedEvaluated);
    REQUIRE(pathPoints.size() == expectedEvaluated);
    CHECK(snapshot.candidates.back().index == expectedGenerated - 1);
    REQUIRE(snapshot.candidates.front().score);
    REQUIRE(snapshot.candidates.back().score);
    CHECK(snapshot.candidates.front().score->failedStage == path);
    CHECK(snapshot.candidates.back().score->failedStage == visibility);
    REQUIRE_FALSE(snapshot.winners.empty());
    CHECK(snapshot.winners.front().itemIndex == snapshot.candidates[1].index);

    request.action = EnvironmentQueryPreviewAction::FocusCandidate;
    request.previewEvidence = document.preview.snapshot;
    request.candidate = snapshot.winners.front().itemIndex;
    const auto camera = model.viewportCamera.target;
    auto transform = model.objects.front().localTransform;
    transform.translation.x += 1;
    controller.ProcessCommand(
        {.command = EditorWorkspaceViewCommand::CommitObjectTransform, .objectPayload = anchor, .transformPayload = transform});
    controller.ProcessCommand({.command = EditorWorkspaceViewCommand::PreviewEnvironmentQuery, .environmentQueryPreview = request});
    CHECK(model.viewportCamera.target == camera);
    CHECK(document.preview.status == Stale);
}

TEST_CASE("Environment Query checkpoints recover only over the same registered source and metadata", "[unit][editor][eqs][workspace]") {
    Project project;
    const auto schema = CaptureBuiltinQuerySchema();
    REQUIRE(schema.HasValue());
    const auto source =
        CreateBuiltinLatticeQuery(QueryId::Create(14).Value(), QueryStageId::Create(41).Value(), QueryResultSchemaId::Create(8).Value(),
                                  "Recoverable points", {.stepMillimeters = 500, .outerSteps = 3}, 9);
    REQUIRE(source.HasValue());
    project.WriteAsset(source.Value());
    JobSystem jobs({.ioWorkerCount = 1});
    const auto dependencies = [&](std::uint8_t owner) {
        return EditorWorkspaceDependencies{.assets = {.mutableAssetRegistry = &project.registry},
                                           .world = {.documentSessionOwner = Assets::AssetId::FromBytes({owner})},
                                           .mutations = &project.mutations,
                                           .durableFiles = &project.files,
                                           .jobs = &jobs,
                                           .querySchema = &schema.Value()};
    };
    const auto disk = [&] {
        return Assets::CaptureAssetSource(project.root, project.root / Project::Path, EnvironmentQuerySourceCodec::MaximumBytes);
    };
    const auto saved = disk();
    REQUIRE(saved.HasValue());

    using enum EnvironmentQueryEditAction;
    {
        EditorWorkspaceController writer(project.root, project.runtime, project.registry.Snapshot(), dependencies(31));
        Project::Open(writer, Project::Path);
        REQUIRE(writer.ViewModel().environmentQueryDocuments.size() == 1);
        const auto &query = writer.ViewModel().environmentQueryDocuments.front();
        REQUIRE(query.settings);
        auto changed = *query.settings;
        changed.lattice.stepMillimeters = 800;
        writer.ProcessCommand(MakeEnvironmentQueryEditCommand(query, Apply, changed));
        REQUIRE(query.dirty);
        writer.UpdateAutosave(60.0F, 1);
    }

    REQUIRE(disk().Value().identity == saved.Value().identity);

    EditorWorkspaceController reopened(project.root, project.runtime, project.registry.Snapshot(), dependencies(32));
    Project::Open(reopened, Project::Path);
    REQUIRE(reopened.ViewModel().environmentQueryDocuments.size() == 1);
    const auto &query = reopened.ViewModel().environmentQueryDocuments.front();
    REQUIRE(query.recovery);
    const auto inspected = query.recovery;
    REQUIRE(inspected->acceptedSource == saved.Value().identity);
    DocumentRecoveryService service(project.root, project.mutations, project.files);
    const auto restore = [&] {
        reopened.ProcessCommand(MakeEnvironmentQueryEditCommand(query, RestoreRecovery));
    };

    SECTION("restore leaves canonical bytes untouched until explicit save") {
        restore();
        REQUIRE_FALSE(query.diagnostic);
        REQUIRE(query.dirty);
        CHECK_FALSE(query.recovery);
        CHECK_FALSE(query.canUndo);
        CHECK_FALSE(query.canRedo);
        CHECK(query.settings->lattice.stepMillimeters == 800);
        CHECK(disk().Value().identity == saved.Value().identity);
        reopened.FlushAutosave();
        REQUIRE(service.Inspect(query.asset, query.identity.key).Value());
        reopened.ProcessCommand(MakeEnvironmentQueryEditCommand(query, Save));
        REQUIRE_FALSE(query.diagnostic);
        CHECK_FALSE(query.dirty);
        CHECK(disk().Value().identity != saved.Value().identity);
        CHECK_FALSE(service.Inspect(query.asset, query.identity.key).Value());
    }
    SECTION("a sidecar change preserves both the canonical asset and the checkpoint") {
        {
            std::ofstream metadata(project.root / (std::string{Project::Path} + ".horo"), std::ios::binary | std::ios::app);
            metadata << '\n';
            REQUIRE(static_cast<bool>(metadata));
        }
        restore();
        REQUIRE(query.diagnostic);
        CHECK(query.recovery == inspected);
        CHECK_FALSE(query.dirty);
        CHECK(query.settings->lattice.stepMillimeters == 500);
        CHECK(disk().Value().identity == saved.Value().identity);
        CHECK(service.Inspect(query.asset, query.identity.key).Value() == *inspected);
        reopened.ProcessCommand(MakeEnvironmentQueryEditCommand(query, DiscardRecovery));
        CHECK_FALSE(query.recovery);
        CHECK_FALSE(service.Inspect(query.asset, query.identity.key).Value());
    }
    SECTION("a newer canonical Query is preserved when its checkpoint is declined") {
        const auto external =
            CreateBuiltinLatticeQuery(QueryId::Create(14).Value(), QueryStageId::Create(41).Value(), QueryResultSchemaId::Create(8).Value(),
                                      "External points", {.stepMillimeters = 900, .outerSteps = 3}, 9);
        REQUIRE(external.HasValue());
        project.WriteAsset(external.Value());
        const auto newer = disk();
        REQUIRE(newer.HasValue());
        REQUIRE(newer.Value().identity != saved.Value().identity);
        restore();
        REQUIRE(query.diagnostic);
        CHECK(query.recovery == inspected);
        CHECK_FALSE(query.dirty);
        CHECK(disk().Value().identity == newer.Value().identity);
        reopened.ProcessCommand(MakeEnvironmentQueryEditCommand(query, DiscardRecovery));
        CHECK_FALSE(query.recovery);
        CHECK(disk().Value().identity == newer.Value().identity);
    }
}

TEST_CASE("Registered Query settings use shared history and tracked source publication", "[unit][editor][eqs][workspace]") {
    Project project;
    const auto registry = CaptureBuiltinQuerySchema();
    REQUIRE(registry.HasValue());
    const auto initial =
        CreateBuiltinLatticeQuery(QueryId::Create(14).Value(), QueryStageId::Create(41).Value(), QueryResultSchemaId::Create(8).Value(),
                                  "Editable points", {.stepMillimeters = 500, .outerSteps = 3}, 9);
    REQUIRE(initial.HasValue());
    const auto first = AddBuiltinPointDistanceTest(initial.Value(), QueryStageId::Create(52).Value(), {1500, true});
    REQUIRE(first.HasValue());
    const auto native = AddBuiltinPointDistanceTest(first.Value(), QueryStageId::Create(53).Value(), {1200, false});
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
    REQUIRE(document.settings);
    REQUIRE(document.settings->distanceTests.size() == 2);
    CHECK(document.settings->distanceTests[0].test.scoreWeight == 1);
    CHECK(document.settings->distanceTests[1].test.scoreWeight == 1);
    const auto original = document.query;
    const auto instance = document.identity.instance;
    using enum EnvironmentQueryEditAction;

    auto changed = *document.settings;
    const auto issue = [&](EnvironmentQueryEditAction action, const std::optional<BuiltinPointQuerySettings> &values = {},
                           EnvironmentQueryStageChange change = {}) {
        controller.ProcessCommand(MakeEnvironmentQueryEditCommand(document, action, values, change));
    };
    changed.lattice.stepMillimeters = 0;
    issue(Apply, changed);
    REQUIRE(document.diagnostic);
    CHECK(document.query == original);
    CHECK_FALSE(document.canUndo);
    changed.lattice = {.stepMillimeters = 800, .outerSteps = 4, .innerSteps = 1};
    changed.maximumResults = 7;
    changed.distanceTests[0].test = {2400, false, 2};
    const auto worldOrigin = BuiltinQueryContextId(BuiltinQueryContext::WorldOrigin);
    changed.distanceTests[1].test = {1900, true, 3, worldOrigin};
    auto invalidWeight = changed;
    invalidWeight.distanceTests[1].test.scoreWeight = 0;
    issue(Apply, invalidWeight);
    REQUIRE(document.diagnostic);
    CHECK(document.query == original);
    auto foreign = changed;
    foreign.distanceTests[1].stage = QueryStageId::Create(99).Value();
    issue(Apply, foreign);
    REQUIRE(document.diagnostic);
    CHECK(document.query == original);
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
        REQUIRE(reopened.settings);
        REQUIRE(reopened.settings->distanceTests.size() == 2);
        CHECK(reopened.identity.instance != instance);
        CHECK(reopened.asset == project.asset);
        CHECK(reopened.query->Id() == original->Id());
        CHECK(reopened.query->Stages().back().id == native.Value().Stages().back().id);
        CHECK(reopened.settings->lattice.stepMillimeters == 800);
        CHECK(reopened.settings->lattice.innerSteps == 1);
        CHECK(reopened.settings->maximumResults == 7);
        CHECK(reopened.settings->distanceTests[0].stage == native.Value().Stages()[1].id);
        CHECK(reopened.settings->distanceTests[0].test.radiusMillimeters == 2400);
        CHECK_FALSE(reopened.settings->distanceTests[0].test.preferNearer);
        CHECK(reopened.settings->distanceTests[0].test.scoreWeight == 2);
        CHECK(reopened.settings->distanceTests[1].stage == native.Value().Stages()[2].id);
        CHECK(reopened.settings->distanceTests[1].test.radiusMillimeters == 1900);
        CHECK(reopened.settings->distanceTests[1].test.preferNearer);
        CHECK(reopened.settings->distanceTests[1].test.scoreWeight == 3);
        CHECK(reopened.settings->distanceTests[1].test.center == worldOrigin);
        CHECK_FALSE(reopened.canUndo);
    }
    SECTION("insertion and removal replay and save alongside ordinary field changes") {
        changed.lattice.stepMillimeters = 640;
        const auto append = MakeEnvironmentQueryEditCommand(document, Apply, changed, QueryPointDistanceTest{3500, true, 4});
        controller.ProcessCommand(append);
        REQUIRE_FALSE(document.diagnostic);
        REQUIRE(document.settings->distanceTests.size() == 3);
        const auto appended = document.settings->distanceTests.back().stage;
        CHECK(appended != draft->Stages()[1].id);
        CHECK(appended != draft->Stages()[2].id);
        CHECK(document.settings->lattice.stepMillimeters == 640);
        CHECK(document.settings->distanceTests.back().test.scoreWeight == 4);
        const auto expanded = document.query;

        controller.ProcessCommand(append);
        CHECK(document.diagnostic);
        CHECK(document.query == expanded);
        issue(Apply, document.settings, draft->Stages().front().id);
        CHECK(document.diagnostic);
        CHECK(document.query == expanded);
        issue(Apply, document.settings, draft->Stages()[1].id);
        REQUIRE_FALSE(document.diagnostic);
        REQUIRE(document.settings->distanceTests.size() == 2);
        CHECK(document.settings->distanceTests.front().stage == draft->Stages()[2].id);
        const auto reduced = document.query;
        issue(Undo);
        CHECK(document.query == expanded);
        issue(Undo);
        CHECK(document.query == draft);
        issue(Redo);
        CHECK(document.query == expanded);
        issue(Redo);
        CHECK(document.query == reduced);

        REQUIRE(controller.SaveSourceDocuments().HasValue());
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = instance});
        REQUIRE(documents.empty());
        Project::Open(controller, Project::Path);
        REQUIRE(documents.size() == 1);
        const auto &reopened = documents.front();
        REQUIRE(reopened.settings);
        REQUIRE(reopened.settings->distanceTests.size() == 2);
        CHECK(reopened.settings->lattice.stepMillimeters == 640);
        CHECK(reopened.settings->distanceTests.front().stage == draft->Stages()[2].id);
        CHECK(reopened.settings->distanceTests.back().stage == appended);
        CHECK(reopened.settings->distanceTests.back().test.radiusMillimeters == 3500);
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
    SECTION("explicit reload resets history only after staging a compatible external Query") {
        project.WriteQuery("External read-only Query");
        Project::Open(controller, Project::Path);
        REQUIRE(document.conflict);
        const auto revision = document.revision;
        issue(ReloadExternal);
        CHECK(document.revision == revision);
        CHECK(document.query == draft);
        CHECK(document.dirty);
        CHECK(document.canUndo);

        const auto replacement =
            CreateBuiltinLatticeQuery(QueryId::Create(14).Value(), QueryStageId::Create(41).Value(), QueryResultSchemaId::Create(8).Value(),
                                      "External lattice", {.stepMillimeters = 750, .outerSteps = 3}, 9);
        REQUIRE(replacement.HasValue());
        project.WriteAsset(replacement.Value());
        controller.ProcessCommand({.command = EditorWorkspaceViewCommand::EditEnvironmentQuery,
                                   .environmentQueryEdit = EnvironmentQueryEditRequest{{instance, revision - 1}, ReloadExternal}});
        CHECK(document.query == draft);
        issue(ReloadExternal);
        CHECK(document.revision == revision + 1);
        CHECK(document.query->DisplayName() == "External lattice");
        CHECK_FALSE(document.conflict);
        CHECK_FALSE(document.canUndo);
        CHECK_FALSE(document.canRedo);
        CHECK_FALSE(controller.HasDirtySourceDocuments());
    }
    SECTION("three-way merge keeps local stages and the external display name until explicit save") {
        const auto &base = native.Value();
        const auto external =
            EnvironmentQueryAsset::Capture({.id = base.Id(),
                                            .displayName = "External label",
                                            .result = base.ResultSchema(),
                                            .stages = std::vector<QueryAssetStage>(base.Stages().begin(), base.Stages().end())});
        REQUIRE(external.HasValue());
        project.WriteAsset(external.Value());
        const auto observed = disk();
        REQUIRE(observed.HasValue());
        Project::Open(controller, Project::Path);
        REQUIRE(document.conflict);
        issue(MergeExternal);
        REQUIRE_FALSE(document.diagnostic);
        CHECK_FALSE(document.conflict);
        CHECK_FALSE(document.canUndo);
        CHECK(document.dirty);
        CHECK(document.query->DisplayName() == "External label");
        CHECK(document.settings->lattice.stepMillimeters == 800);
        CHECK(document.settings->distanceTests.front().test.radiusMillimeters == 2400);
        CHECK(disk().Value().identity == observed.Value().identity);
        issue(Save);
        REQUIRE_FALSE(document.diagnostic);
        CHECK_FALSE(document.dirty);
        CHECK(disk().Value().identity != observed.Value().identity);
    }
    SECTION("keep local fences the next save after a competing stage edit") {
        const auto settings = AI::ReadBuiltinPointQuerySettings(native.Value(), registry.Value());
        REQUIRE(settings.HasValue());
        auto outside = settings.Value();
        outside.lattice.stepMillimeters = 900;
        const auto external = AI::EditBuiltinPointQuerySettings(native.Value(), registry.Value(), outside);
        REQUIRE(external.HasValue());
        project.WriteAsset(external.Value());
        const auto observed = disk();
        REQUIRE(observed.HasValue());
        Project::Open(controller, Project::Path);
        REQUIRE(document.conflict);
        issue(MergeExternal);
        REQUIRE(document.diagnostic);
        CHECK(document.conflict);
        CHECK(document.canUndo);
        CHECK(document.query == draft);
        issue(KeepLocal);
        REQUIRE_FALSE(document.diagnostic);
        CHECK_FALSE(document.conflict);
        CHECK_FALSE(document.canUndo);
        CHECK(document.query == draft);
        CHECK(disk().Value().identity == observed.Value().identity);
        outside.lattice.stepMillimeters = 1000;
        const auto newer = AI::EditBuiltinPointQuerySettings(native.Value(), registry.Value(), outside);
        REQUIRE(newer.HasValue());
        project.WriteAsset(newer.Value());
        const auto latest = disk();
        REQUIRE(latest.HasValue());
        issue(Save);
        REQUIRE(document.conflict);
        CHECK(disk().Value().identity == latest.Value().identity);
        issue(KeepLocal);
        issue(Save);
        REQUIRE_FALSE(document.diagnostic);
        CHECK_FALSE(document.dirty);
        CHECK(document.settings->lattice.stepMillimeters == 800);
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
    SECTION("Physics and Navigation stages retain identity through edits, history, save, and cook") {
        const auto command = AssetBrowserInteractionSession::ConfirmCreateQuery(draft);
        REQUIRE(command);
        controller.ProcessCommand(*command);
        const auto &documents = controller.ViewModel().environmentQueryDocuments;
        REQUIRE(documents.size() == 1);
        const auto &document = documents.front();
        REQUIRE(document.settings);
        const auto original = document.query;
        using enum EnvironmentQueryEditAction;
        const auto edit = [&](EnvironmentQueryStageChange change, std::optional<BuiltinPointQuerySettings> values = std::nullopt) {
            controller.ProcessCommand(
                MakeEnvironmentQueryEditCommand(document, Apply, values ? std::move(values) : document.settings, change));
            REQUIRE_FALSE(document.diagnostic);
        };
        const std::array<std::uint8_t, 16> channel{1};
        edit(EnvironmentQueryVisibilityStageAddition{channel});
        REQUIRE(document.settings->visibility);
        const auto visibility = document.settings->visibility->stage;
        edit(EnvironmentQueryNavigationStageAddition{25.0});
        REQUIRE(document.settings->navigation);
        const auto navigation = document.settings->navigation->stage;
        CHECK(visibility != navigation);
        CHECK(document.settings->navigation->scoreWeight == 1);
        edit(QueryPointDistanceTest{1000, true, 2});
        REQUIRE(document.settings->distanceTests.size() == 1);

        auto changed = *document.settings;
        changed.navigation->maximumScoredCost = 50.0;
        changed.navigation->scoreWeight = 7;
        changed.visibility->channel = {2};
        changed.lattice.stepMillimeters = 350;
        edit({}, changed);
        CHECK(document.settings->navigation->maximumScoredCost == 50.0);
        CHECK(document.settings->visibility->channel == changed.visibility->channel);
        CHECK(document.settings->lattice.stepMillimeters == 350);
        CHECK(document.settings->navigation->stage == navigation);
        CHECK(document.settings->visibility->stage == visibility);
        CHECK(document.query->Stages().front().id == original->Stages().front().id);
        const auto mixed = document.query;

        edit(visibility);
        CHECK_FALSE(document.settings->visibility);
        REQUIRE(document.settings->navigation);
        CHECK(document.settings->navigation->stage == navigation);
        controller.ProcessCommand(MakeEnvironmentQueryEditCommand(document, Undo));
        CHECK(document.query == mixed);
        REQUIRE(document.settings->visibility);
        controller.ProcessCommand(MakeEnvironmentQueryEditCommand(document, Redo));
        CHECK_FALSE(document.settings->visibility);
        controller.ProcessCommand(MakeEnvironmentQueryEditCommand(document, Undo));
        controller.ProcessCommand(MakeEnvironmentQueryEditCommand(document, Save));
        REQUIRE_FALSE(document.diagnostic);
        REQUIRE_FALSE(document.dirty);
        controller.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = document.identity.instance});
        REQUIRE(documents.empty());
        Project::Open(controller, path);
        REQUIRE(documents.size() == 1);
        const auto &reopened = documents.front();
        REQUIRE(reopened.settings);
        REQUIRE(reopened.settings->visibility);
        REQUIRE(reopened.settings->navigation);
        CHECK(reopened.settings->visibility->stage == visibility);
        CHECK(reopened.settings->navigation->stage == navigation);
        CHECK(reopened.settings->visibility->channel == changed.visibility->channel);
        CHECK(reopened.settings->navigation->maximumScoredCost == 50.0);
        CHECK(reopened.settings->navigation->scoreWeight == 7);
        REQUIRE(reopened.settings->distanceTests.size() == 1);
        CHECK(reopened.settings->distanceTests.front().test.scoreWeight == 2);
        const auto encoded = EnvironmentQueryCookedCodec::Encode(*reopened.query, registry.Value());
        REQUIRE(encoded.HasValue());
        const auto cooked = EnvironmentQueryCookedCodec::Decode(encoded.Value(), registry.Value());
        REQUIRE(cooked.HasValue());
        CHECK(UsesBuiltinPointVisibility(cooked.Value()));
        CHECK(UsesBuiltinPointNavigation(cooked.Value()));
    }
    SECTION("Authored and surface generators retain settings through registered save and cook") {
        using enum EnvironmentQueryPointGenerator;
        draft.pointGenerator = GENERATE(Authored, NavigationSurface);
        draft.distance = true;
        const auto command = AssetBrowserInteractionSession::ConfirmCreateQuery(draft);
        REQUIRE(command);
        controller.ProcessCommand(*command);
        const auto &documents = controller.ViewModel().environmentQueryDocuments;
        REQUIRE(documents.size() == 1);
        const auto &document = documents.front();
        REQUIRE(document.settings);
        const auto stage = document.query->Stages().front().id;
        const auto oldSource = document.query;
        auto changed = *document.settings;
        auto *surface = std::get_if<QueryNavigationSurfaceSettings>(&changed.alternateGenerator);
        if (surface) {
            surface->radiusMillimeters = 3000;
            surface->maximumCandidates = 12;
        } else {
            auto &points = std::get<std::vector<Math::WorldCoordinate64>>(changed.alternateGenerator);
            points.insert(points.begin(), Math::WorldCoordinate64::FromMillimeters(-9223372036854775807LL, 12, 34));
        }
        using enum EnvironmentQueryEditAction;
        controller.ProcessCommand(MakeEnvironmentQueryEditCommand(document, Apply, changed));
        REQUIRE_FALSE(document.diagnostic);
        CHECK(document.dirty);
        CHECK(document.query->Stages().front().id == stage);
        controller.ProcessCommand(MakeEnvironmentQueryEditCommand(document, Undo));
        CHECK(document.query == oldSource);
        controller.ProcessCommand(MakeEnvironmentQueryEditCommand(document, Redo));
        controller.ProcessCommand(MakeEnvironmentQueryEditCommand(document, Save));
        REQUIRE_FALSE(document.diagnostic);
        REQUIRE_FALSE(document.dirty);
        controller.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = document.identity.instance});
        REQUIRE(documents.empty());
        Project::Open(controller, path);
        REQUIRE(documents.size() == 1);
        const auto &reopened = documents.front();
        REQUIRE(reopened.settings);
        if (surface) {
            const auto &settings = std::get<QueryNavigationSurfaceSettings>(reopened.settings->alternateGenerator);
            CHECK(settings.radiusMillimeters == surface->radiusMillimeters);
            CHECK(settings.maximumCandidates == surface->maximumCandidates);
        } else {
            using Points = std::vector<Math::WorldCoordinate64>;
            CHECK(std::get<Points>(reopened.settings->alternateGenerator) == std::get<Points>(changed.alternateGenerator));
        }
        REQUIRE(reopened.settings->distanceTests.size() == 1);
        const auto encoded = EnvironmentQueryCookedCodec::Encode(*reopened.query, registry.Value());
        REQUIRE(encoded.HasValue());
        const auto cooked = EnvironmentQueryCookedCodec::Decode(encoded.Value(), registry.Value());
        REQUIRE(cooked.HasValue());
        CHECK(UsesBuiltinNavigationSurface(cooked.Value()) == (surface != nullptr));
    }
    SECTION("Perception creation and history publish the exact cooked actor query") {
        draft.perceivedActors = true;
        draft.currentlySensed = false;
        draft.stepMillimeters = 0;
        const auto command = AssetBrowserInteractionSession::ConfirmCreateQuery(draft);
        REQUIRE(command);
        controller.ProcessCommand(*command);
        REQUIRE(controller.ViewModel().contentBrowserOperationError.empty());
        const auto &documents = controller.ViewModel().environmentQueryDocuments;
        REQUIRE(documents.size() == 1);
        const auto &document = documents.front();
        REQUIRE(document.perception);
        CHECK_FALSE(document.settings);
        CHECK(document.perception->match == QueryPerceptionMatch::Remembered);
        CHECK(document.perception->maximumResults == 7);
        const auto original = document.query;
        using enum EnvironmentQueryEditAction;
        auto apply = MakeEnvironmentQueryEditCommand(document, Apply);
        apply.environmentQueryEdit->perception = BuiltinPerceptionQuerySettings{QueryPerceptionMatch::CurrentlySensed, 3};
        controller.ProcessCommand(apply);
        REQUIRE_FALSE(document.diagnostic);
        CHECK(document.dirty);
        CHECK(document.perception->match == QueryPerceptionMatch::CurrentlySensed);
        CHECK(document.perception->maximumResults == 3);
        const auto edited = document.query;
        controller.ProcessCommand(apply);
        REQUIRE(document.diagnostic);
        CHECK(document.query == edited);
        controller.ProcessCommand(MakeEnvironmentQueryEditCommand(document, Undo));
        CHECK(document.query == original);
        controller.ProcessCommand(MakeEnvironmentQueryEditCommand(document, Redo));
        CHECK(document.query == edited);
        controller.ProcessCommand(MakeEnvironmentQueryEditCommand(document, Save));
        REQUIRE_FALSE(document.diagnostic);
        REQUIRE_FALSE(document.dirty);
        controller.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = document.identity.instance});
        REQUIRE(documents.empty());
        Project::Open(controller, path);
        REQUIRE(documents.size() == 1);
        const auto &saved = *documents.front().query;
        CHECK(saved.Id() == original->Id());
        CHECK(saved.ResultSchema().id == original->ResultSchema().id);
        REQUIRE(saved.Stages().size() == original->Stages().size());
        for (std::size_t index = 0; index < saved.Stages().size(); ++index)
            CHECK(saved.Stages()[index].id == original->Stages()[index].id);
        const auto encoded = EnvironmentQueryCookedCodec::Encode(saved, registry.Value());
        REQUIRE(encoded.HasValue());
        const auto plan = EnvironmentQueryCookedCodec::Decode(encoded.Value(), registry.Value());
        REQUIRE(plan.HasValue());
        CHECK(UsesBuiltinPerceptionQuery(plan.Value()));
        CHECK(plan.Value().ResultSchema().maximumResults == 3);
        REQUIRE(documents.front().perception);
        CHECK(documents.front().perception->match == QueryPerceptionMatch::CurrentlySensed);
    }
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
