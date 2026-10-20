#include "Horo/Scene/PrefabComponentProjection.h"
#include "NavigationSettingsProjectTestSupport.h"
#include "SceneDocumentTestSupport.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <fstream>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Tests::NavigationSettingsSupport;

    /** @brief Own actual project bytes, registered descriptors, source capture and the one canonical workspace owner. */
    struct Workspace final {
        SessionProjectFixture project;
        Runtime::RuntimeSceneService runtime;
        std::unique_ptr<EditorWorkspaceController> controller;
        std::size_t captures{};

        explicit Workspace(bool prefab = false) {
            std::filesystem::create_directories(project.root / ".horo");
            std::ofstream(project.root / ".horo/project.json") << R"({"settings":{"defaultScene":"assets/a.horo"}})";
            if (prefab)
                AddPrefab();
            EditorWorkspaceDependencies dependencies{.mutableAssetRegistry = &project.registry,
                                                     .mutations = &project.mutations,
                                                     .durableFiles = &project.files,
                                                     .documentSessionOwner = Asset(9)};
            dependencies.prefabSources = [this](const auto &registry, const auto roots, const auto &limits, const auto &cancellation) {
                ++captures;
                auto lease = project.mutations.TryAcquire({project.root, ProjectMutationOwner::Asset, "navigation.inspector.test"});
                if (lease.HasError())
                    return Result<PreparedPrefabHierarchySources>::Failure(lease.ErrorValue());
                auto sources = CapturePrefabHierarchySources(project.root, registry, roots, limits,
                                                             SceneDocumentTestSupport::ScenePrefabVersion(), cancellation);
                if (sources.HasError())
                    return Result<PreparedPrefabHierarchySources>::Failure(sources.ErrorValue());
                return Result<PreparedPrefabHierarchySources>::Success({std::move(sources).Value(), std::move(lease).Value()});
            };
            controller = std::make_unique<EditorWorkspaceController>(project.root, runtime, project.registry.Snapshot(), dependencies);
            REQUIRE_FALSE(controller->InitializationError());
            REQUIRE(controller->ViewModel().navigationDefinitions.size() == 1);
        }

        /** @brief Write a genuine canonical navigation Prefab source/sidecar and containing Scene placement. */
        void AddPrefab() {
            SceneSource::SceneObjectComponentSet typed;
            typed.navigationAgent = Runtime::NavigationAgentComponent{.profile = Id<Navigation::NavigationAgentProfileId>(1),
                                                                      .filter = Id<Navigation::NavigationFilterId>(4)};
            const auto raw = SceneSource::EncodePrefabNavigationComponent(Runtime::BuiltinSceneComponentKind::NavigationAgent,
                                                                          Prefab::PrefabComponentInstanceId::Create(1).Value(), typed);
            REQUIRE(raw.HasValue());
            const auto prefab =
                SceneDocumentTestSupport::ScenePrefabDocument(Asset(3),
                                                              {{.localId = {0}, .name = "Navigation member", .components = {raw.Value()}}});
            const auto bytes = prefab.SerializeCanonical();
            REQUIRE(bytes.HasValue());
            std::ofstream(project.root / "assets/agent.prefab") << bytes.Value();
            std::ofstream(project.root / "assets/agent.prefab.horo")
                << "{\"schemaVersion\":1,\"assetId\":\"" << Asset(3).ToString() << "\",\"assetType\":\"core.prefab\"}";
            REQUIRE(project.registry
                        .Publish({Record(1, "core.navmesh", "assets/nav.horoasset"), Record(2, "core.scene", "assets/a.horo"),
                                  Record(3, "core.prefab", "assets/agent.prefab")})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            const std::vector<SceneSource::ScenePrefabInstance> placements{
                {.instanceId = Prefab::PrefabInstanceId::Create(1).Value(),
                 .sourcePrefab = Prefab::PrefabAssetReference::Create(Asset(3)).Value()}};
            std::ofstream(project.root / "assets/a.horo") << SceneSource::EncodeSceneSource({Objects(), placements});
        }

        /** @brief Alter semantic source content while retaining the same registered asset/sidecar identity. */
        void ChangeSource() const {
            const auto path = project.root / "assets/agent.prefab";
            std::ifstream input(path);
            std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
            input.close();
            const auto position = text.find("Navigation member");
            REQUIRE(position != std::string::npos);
            text.replace(position, std::string_view{"Navigation member"}.size(), "Changed member");
            std::ofstream(path) << text;
        }

        /** @brief Select through the actual selection command owner. */
        void Select(std::vector<SceneObjectId> objects) {
            EditorWorkspaceViewCommandData command;
            command.command = EditorWorkspaceViewCommand::SelectObject;
            command.objectSelection = ObjectSelectionRequest{objects, objects.empty() ? std::optional<SceneObjectId>{} : objects.front()};
            controller->ProcessCommand(command);
        }

        /** @brief Capture a gesture with actual owner, selection, source and descriptor publications. */
        EditorWorkspaceViewCommandData Request(std::vector<NavigationInspectorEdit> edits) const {
            const auto &view = controller->ViewModel();
            EditorWorkspaceViewCommandData command;
            command.command = EditorWorkspaceViewCommand::UpdateNavigationComponents;
            command.navigationPayload =
                NavigationInspectorRequest{view.physicsContext,  view.selectionRevision,     view.selectedObjects, view.prefabSelection,
                                           view.prefabHierarchy, view.navigationDefinitions, std::move(edits)};
            return command;
        }

        /** @brief Select the exact expanded root member without mutating source or ordinary object selection. */
        void SelectPrefab() {
            const auto &view = controller->ViewModel();
            REQUIRE(view.prefabHierarchy);
            const auto member = std::ranges::find_if(view.prefabHierarchy->entries, [](const auto &entry) {
                return entry.target.member.has_value();
            });
            REQUIRE(member != view.prefabHierarchy->entries.end());
            controller->ProcessCommand(
                {.command = EditorWorkspaceViewCommand::PrefabHierarchy,
                 .prefabHierarchy =
                     PrefabHierarchyCommand{view.prefabHierarchy->documentSession, view.prefabHierarchy->publication,
                                            view.prefabHierarchy->documentRevision, member->target, PrefabHierarchyAction::Select}});
        }
    };
}  // namespace

