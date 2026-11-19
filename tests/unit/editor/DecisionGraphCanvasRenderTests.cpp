#include "Horo/AI/BlackboardUsageInspection.h"
#include "Horo/AI/DecisionGraphCompiler.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/screens/workspace/DecisionGraphCanvas.h"
#include "editor/screens/workspace/DecisionGraphCanvasModel.h"
#include "editor/screens/workspace/DecisionProblemsView.h"
#include "editor/screens/workspace/DecisionTaskPalette.h"
#include "editor/screens/workspace/DecisionTaskPropertiesView.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserDialogs.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserInteractionSession.h"
#include "support/DecisionGraphCompilerTestSupport.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <imgui.h>
#include <imgui_internal.h>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string_view>

namespace Horo::Editor {
    namespace {
        /** @brief Provides long labels without any native localization/filesystem setup. */
        class CanvasLabels final : public ILocalizationService {
        public:
            const std::string &Get(std::string_view, std::string_view key) const override {
                return labels_.try_emplace(std::string{key}, "Localized graph label " + std::string{key}).first->second;
            }

            void Clear() {
                labels_.clear();
            }

            bool Used(std::string_view key) const {
                return labels_.contains(std::string{key});
            }

        private:
            mutable std::map<std::string, std::string> labels_;
        };

        /** @brief Owns real offscreen ImGui and the production adapter in the correct destruction order. */
        struct CanvasFixture final {
            std::unique_ptr<ImGuiContext, void (*)(ImGuiContext *)> imgui{ImGui::CreateContext(), &ImGui::DestroyContext};
            EngineDataBus engine;
            EditorDataBus editor;
            CanvasLabels labels;
            Theme::Fonts fonts;
            ThemeContext theme{fonts};
            EditorSettingsSnapshot settings{.settings = DefaultEditorSettings()};
            EditorGuiContext context{engine, editor, labels, theme, settings};
            DecisionGraphCanvas canvas;

            CanvasFixture() {
                auto &io = ImGui::GetIO();
                io.DisplaySize = {1024, 768};
                io.DeltaTime = 1.0F / 60.0F;
                io.IniFilename = nullptr;
                io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
                auto *font = io.Fonts->AddFontDefault();
                REQUIRE(io.Fonts->Build());
                fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font, .icon = font};
                Theme::ApplyCurrentTheme();
            }

            DecisionCanvasInteraction Draw(const DecisionGraphWorkspaceDocument &document, float width,
                                           const std::shared_ptr<const AI::DecisionBlackboardProviderMetadata> &providers = {}) {
                ImGui::NewFrame();
                ImGui::SetNextWindowPos({0, 0});
                ImGui::SetNextWindowSize({width, 650});
                ImGui::Begin("Decision canvas test", nullptr, ImGuiWindowFlags_NoSavedSettings);
                canvas.Retain(std::span{&document, 1});
                const auto interaction = canvas.Draw(document, context, providers);
                CHECK_FALSE(interaction.edit.has_value());
                ImGui::End();
                ImGui::Render();
                const auto *draw = ImGui::GetDrawData();
                REQUIRE(draw);
                CHECK(draw->TotalVtxCount > 0);
                for (int list = 0; list < draw->CmdListsCount; ++list)
                    REQUIRE(std::ranges::all_of(draw->CmdLists[list]->VtxBuffer, [](const auto &vertex) {
                        return std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y);
                    }));
                return interaction;
            }

            const DecisionTaskChoice *DrawPalette(const DecisionTaskPaletteCatalog &catalog, DecisionTaskPaletteDraft &draft) {
                EditorWorkspaceViewCommandData command;
                ImGui::NewFrame();
                ImGui::SetNextWindowPos({0, 0});
                ImGui::SetNextWindowSize({220, 210});
                ImGui::Begin("Decision palette test", nullptr, ImGuiWindowFlags_NoSavedSettings);
                const auto *choice = DrawDecisionTaskPalette(catalog, draft, context, command);
                ImGui::End();
                ImGui::Render();
                REQUIRE(ImGui::GetDrawData()->TotalVtxCount > 0);
                return choice;
            }

