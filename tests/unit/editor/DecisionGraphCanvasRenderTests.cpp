#include "Horo/AI/DecisionGraphCompiler.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/screens/workspace/DecisionGraphCanvas.h"
#include "editor/screens/workspace/DecisionGraphCanvasModel.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <imgui.h>
#include <map>
#include <memory>
#include <span>

namespace Horo::Editor {
    namespace {
        /** @brief Provides long labels without any native localization/filesystem setup. */
        class CanvasLabels final : public ILocalizationService {
        public:
            const std::string &Get(std::string_view, std::string_view key) const override {
                return labels_.try_emplace(std::string{key}, "Localized graph label " + std::string{key}).first->second;
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

            DecisionCanvasInteraction Draw(const DecisionGraphWorkspaceDocument &document, float width) {
                ImGui::NewFrame();
                ImGui::SetNextWindowPos({0, 0});
                ImGui::SetNextWindowSize({width, 650});
                ImGui::Begin("Decision canvas test", nullptr, ImGuiWindowFlags_NoSavedSettings);
                canvas.Retain(std::span{&document, 1});
                const auto interaction = canvas.Draw(document, context);
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