TEST_CASE("Navigation workspace dispatch publishes one multi-selection history gesture", "[editor][navigation][inspector][native]") {
    Workspace workspace;
    auto &owner = *workspace.controller;
    workspace.Select({{2}, {3}});
    auto agent = *owner.ViewModel().objects[1].components.navigationAgent;
    agent.radiusOverride = 0.8F;
    auto modifier = *owner.ViewModel().objects[2].components.navigationModifier;
    modifier.area = Id<Navigation::NavigationAreaId>(5);
    auto command = workspace.Request({{{2}, agent}, {{3}, modifier}});
    const auto before = owner.ViewModel().documentRevision;
    std::size_t events{};
    auto subscription = owner.DataBus().Subscribe<SceneDocumentChangedEvent>([&](const auto &) {
        ++events;
    });
    owner.ProcessCommand(command);
    CHECK(owner.ViewModel().navigationEditError.empty());
    CHECK(events == 1);
    CHECK(owner.ViewModel().documentRevision.value == before.value + 1);
    CHECK(owner.ViewModel().objects[1].components.navigationAgent->radiusOverride == 0.8F);
    owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
    CHECK_FALSE(owner.ViewModel().objects[1].components.navigationAgent->radiusOverride);
    owner.ProcessCommand({.command = EditorWorkspaceViewCommand::RedoScene});
    CHECK(owner.ViewModel().objects[1].components.navigationAgent->radiusOverride == 0.8F);
    owner.ProcessCommand(command);
    CHECK_FALSE(owner.ViewModel().navigationEditError.empty());
    CHECK(events == 3);
}