            void DrawCreateDecisionModal(AssetBrowserInteractionState &state, const EditorWorkspaceViewModel &model,
                                         const AI::DecisionNodeTypeId selected) {
                state.initialTaskPalette.selected = selected;
                EditorWorkspaceViewCommandData command;
                ImGui::NewFrame();
                ImGui::SetNextWindowSize({600, 650});
                ImGui::Begin("Decision creation test", nullptr, ImGuiWindowFlags_NoSavedSettings);
                DrawAssetBrowserDialogs(state, model, command, context);
                ImGui::End();
                ImGui::Render();
            }
        };

        /** @brief Captures the native tree limits using one legal root and proportional Sequence fan-out. */
        std::shared_ptr<const AI::DecisionGraphSource> MaximumTree() {
            using Limits = AI::BehaviorTreeAssetHardLimits;
            constexpr std::size_t Hub = 2;
            constexpr std::size_t FirstGroup = 3;
            constexpr std::size_t Fanout = 4;
            constexpr std::size_t FirstLeaf = FirstGroup + Fanout;
            const auto graph = AI::DecisionGraphAssetId::Create(9).Value();
            const auto node = [](std::size_t value) {
                return AI::BehaviorTreeNodeId::Create(value).Value();
            };
            const auto pin = [](std::size_t value, std::size_t role) {
                return AI::BehaviorTreePinId::Create((value - 1) * 4 + role).Value();
            };
            AI::BehaviorTreeAssetDescriptor data{.graph = AI::BehaviorTreeGraphId::Create(graph.Value()).Value(), .root = node(1)};
            data.nodes.reserve(Limits::Nodes);
            data.edges.reserve(Limits::Nodes - 1);
            std::array<std::size_t, FirstLeaf> ordinals{};
            for (std::size_t identity = 1; identity <= Limits::Nodes; ++identity) {
                const bool root = identity == 1;
                const bool group = !root && identity < FirstLeaf;
                AI::BehaviorTreeNode entry{
                    .id = node(identity),
                    .kind = root    ? AI::BehaviorTreeNodeKind::Root
                            : group ? AI::BehaviorTreeNodeKind::Composite
                                    : AI::BehaviorTreeNodeKind::Task,
                    .minimumChildren = root || group ? 1U : 0U,
                    .maximumChildren = root    ? 1U
                                       : group ? Limits::ChildrenPerNode
                                               : 0U,
                    .pins = {{pin(identity, 1), AI::BehaviorTreePinDirection::Input},
                             {pin(identity, 2), AI::BehaviorTreePinDirection::Output, AI::BehaviorTreePinCardinality::Multiple},
                             {pin(identity, 3), AI::BehaviorTreePinDirection::Input},
                             {pin(identity, 4), AI::BehaviorTreePinDirection::Output}},
                    .control = {.kind = root    ? AI::DecisionControlKind::Root
                                        : group ? AI::DecisionControlKind::Sequence
                                                : AI::DecisionControlKind::Task},
                };
                if (!root && !group) {
                    entry.type = AI::BehaviorTreeNodeTypeId::Create(19).Value();
                    entry.task = AI::TaskId::Create(identity).Value();
                }
                data.nodes.push_back(std::move(entry));
                if (root)
                    continue;
                const auto parent = identity == Hub ? 1U : group ? Hub : FirstGroup + (identity - FirstLeaf) % Fanout;
                data.edges.push_back({AI::BehaviorTreeEdgeId::Create(identity - 1).Value(), AI::BehaviorTreeEdgeKind::Child, node(parent),
                                      pin(parent, 2), node(identity), pin(identity, 1), ordinals[parent]++});
            }
            auto source = AI::BehaviorTreeAsset::Capture(std::move(data));
            REQUIRE(source.HasValue());
            return std::make_shared<const AI::DecisionGraphSource>(AI::DecisionGraphSource{
                .asset = graph,
                .body = std::make_shared<const AI::BehaviorTreeAsset>(std::move(source).Value()),
            });
        }
    }  // namespace

    TEST_CASE("Decision task palette only offers an explicit visible selection from the current catalog",
              "[unit][editor][decision][palette]") {
        CanvasFixture fixture;
        const auto task = AI::DecisionNodeTypeId::Create(101).Value();
        const auto query = AI::DecisionNodeTypeId::Create(102).Value();
        const std::array choices{
            DecisionTaskChoice{task, AI::DecisionProviderId::Create(202).Value(), 3, "Native Movement (101)", false},
            DecisionTaskChoice{query, AI::DecisionProviderId::Create(203).Value(), 7, "Query Task (102)", true},
        };
        DecisionTaskPaletteDraft draft{.selected = query, .catalogRevision = 4};
        const auto draw = [&](std::span<const DecisionTaskChoice> catalog, std::uint64_t revision, std::string_view filter) {
            draft.search.fill('\0');
            std::ranges::copy(filter, draft.search.begin());
            return fixture.DrawPalette({catalog, revision}, draft);
        };

        CHECK(draw(choices, 4, "qUeRy") == &choices[1]);
        CHECK(fixture.labels.Used("workspace.source_edit.find"));
        CHECK(draw(choices, 4, "101") == nullptr);
        CHECK(draft.selected == query);
        CHECK(draw(choices, 4, "nonexistent") == nullptr);
        CHECK(fixture.labels.Used("workspace.source_edit.no_match"));
        CHECK(draw(choices, 4, "102") == &choices[1]);
        CHECK(draw(std::span{choices}.first(1), 4, "") == nullptr);
        CHECK(draw(choices, 5, "query") == nullptr);
        CHECK_FALSE(draft.selected.IsValid());
    }

    TEST_CASE("Decision task palette filters actual capability and provider declarations", "[unit][editor][decision][palette]") {
        using namespace AI;
        CanvasFixture fixture;
        const auto move = DecisionNodeTypeId::Create(101).Value();
        const auto sense = DecisionNodeTypeId::Create(102).Value();
        const auto provider = DecisionProviderId::Create(202).Value();
        const std::array choices{
            DecisionTaskChoice{move, provider, 1, "Move", false, AiCapabilitySet::Of(AiCapability::Navigation),
                               DecisionDescriptorSourceKind::Native},
            DecisionTaskChoice{sense, provider, 1, "Sense", false, AiCapabilitySet::Of(AiCapability::Perception),
                               DecisionDescriptorSourceKind::Package},
        };
        DecisionTaskPaletteDraft draft{.selected = sense, .catalogRevision = 4, .capability = AiCapability::Navigation};
        CHECK(fixture.DrawPalette({choices, 4}, draft) == nullptr);
        CHECK(draft.selected == sense);
        CHECK(fixture.labels.Used("workspace.decision_graph.task_filter_capability"));
        draft.capability = AiCapability::Perception;
        draft.origin = DecisionDescriptorSourceKind::Native;
        CHECK(fixture.DrawPalette({choices, 4}, draft) == nullptr);
        draft.origin = DecisionDescriptorSourceKind::Package;
        CHECK(fixture.DrawPalette({choices, 4}, draft) == &choices[1]);
        CHECK(fixture.labels.Used("workspace.decision_graph.task_filter_origin"));
        auto declared = choices;
        declared[1].role = DecisionNodeRole::Condition;
        declared[1].admission = DecisionPaletteAdmission::UnsupportedRole;
        draft.role = DecisionNodeRole::Condition;
        CHECK(fixture.DrawPalette({declared, 4}, draft) == nullptr);
        CHECK(fixture.labels.Used("workspace.decision_graph.task_filter_role"));
        CHECK(fixture.labels.Used("workspace.decision_graph.palette_role_unavailable"));
        CHECK(fixture.DrawPalette({std::span{choices}.first(1), 5}, draft) == nullptr);
        CHECK_FALSE(draft.capability);
        CHECK_FALSE(draft.origin);
        CHECK_FALSE(draft.role);
        CHECK_FALSE(draft.selected.IsValid());
    }

    TEST_CASE("New Decision graph modal uses the shared palette and rejects incompatible starter tasks",
              "[unit][editor][decision][palette]") {
        using namespace AI;
        CanvasFixture fixture;
        EditorWorkspaceViewModel model;
        model.availableBehaviorsRevision = 3;
        const auto schema = Assets::AssetId::FromBytes({7});
        model.decisionSchemaChoices.push_back({schema, "assets/guard.horo_bb"});
        const auto task = DecisionNodeTypeId::Create(101).Value();
        const auto query = DecisionNodeTypeId::Create(102).Value();
        const auto largeVersion = DecisionNodeTypeId::Create(103).Value();
        const auto provider = DecisionProviderId::Create(202).Value();
        model.decisionTaskPaletteChoices = {
            {task, provider, 3, "Starter task"},
            {query, provider, 7, "Query output", true},
            {largeVersion, provider, std::uint32_t{std::numeric_limits<std::uint16_t>::max()} + 1, "Large version"},
        };
        model.decisionPackageActivationTrustRequired = true;
        AssetBrowserInteractionSession browser;
        browser.OpenCreateTaskDecision("assets", DecisionPlanKind::BehaviorTree, model.availableBehaviorsRevision);
        auto &state = browser.State();
        state.createTaskDecision.name = "Guard";
        state.createTaskDecision.schema = schema;
        state.initialTaskPalette.catalogRevision = model.availableBehaviorsRevision;

        fixture.DrawCreateDecisionModal(state, model, task);
        CHECK(state.createTaskDecision.task == task);
        CHECK(state.createTaskDecision.provider == provider);
        CHECK(state.createTaskDecision.version == 3);
        CHECK(AssetBrowserInteractionSession::CanConfirmTaskDecision(state.createTaskDecision));
        CHECK(fixture.labels.Used("workspace.decision_graph.task_filter_verified_compatible"));
        CHECK(fixture.labels.Used("workspace.packages.execution_trust_required"));

        fixture.DrawCreateDecisionModal(state, model, query);
        CHECK_FALSE(AssetBrowserInteractionSession::CanConfirmTaskDecision(state.createTaskDecision));
        CHECK(fixture.labels.Used("workspace.decision_graph.palette_query_requires_wiring"));
        state.createTaskDecision.family = DecisionPlanKind::Utility;
        fixture.DrawCreateDecisionModal(state, model, query);
        CHECK_FALSE(state.createTaskDecision.task.IsValid());
        CHECK(fixture.labels.Used("workspace.decision_graph.palette_query_requires_tree"));
        state.createTaskDecision.family = DecisionPlanKind::BehaviorTree;
        fixture.DrawCreateDecisionModal(state, model, largeVersion);
        CHECK_FALSE(AssetBrowserInteractionSession::CanConfirmTaskDecision(state.createTaskDecision));
        CHECK(fixture.labels.Used("workspace.decision_canvas.provider_version_mismatch"));

        ++model.availableBehaviorsRevision;
        fixture.DrawCreateDecisionModal(state, model, task);
        CHECK_FALSE(state.initialTaskPalette.selected.IsValid());
        CHECK_FALSE(AssetBrowserInteractionSession::CanConfirmTaskDecision(state.createTaskDecision));
        CHECK(fixture.labels.Used("workspace.decision_graph.catalog_changed"));
    }

    TEST_CASE("New Utility modal binds only saved single scalar score keys", "[unit][editor][decision][palette]") {
        using namespace AI;
        CanvasFixture fixture;
        EditorWorkspaceViewModel model;
        model.availableBehaviorsRevision = 1;
        const auto asset = Assets::AssetId::FromBytes({7});
        const auto task = DecisionNodeTypeId::Create(101).Value();
        const auto provider = DecisionProviderId::Create(202).Value();
        const auto schemaId = BlackboardSchemaId::Create(21).Value();
        const auto boolean = BlackboardKeyId::Create(1).Value();
        const auto scalar = BlackboardKeyId::Create(2).Value();
        const std::array keys{BlackboardKeyDescriptor{.key = boolean},
                              BlackboardKeyDescriptor{.key = scalar, .kind = BlackboardValueKind::Scalar}};
        auto schema = BlackboardSchema::Capture({.identity = schemaId, .keys = keys});
        REQUIRE(schema.HasValue());
        const Sha256Digest savedDigest{{3}};
        model.decisionSchemaChoices.push_back({asset, "assets/utility.horo_bb"});
        model.decisionTaskPaletteChoices.push_back({task, provider, 1, "Starter"});
        model.blackboardDocuments.push_back(
            {.snapshot = {
                 .source = std::make_shared<BlackboardSchemaSource>(
                     BlackboardSchemaSource{.asset = asset, .schema = schemaId, .rows = {{keys[0], "Condition"}, {keys[1], "Score"}}}),
                 .schema = std::make_shared<const BlackboardSchema>(std::move(schema).Value()),
                 .sourceDigest = savedDigest,
             }});

        AssetBrowserInteractionSession browser;
        browser.OpenCreateTaskDecision("assets", DecisionPlanKind::Utility, model.availableBehaviorsRevision);
        auto &state = browser.State();
        auto &draft = state.createTaskDecision;
        draft.name = "Utility";
        draft.schema = asset;
        state.initialTaskPalette.catalogRevision = model.availableBehaviorsRevision;
        REQUIRE(draft.score);
        draft.score->key = scalar;
        fixture.DrawCreateDecisionModal(state, model, task);
        CHECK(draft.score->key == scalar);
        CHECK(draft.score->schemaSource == savedDigest);
        CHECK(AssetBrowserInteractionSession::CanConfirmTaskDecision(draft));

        draft.score->key = boolean;
        fixture.DrawCreateDecisionModal(state, model, task);
        CHECK_FALSE(draft.score->key.IsValid());
        CHECK_FALSE(AssetBrowserInteractionSession::CanConfirmTaskDecision(draft));

        model.blackboardDocuments.front().snapshot.dirty = true;
        draft.score->key = scalar;
        fixture.DrawCreateDecisionModal(state, model, task);
        CHECK_FALSE(draft.score->key.IsValid());
        CHECK(draft.score->schemaSource == Sha256Digest{});
    }

    TEST_CASE("Decision task palette explains exact saved schema incompatibilities without creating a provider",
              "[unit][editor][decision][palette]") {
        using namespace AI;
        using namespace AI::DecisionGraphTestSupport;
        CanvasFixture fixture;
        Fixture provider;
        auto registration = Registration(provider.probe);
        const auto key = MakeIdentity<BlackboardKeyId>(1);
        registration.contract.descriptor.requirements = {
            {.key = key, .kind = BlackboardValueKind::Boolean, .access = BlackboardKeyAccess::ReadWrite, .source = Location()},
        };
        const auto generation = DecisionNodeGeneration::Capture({&registration, 1}, provider.admission);
        REQUIRE(generation.HasValue());
        const auto metadata = DecisionBlackboardProviderMetadata::Capture(*generation.Value());
        REQUIRE(metadata.HasValue());
        const auto &descriptor = registration.contract.descriptor;
        const std::array choices{DecisionTaskChoice{descriptor.type, descriptor.origin.provider, descriptor.origin.version, "Task", true}};
        DecisionTaskPaletteDraft draft{.selected = descriptor.type, .catalogRevision = 1};
        DecisionTaskPaletteCatalog catalog{choices, 1, metadata.Value().get()};
        CHECK(fixture.DrawPalette(catalog, draft) == &choices[0]);
        CHECK(fixture.labels.Used("workspace.decision_graph.task_schema_unverified"));
        draft.verifiedCompatibleOnly = true;
        CHECK(fixture.DrawPalette(catalog, draft) == nullptr);
        CHECK(fixture.labels.Used("workspace.decision_graph.task_filter_verified_compatible"));
        CHECK(draft.selected == descriptor.type);

        const std::array readOnly{
            BlackboardKeyDescriptor{.key = key, .kind = BlackboardValueKind::Boolean, .access = BlackboardKeyAccess::ReadOnly}};
        const auto incompatible = BlackboardSchema::Capture({SchemaId(), 1, BlackboardUnknownValuePolicy::Reject, readOnly});
        REQUIRE(incompatible.HasValue());
        catalog.schema = &incompatible.Value();
        CHECK(fixture.DrawPalette(catalog, draft) == nullptr);
        CHECK(fixture.labels.Used("workspace.source_edit.no_match"));
        draft.verifiedCompatibleOnly = false;
        CHECK(fixture.DrawPalette(catalog, draft) == nullptr);
        CHECK(fixture.labels.Used("workspace.decision_graph.task_key_access"));

        const std::array writable{
            BlackboardKeyDescriptor{.key = key, .kind = BlackboardValueKind::Boolean, .access = BlackboardKeyAccess::ReadWrite}};
        const auto compatible = BlackboardSchema::Capture({SchemaId(), 2, BlackboardUnknownValuePolicy::Reject, writable});
        REQUIRE(compatible.HasValue());
        catalog.schema = &compatible.Value();
        CHECK(fixture.DrawPalette(catalog, draft) == &choices[0]);
        draft.verifiedCompatibleOnly = true;
        CHECK(fixture.DrawPalette(catalog, draft) == &choices[0]);
        catalog.allowQueryOutput = false;
        CHECK(fixture.DrawPalette(catalog, draft) == nullptr);
        catalog.allowQueryOutput = true;
        catalog.providers = nullptr;
        CHECK(fixture.DrawPalette(catalog, draft) == nullptr);
        CHECK(draft.selected == descriptor.type);
        catalog.providers = metadata.Value().get();
        CHECK(fixture.DrawPalette(catalog, draft) == &choices[0]);

        auto stale = choices;
        ++stale[0].version;
        catalog.choices = stale;
        CHECK(fixture.DrawPalette(catalog, draft) == nullptr);
        draft.verifiedCompatibleOnly = false;
        CHECK(fixture.DrawPalette(catalog, draft) == nullptr);
        CHECK(fixture.labels.Used("workspace.decision_canvas.provider_version_mismatch"));
        CHECK(provider.probe.factories == 0);
    }

    TEST_CASE("Decision package trust feedback is shown only for the current admitted denial evidence",
              "[unit][editor][decision][palette]") {
        CanvasFixture fixture;
        constexpr auto key = "workspace.packages.execution_trust_required";
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({195, 240});
        ImGui::Begin("Decision package trust test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        DrawDecisionPackageTrustRequired(false, fixture.context);
        CHECK_FALSE(fixture.labels.Used(key));
        DrawDecisionPackageTrustRequired(true, fixture.context);
        CHECK(fixture.labels.Used(key));
        ImGui::End();
        ImGui::Render();
        REQUIRE(ImGui::GetDrawData()->TotalVtxCount > 0);
    }

    TEST_CASE("Decision provider property inspector renders authored typed values and preserves opaque data",
              "[unit][editor][decision][palette]") {
        using namespace AI;
        using AI::DecisionGraphTestSupport::MakeIdentity;
        CanvasFixture fixture;
        const std::array properties{
            BehaviorTreeProperty{MakeIdentity<BehaviorTreePropertyId>(1), "Enabled", BehaviorTreePropertyKind::Boolean, true},
            BehaviorTreeProperty{MakeIdentity<BehaviorTreePropertyId>(2), "Integer", BehaviorTreePropertyKind::SignedInteger,
                                 std::int64_t{-4'000'000'000}},
            BehaviorTreeProperty{MakeIdentity<BehaviorTreePropertyId>(3), "Scalar", BehaviorTreePropertyKind::Scalar, 0.125},
            BehaviorTreeProperty{MakeIdentity<BehaviorTreePropertyId>(4), "Long source label without a format",
                                 BehaviorTreePropertyKind::String, std::string{"Literal %d and ## characters"}},
            BehaviorTreeProperty{MakeIdentity<BehaviorTreePropertyId>(5), "Node", BehaviorTreePropertyKind::NodeReference,
                                 MakeIdentity<BehaviorTreeNodeId>(55)},
            BehaviorTreeProperty{MakeIdentity<BehaviorTreePropertyId>(6), "Preserved", BehaviorTreePropertyKind::Opaque,
                                 BehaviorTreeOpaqueProperty{7, {0, 0xFF, 0xA0}}},
            BehaviorTreeProperty{MakeIdentity<BehaviorTreePropertyId>(7), "Query", BehaviorTreePropertyKind::QueryOutput,
                                 BehaviorTreeQueryOutput{MakeIdentity<QueryId>(71), MakeIdentity<QueryResultSchemaId>(72), 2,
                                                         MakeIdentity<QueryItemTypeId>(73), MakeIdentity<BlackboardKeyId>(74), 24}},
        };
        const auto original = properties;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({195, 580});
        ImGui::Begin("Decision task inspector test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        DrawDecisionTaskProperties(properties, fixture.context);
        ImGui::End();
        ImGui::Render();

        REQUIRE(ImGui::GetDrawData()->TotalVtxCount > 0);
        CHECK(fixture.labels.Used("workspace.decision_graph.task_properties"));
        CHECK(fixture.labels.Used("workspace.blackboard.value_true"));
        CHECK(fixture.labels.Used("workspace.decision_graph.task_property_opaque"));
        CHECK(fixture.labels.Used("workspace.decision_graph.task_property_version"));
        CHECK(fixture.labels.Used("workspace.decision_graph.task_property_result"));
        CHECK(fixture.labels.Used("workspace.decision_graph.task_property_item"));
        CHECK(fixture.labels.Used("workspace.decision_graph.query_output_key"));
        CHECK(properties == original);
    }

    TEST_CASE("Decision canvas validation resolves only exact visible nodes and clears replaced findings",
              "[unit][editor][decision][canvas]") {
        const auto graph = AI::DecisionGraphAssetId::Create(10).Value();
        const auto other = AI::DecisionGraphAssetId::Create(11).Value();
        const auto node = AI::DecisionNodeId::Create(1).Value();
        const auto sibling = AI::DecisionNodeId::Create(2).Value();
        GraphViewSnapshot view{.nodes = {{.id = node}, {.id = sibling}}};
        const std::array findings{
            AI::DecisionAssetValidationDiagnostic{.severity = DiagnosticSeverity::Warning, .target = {graph, node}},
            AI::DecisionAssetValidationDiagnostic{.severity = DiagnosticSeverity::Error, .target = {graph, node}},
            AI::DecisionAssetValidationDiagnostic{.severity = DiagnosticSeverity::Fatal, .target = {other, node}},
            AI::DecisionAssetValidationDiagnostic{.severity = DiagnosticSeverity::Fatal, .target = {other, sibling}},
        };
        ApplyDecisionCanvasValidation(view, graph, findings);
        CHECK(view.nodes[0].validationSeverity == DiagnosticSeverity::Error);
        CHECK_FALSE(view.nodes[1].validationSeverity);
        ApplyDecisionCanvasValidation(view, graph, {});
        CHECK_FALSE(view.nodes[0].validationSeverity);
    }

    TEST_CASE("Decision canvas renders a validation badge on the current graph revision", "[unit][editor][decision][canvas]") {
        using namespace AI::DecisionGraphTestSupport;
        Fixture provider;
        auto task = TreeNode(2, AI::DecisionControlKind::Task);
        task.typeVersion = {2, 0};
        auto source =
            std::make_shared<const AI::DecisionGraphSource>(Tree(4, {TreeNode(1, AI::DecisionControlKind::Root), task}, {Edge(1, 1, 2)}));
        auto compiled = AI::DecisionGraphCompiler::Compile(source->asset, std::span{source.get(), 1}, provider.Environment());
        REQUIRE(compiled.HasValue());
        REQUIRE_FALSE(compiled.Value().validation.Empty());

        DecisionGraphWorkspaceDocument document;
        document.identity.instance = DocumentInstanceId::Create(19).Value();
        document.graph = std::move(source);
        document.family = AI::DecisionPlanKind::BehaviorTree;
        document.revision = 1;
        document.validation = std::make_shared<const AI::DecisionAssetValidationReport>(std::move(compiled.Value().validation));
        CanvasFixture fixture;
        fixture.Draw(document, 280.0F);
        CHECK(fixture.labels.Used("workspace.decision_canvas.validation_issue"));

        fixture.labels.Clear();
        document.validation.reset();
        fixture.Draw(document, 280.0F);
        CHECK_FALSE(fixture.labels.Used("workspace.decision_canvas.validation_issue"));
    }

    TEST_CASE("Decision Problems pages findings and routes the exact activated target", "[unit][editor][decision][canvas]") {
        using namespace AI::DecisionGraphTestSupport;
        CanvasFixture fixture;
        const auto source = std::make_shared<const AI::DecisionGraphSource>(
            Tree(4, {TreeNode(1, AI::DecisionControlKind::Root), TreeNode(2, AI::DecisionControlKind::Task)}, {Edge(1, 1, 2)}));
        DecisionGraphWorkspaceDocument document;
        document.identity.instance = DocumentInstanceId::Create(21).Value();
        document.graph = source;
        document.revision = 3;
        std::array<AI::DecisionAssetValidationDiagnostic, 8> findings{};
        const auto firstKey = AI::BlackboardKeyId::Create(4).Value();
        const auto secondKey = AI::BlackboardKeyId::Create(6).Value();
        findings[0].target = {Graph(5), Node(1), source->blackboard, firstKey};
        findings[1].target = {source->asset, Node(1), source->blackboard, firstKey};
        findings[2].target = {source->asset, Node(2), source->blackboard, secondKey};
        findings[6].severity = DiagnosticSeverity::Fatal;
        findings[7].target = {source->asset, Node(99), {}, {}};

        const auto draw = [&] {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({480, 680});
            ImGui::Begin("Decision Problems test", nullptr, ImGuiWindowFlags_NoSavedSettings);
            EditorWorkspaceViewCommandData command;
            const auto node = DrawDecisionProblems(document, findings, fixture.context, command);
            ImGui::End();
            ImGui::Render();
            return std::pair{node, std::move(command)};
        };
        auto initial = draw();
        CHECK(initial.second.command == EditorWorkspaceViewCommand::None);
        CHECK_FALSE(fixture.labels.Used("workspace.global_dock.build_output.row_status.fatal"));
        ImGuiWindow *card{};
        for (auto *window : ImGui::GetCurrentContext()->Windows) {
            if (window->Active && std::string_view{window->Name}.find("/##DecisionProblems_") != std::string_view::npos)
                card = window;
        }
        REQUIRE(card);
        ImGui::FocusWindow(card);
        const auto activate = [&](const char *key, int index) {
            const auto &label = fixture.labels.Get("editor", key);
            const auto seed = ImHashData(&index, sizeof(index), card->ID);
            ImGui::ActivateItemByID(ImHashStr(label.c_str(), 0, seed));
        };
        activate("workspace.decision_graph.open_diagnostic_key", 0);
        CHECK(draw().second.command == EditorWorkspaceViewCommand::None);
        activate("workspace.decision_graph.open_diagnostic_key", 2);
        const auto selected = draw().second.decisionDiagnosticKey;
        REQUIRE(selected);
        CHECK(selected->key == secondKey);
        CHECK(selected->fence.instance == document.identity.instance);
        CHECK(selected->fence.revision == document.revision);
        activate("workspace.decision_graph.locate_problem_node", 1);
        CHECK(draw().first == Node(1));

        const auto &next = fixture.labels.Get("editor", "workspace.blackboard.usage_next");
        ImGui::ActivateItemByID(card->GetID(next.c_str()));
        static_cast<void>(draw());
        static_cast<void>(draw());
        CHECK(fixture.labels.Used("workspace.global_dock.build_output.row_status.fatal"));
        CHECK(fixture.labels.Used("workspace.decision_graph.problem_node_missing"));
    }

    TEST_CASE("Decision graph canvas renders native links across revisions and narrow widths", "[unit][editor][decision][canvas]") {
        CanvasFixture fixture;
        const auto first = AI::DecisionNodeId::Create(1).Value();
        const auto last = AI::DecisionNodeId::Create(2).Value();
        const auto edge = AI::DecisionNodeId::Create(3).Value();
        auto source = std::make_shared<AI::DecisionGraphSource>();
        source->body = AI::StateMachineAssetDescriptor{
            .initial = first,
            .states = {{.id = first}, {.id = last, .terminal = AI::DecisionStateTerminal::Succeeded}},
            .transitions = {{.id = edge, .source = first, .target = last}},
        };
        DecisionGraphWorkspaceDocument document;
        document.identity.instance = DocumentInstanceId::Create(5).Value();
        document.graph = source;
        document.revision = 1;
        document.editable = true;
        fixture.Draw(document, 920.0F);
        fixture.Draw(document, 280.0F);
        auto replacement = std::make_shared<AI::DecisionGraphSource>(*source);
        std::get<AI::StateMachineAssetDescriptor>(replacement->body).states.back().terminal = AI::DecisionStateTerminal::Failed;
        document.graph = std::move(replacement);
        ++document.revision;
        fixture.Draw(document, 280.0F);
        fixture.canvas.Retain({});
    }

    TEST_CASE("Decision canvas locates bounded tree endpoints at the authored node and pin ceilings", "[unit][editor][decision][canvas]") {
        using Limits = AI::BehaviorTreeAssetHardLimits;
        CanvasFixture fixture;
        const auto graph = MaximumTree();
        const auto instance = DocumentInstanceId::Create(41).Value();
        const DecisionGraphDocumentFence fence{instance, 2};
        const auto projected = ProjectDecisionCanvas(*graph, fence, true);
        REQUIRE(projected.nodes.size() == Limits::Nodes);
        REQUIRE(projected.pins.size() == Limits::Pins);
        REQUIRE(projected.links.size() == Limits::Nodes - 1);
        CHECK(projected.links.size() <= Limits::Edges);
        for (const auto &link : projected.links) {
            REQUIRE(link.from < projected.pins.size());
            REQUIRE(link.to < projected.pins.size());
            CHECK(projected.pins[link.from].output);
            CHECK_FALSE(projected.pins[link.to].output);
            CHECK(projected.pins[link.from].node != projected.pins[link.to].node);
        }
        const auto automatic = AutomaticDecisionLayout(projected);
        REQUIRE(automatic.size() == MaximumDecisionLayoutNodes);
        CHECK(automatic == AutomaticDecisionLayout(projected));
        for (std::size_t index = 0; index < automatic.size(); ++index) {
            CHECK(automatic[index].node == projected.nodes[index].id);
            CHECK(std::abs(automatic[index].x) <= MaximumDecisionLayoutCoordinate);
            CHECK(std::abs(automatic[index].y) <= MaximumDecisionLayoutCoordinate);
        }
        auto positions = automatic;
        const auto first = projected.nodes.front().id;
        const auto last = projected.nodes.back().id;
        constexpr float Distant = MaximumDecisionLayoutCoordinate * 0.75F;
        positions.front().x = -Distant;
        positions.back().x = Distant;
        DecisionGraphWorkspaceDocument document;
        document.identity.instance = instance;
        document.graph = graph;
        document.family = AI::DecisionPlanKind::BehaviorTree;
        document.revision = fence.revision;
        document.editable = true;
        document.layoutWritable = true;
        document.layout = std::make_shared<const DecisionGraphLayout>(std::move(positions));
        fixture.Draw(document, 920.0F);

        fixture.canvas.RequestFocus({instance, fence.revision - 1}, last);
        const auto stale = fixture.Draw(document, 280.0F);
        CHECK_FALSE(stale.revealed);
        CHECK_FALSE(stale.node);
        fixture.canvas.RequestFocus(fence, AI::DecisionNodeId::Create(Limits::Nodes + 1).Value());
        const auto unknown = fixture.Draw(document, 280.0F);
        CHECK_FALSE(unknown.revealed);
        CHECK_FALSE(unknown.node);

        const auto reveal = [&](AI::DecisionNodeId node, float width) {
            fixture.canvas.RequestFocus(fence, node);
            auto interaction = fixture.Draw(document, width);
            CHECK(interaction.node == node);
            for (int frame = 0; frame < 4 && !interaction.revealed; ++frame)
                interaction = fixture.Draw(document, width);
            CHECK(interaction.revealed == node);
            CHECK_FALSE(interaction.layout);
        };
        reveal(last, 280.0F);
        reveal(first, 640.0F);
        CHECK(document.revision == fence.revision);
        CHECK(document.graph == graph);
    }

    TEST_CASE("State-machine transition inspection reflects the cooked source-local order and real leaf selection",
              "[unit][editor][decision][canvas]") {
        using namespace AI::DecisionGraphTestSupport;
        Fixture fixture;
        auto source = TerminalMachine(1);
        const auto root = Node(1), leaf = Node(2), target = Node(3);
        auto &machine = std::get<AI::StateMachineAssetDescriptor>(source.body);
        machine = {
            .initial = root,
            .states = {{.id = root, .initialChild = leaf},
                       {.id = leaf, .parent = root},
                       {.id = target, .parent = root, .terminal = AI::DecisionStateTerminal::Succeeded}},
            .transitions = {{.id = Node(11), .source = leaf, .target = target, .priority = -2},
                            {.id = Node(4), .source = leaf, .target = target, .priority = -8},
                            {.id = Node(9), .source = root, .target = target, .priority = 99},
                            {.id = Node(10), .source = leaf, .target = target, .priority = -2}},
        };
        const auto compiled = fixture.Compile(std::span{&source, 1}, 1);
        REQUIRE(compiled->Graphs().size() == 1);
        const auto &cooked = std::get<std::shared_ptr<const AI::CookedStateMachinePlan>>(compiled->Graphs().front().execution);
        REQUIRE(cooked->Transitions().size() == machine.transitions.size());
        CHECK(cooked->Transitions().front().id == Node(9));
        std::size_t leafRank{};
        for (const auto &runtime : cooked->Transitions()) {
            const auto authored = std::ranges::find(machine.transitions, runtime.id, &AI::StateMachineTransition::id);
            REQUIRE(authored != machine.transitions.end());
            const auto order = ProjectStateMachineTransitionOrder(machine, *authored);
            CHECK(order.priority == runtime.priority);
            CHECK(order.rank == (runtime.source == leaf ? ++leafRank : 1));
        }
        CHECK(leafRank == 3);

        auto instance = fixture.Instance(compiled);
        const auto snapshot = fixture.blackboard->Snapshot();
        REQUIRE(snapshot.HasValue());
        AI::DecisionStepTrace trace;
        REQUIRE(instance->StepWithTrace({1, fixture.binding, snapshot.Value(), {}}, trace).HasValue());
        const auto committed =
            std::ranges::find(trace.Records(), AI::DecisionStepRecordKind::StateMachineTransition, &AI::DecisionStepRecord::kind);
        REQUIRE(committed != trace.Records().end());
        CHECK(committed->node == Node(10));
        CHECK(fixture.probe.factories == 0);
    }

    TEST_CASE("Decision canvas shows an unknown placeholder and refreshes provider warnings without changing the source",
              "[unit][editor][decision][canvas]") {
        using namespace AI::DecisionGraphTestSupport;
        CanvasFixture fixture;
        Fixture provider;
        auto task = TreeNode(3, AI::DecisionControlKind::Task);
        task.typeVersion = {2, 0};
        auto unknown = TreeNode(4, AI::DecisionControlKind::Task);
        unknown.kind = AI::BehaviorTreeNodeKind::Unknown;
        unknown.control.kind = AI::DecisionControlKind::Unspecified;
        unknown.task = {};
        const auto source = std::make_shared<const AI::DecisionGraphSource>(
            Tree(4, {TreeNode(1, AI::DecisionControlKind::Root), TreeNode(2, AI::DecisionControlKind::Parallel), task, unknown},
                 {Edge(1, 1, 2), Edge(2, 2, 3), Edge(3, 2, 4, 1)}));
        DecisionGraphWorkspaceDocument document;
        document.identity.instance = DocumentInstanceId::Create(18).Value();
        document.graph = source;
        document.family = AI::DecisionPlanKind::BehaviorTree;
        document.revision = 1;

        fixture.Draw(document, 280.0F);
        CHECK(fixture.labels.Used("workspace.decision_canvas.unknown"));
        CHECK(fixture.labels.Used("workspace.decision_canvas.provider_catalog_unavailable"));
        fixture.labels.Clear();

        const auto metadata = AI::DecisionBlackboardProviderMetadata::Capture(*provider.providers);
        REQUIRE(metadata.HasValue());
        fixture.Draw(document, 280.0F, metadata.Value());
        CHECK(fixture.labels.Used("workspace.decision_canvas.unknown"));
        CHECK(fixture.labels.Used("workspace.decision_canvas.provider_version_mismatch"));
        CHECK_FALSE(fixture.labels.Used("workspace.decision_canvas.provider_catalog_unavailable"));
        CHECK(provider.probe.factories == 0);
        CHECK(document.graph == source);
        CHECK(document.revision == 1);
    }

    TEST_CASE("Utility canvas labels options in the real selector evaluation order", "[unit][editor][decision][canvas]") {
        CanvasFixture fixture;
        const auto node = [](std::uint64_t id) {
            return AI::DecisionNodeId::Create(id).Value();
        };
        const auto key = AI::BlackboardKeyId::Create(1).Value();
        const std::array keys{AI::BlackboardKeyDescriptor{.key = key, .kind = AI::BlackboardValueKind::Scalar}};
        auto schema = AI::BlackboardSchema::Capture({.identity = AI::BlackboardSchemaId::Create(1).Value(), .keys = keys});
        REQUIRE(schema.HasValue());
        const auto option = [&](std::uint64_t id, double priority) {
            return AI::UtilityOption{node(id), AI::TaskId::Create(id).Value(), priority, 0, {{.key = key}}};
        };
        const std::array authored{option(90, 1.0), option(7, 0.2), option(42, 0.7)};
        const auto selector =
            AI::UtilitySelector::Capture(std::make_shared<const AI::BlackboardSchema>(std::move(schema).Value()), authored);
        REQUIRE(selector.HasValue());
        AI::UtilityDecisionSource utility{.root = node(12)};
        for (const auto &entry : authored)
            utility.options.push_back({.option = entry});

        auto source = std::make_shared<const AI::DecisionGraphSource>(AI::DecisionGraphSource{.body = std::move(utility)});
        const DecisionGraphDocumentFence fence{DocumentInstanceId::Create(8).Value(), 1};
        const auto graph = ProjectDecisionCanvas(*source, fence, true);
        REQUIRE(graph.nodes.size() == authored.size() + 1);
        REQUIRE(graph.links.size() == authored.size());
        CHECK_FALSE(graph.nodes.front().incomingOrder.has_value());
        for (std::size_t index = 0; index < authored.size(); ++index) {
            const auto &expected = selector.Value().Options()[index];
            CHECK(graph.nodes[index + 1].id == expected.id);
            CHECK(graph.nodes[index + 1].incomingOrder == index);
            CHECK(graph.links[index].authoredId == expected.id.Value());
            CHECK(graph.pins[graph.links[index].to].node == expected.id);
        }
        CHECK(std::get<AI::UtilityDecisionSource>(source->body).options.front().option.id == authored.front().id);

        DecisionGraphWorkspaceDocument document;
        document.identity.instance = fence.instance;
        document.graph = std::move(source);
        document.family = AI::DecisionPlanKind::Utility;
        document.revision = fence.revision;
        fixture.Draw(document, 280.0F);
        CHECK(fixture.labels.Used("workspace.decision_canvas.utility_order"));
        CHECK_FALSE(fixture.labels.Used("workspace.decision_graph.edit_tree_order"));
    }

    TEST_CASE("Decision projection retains maximum machine transitions and utility option links", "[unit][editor][decision][canvas]") {
        const auto node = [](std::size_t id) {
            return AI::DecisionNodeId::Create(id).Value();
        };
        const DecisionGraphDocumentFence fence{DocumentInstanceId::Create(7).Value(), 1};
        AI::StateMachineAssetDescriptor machine{.initial = node(1)};
        for (std::size_t index = 0; index < AI::StateMachineLimits::States; ++index) {
            const auto origin = node(index + 1);
            machine.states.push_back({.id = origin});
            machine.transitions.push_back({.id = node(AI::StateMachineLimits::States + index * 2 + 1),
                                           .source = origin,
                                           .target = node((index + 1) % AI::StateMachineLimits::States + 1)});
            machine.transitions.push_back({.id = node(AI::StateMachineLimits::States + index * 2 + 2), .source = origin, .target = origin});
        }
        const auto states = ProjectDecisionCanvas(AI::DecisionGraphSource{.body = machine}, fence, true);
        REQUIRE(states.nodes.size() == AI::StateMachineLimits::States);
        REQUIRE(states.links.size() == AI::StateMachineLimits::Transitions);
        REQUIRE(states.pins.size() == AI::StateMachineLimits::States * 2);
        for (std::size_t index = 0; index < machine.transitions.size(); ++index) {
            const auto &link = states.links[index];
            const auto &transition = machine.transitions[index];
            CHECK(link.authoredId == transition.id.Value());
            CHECK(states.pins[link.from].node == transition.source);
            CHECK(states.pins[link.to].node == transition.target);
        }
        CHECK(AutomaticDecisionLayout(states).size() == AI::StateMachineLimits::States);

        AI::UtilityDecisionSource utility{.root = node(1)};
        for (std::size_t index = 0; index < AI::UtilitySelector::MaximumOptions; ++index)
            utility.options.push_back({.option = {.id = node(index + 2)}});
        const auto options = ProjectDecisionCanvas(AI::DecisionGraphSource{.body = utility}, fence, true);
        REQUIRE(options.nodes.size() == AI::UtilitySelector::MaximumOptions + 1);
        REQUIRE(options.links.size() == AI::UtilitySelector::MaximumOptions);
        for (std::size_t index = 0; index < utility.options.size(); ++index) {
            const auto &link = options.links[index];
            CHECK(link.role == GraphLinkRole::UtilityOption);
            CHECK(options.pins[link.from].node == utility.root);
            CHECK(options.pins[link.to].node == utility.options[index].option.id);
        }
        CHECK(AutomaticDecisionLayout(options).size() == AI::UtilitySelector::MaximumOptions + 1);
    }
}  // namespace Horo::Editor