TEST_CASE("Navigation workspace rejects unauthorized stale and invalid gestures", "[editor][navigation][inspector][native]") {
    Workspace workspace;
    auto &owner = *workspace.controller;
    workspace.Select({{2}});
    auto agent = *owner.ViewModel().objects[1].components.navigationAgent;
    agent.radiusOverride = 0.8F;
    auto command = workspace.Request({{{2}, agent}});
    const auto before = owner.ViewModel().documentRevision;
    SECTION("selection expired") {
        workspace.Select({{3}});
    }
    SECTION("wrong owner") {
        ++command.navigationPayload->context.session.generation;
    }
    SECTION("permission denied") {
        command.navigationPayload->context.canEdit = false;
    }
    SECTION("cancelled") {
        CancellationSource stop;
        stop.RequestCancellation();
        command.navigationPayload->context.cancellation = stop.Token();
    }
    SECTION("descriptor publication") {
        command.navigationPayload->definitions.clear();
    }
    SECTION("invalid draft") {
        std::get<Runtime::NavigationAgentComponent>(command.navigationPayload->edits[0].value).radiusOverride = -1;
    }
    SECTION("foreign selected target") {
        command.navigationPayload->edits[0].object = {3};
    }
    owner.ProcessCommand(command);
    CHECK_FALSE(owner.ViewModel().navigationEditError.empty());
    CHECK(owner.ViewModel().documentRevision == before);
    CHECK_FALSE(owner.ViewModel().objects[1].components.navigationAgent->radiusOverride);
}

TEST_CASE("Navigation Prefab Inspector routes actual member edits through fresh source capture and canonical history",
          "[editor][navigation][prefab][native]") {
    Workspace workspace{true};
    auto &owner = *workspace.controller;
    workspace.SelectPrefab();
    Runtime::NavigationAgentComponent agent{.profile = Id<Navigation::NavigationAgentProfileId>(1),
                                            .filter = Id<Navigation::NavigationFilterId>(4),
                                            .radiusOverride = 0.8F};
    const auto before = owner.ViewModel().documentRevision;
    const auto captures = workspace.captures;
    auto command = workspace.Request({{{}, agent}});
    owner.ProcessCommand(command);
    INFO(owner.ViewModel().navigationEditError);
    CHECK(owner.ViewModel().navigationEditError.empty());
    CHECK(owner.ViewModel().documentRevision.value == before.value + 1);
    CHECK(workspace.captures == captures + 1);
    const auto &instances = owner.ViewModel().prefabHierarchy->sources->instances;
    REQUIRE(instances[0].authored.valueOverrides);
    CHECK(instances[0].authored.valueOverrides->componentRecords.size() == 2);
    owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
    CHECK_FALSE(owner.ViewModel().prefabHierarchy->sources->instances[0].authored.valueOverrides);
    owner.ProcessCommand({.command = EditorWorkspaceViewCommand::RedoScene});
    CHECK(owner.ViewModel().prefabHierarchy->sources->instances[0].authored.valueOverrides.has_value());
    owner.ProcessCommand(command);
    CHECK_FALSE(owner.ViewModel().navigationEditError.empty());
}

TEST_CASE("Navigation Prefab Inspector rejects source changes and forged component ownership", "[editor][navigation][prefab][native]") {
    Workspace workspace{true};
    auto &owner = *workspace.controller;
    workspace.SelectPrefab();
    Runtime::NavigationAgentComponent agent{.profile = Id<Navigation::NavigationAgentProfileId>(1),
                                            .filter = Id<Navigation::NavigationFilterId>(4),
                                            .radiusOverride = 0.8F};
    auto command = workspace.Request({{{}, agent}});
    const auto before = owner.ViewModel().documentRevision;
    SECTION("source bytes changed") {
        workspace.ChangeSource();
    }
    SECTION("foreign component schema") {
        std::get<Runtime::NavigationAgentComponent>(command.navigationPayload->edits[0].value).schemaVersion = 99;
    }
    SECTION("forged object") {
        command.navigationPayload->edits[0].object = {2};
    }
    SECTION("duplicate component target") {
        command.navigationPayload->edits.push_back(command.navigationPayload->edits.front());
    }
    owner.ProcessCommand(command);
    CHECK_FALSE(owner.ViewModel().navigationEditError.empty());
    CHECK(owner.ViewModel().documentRevision == before);
    CHECK_FALSE(owner.ViewModel().prefabHierarchy->sources->instances[0].authored.valueOverrides);
}
