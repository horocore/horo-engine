#include "DecisionGraphCanvasModel.h"
#include "EditorWorkspaceView.h"
#include "Horo/Application/DecisionAssetCookHost.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <type_traits>
#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief Projects bounded family information without instantiating or preparing the graph. */
        struct DecisionSummary final {
            const char *familyLabel;
            std::size_t nodes;
            std::size_t unknown{};
        };

        /** @brief Counts the elements represented by one authored graph family. */
        DecisionSummary Summarize(const AI::DecisionGraphSource &source) {
            return std::visit([](const auto &body) -> DecisionSummary {
                using Type = std::remove_cvref_t<decltype(body)>;
                if constexpr (std::is_same_v<Type, std::shared_ptr<const AI::BehaviorTreeAsset>>)
                    return {"workspace.decision_graph.behavior_tree", body->Nodes().size(), body->UnknownNodeCount()};
                else if constexpr (std::is_same_v<Type, AI::StateMachineAssetDescriptor>)
                    return {"workspace.decision_graph.state_machine", body.states.size() + body.transitions.size()};
                else
                    return {"workspace.decision_graph.utility", body.options.size() + 1};
            }, source.body);
        }
    }  // namespace

    /** @copydoc EditorWorkspaceView::DrawBehaviorTreeQueryOutput */
    void EditorWorkspaceView::DrawBehaviorTreeQueryOutput(const EditorWorkspaceViewModel &viewModel,
                                                          const DecisionGraphWorkspaceDocument &document, const AI::BehaviorTreeNode &node,
                                                          EditorWorkspaceViewCommandData &outCommand,
                                                          std::optional<DecisionTaskSelection> task) const {
        const auto property =
            std::ranges::find(node.properties, AI::BehaviorTreePropertyKind::QueryOutput, &AI::BehaviorTreeProperty::kind);
        if (!task && property == node.properties.end())
            return;
        const auto *bound = task ? nullptr : std::get_if<AI::BehaviorTreeQueryOutput>(&property->value);
        const auto id = AI::DecisionNodeId::Create(node.id.Value()).Value();
        auto &selection = m_behaviorTreeQuerySelection;
        if (selection.instance != document.identity.instance || selection.node != id || selection.revision != document.revision ||
            selection.task != task) {
            selection = {.instance = document.identity.instance,
                         .node = id,
                         .revision = document.revision,
                         .task = task,
                         .key = bound ? bound->key : AI::BlackboardKeyId{},
                         .timeout = bound ? std::to_string(bound->timeoutTicks) : std::string{}};
        }

        const auto &fonts = m_context.theme.fonts;
        const auto text = [&](std::string_view key) -> const std::string & {
            return m_context.localization.Get("editor", key);
        };
        const auto button = [&](std::string_view key, bool enabled) {
            return Ui::Button({.label = text(key).c_str(), .enabled = enabled, .font = fonts.sans});
        };
        const auto emit = [&](BehaviorTreeEditOperation operation, std::uint32_t timeout = 0) {
            outCommand = {.command = EditorWorkspaceViewCommand::EditDecisionBehaviorTree,
                          .behaviorTreeEdit = BehaviorTreeEditRequest{.instance = document.identity.instance,
                                                                      .revision = document.revision,
                                                                      .operation = operation,
                                                                      .node = id,
                                                                      .task = task,
                                                                      .queryOutput = BehaviorTreeQueryOutputEdit{selection.source,
                                                                                                                 selection.key, timeout}}};
        };
        if (bound)
            Ui::WrappedText(ImGui::GetContentRegionAvail().x,
                            std::format("{} {}", text("workspace.decision_graph.bound_query"), bound->query.Value()), Theme::Text(), fonts);
        const auto &queries = viewModel.behaviorTreeQueryChoices;
        const auto query = std::ranges::find(queries, selection.source, &DecisionSchemaChoice::asset);
        int queryIndex = query == queries.end() ? 0 : static_cast<int>(query - queries.begin()) + 1;
        const Ui::ComboItemSource queryItems{.label = [&](int slot) {
            return slot == 0 ? text("workspace.decision_graph.select_query").c_str()
                             : queries[static_cast<std::size_t>(slot - 1)].path.c_str();
        }};
        static_cast<void>(Ui::ComboControl("##BehaviorTreeQuery", &queryIndex, static_cast<int>(queries.size()) + 1, queryItems, fonts));
        selection.source = queryIndex == 0 ? Assets::AssetId{} : queries[static_cast<std::size_t>(queryIndex - 1)].asset;
        if (button("workspace.decision_graph.review_query", selection.source.IsValid()))
            emit(BehaviorTreeEditOperation::ReviewQueryOutput);

        const auto &review = document.queryReview;
        if (!review || review->asset != selection.source || review->node != id || review->documentRevision != document.revision ||
            review->task != task)
            return;
        const auto timeout = DrawBehaviorTreeQueryFields(*review);
        const auto operation = task ? BehaviorTreeEditOperation::AppendTask : BehaviorTreeEditOperation::SetQueryOutput;
        const auto action = task ? "workspace.decision_graph.edit_tree_append_task" : "workspace.decision_graph.apply_query_output";
        if (button(action, timeout.has_value()) && timeout)
            emit(operation, *timeout);
    }

    /** @copydoc EditorWorkspaceView::DrawBehaviorTreeQueryFields */
    std::optional<std::uint32_t> EditorWorkspaceView::DrawBehaviorTreeQueryFields(const BehaviorTreeQueryReview &review) const {
        auto &selection = m_behaviorTreeQuerySelection;
        const auto &fonts = m_context.theme.fonts;
        const auto text = [&](std::string_view key) -> const std::string & {
            return m_context.localization.Get("editor", key);
        };
        const auto label = [&](std::string_view key) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, text(key), Theme::Text(), fonts);
        };
        const auto &keys = review.keys;
        if (keys.empty()) {
            label("workspace.decision_graph.no_query_keys");
            return std::nullopt;
        }
        label("workspace.decision_graph.query_output_key");
        const auto key = std::ranges::find_if(keys, [&](const auto &entry) {
            return entry.first == selection.key;
        });
        int keyIndex = key == keys.end() ? 0 : static_cast<int>(key - keys.begin()) + 1;
        const Ui::ComboItemSource keyItems{.label = [&](int slot) {
            return slot == 0 ? text("workspace.decision_graph.select_query_key").c_str()
                             : keys[static_cast<std::size_t>(slot - 1)].second.c_str();
        }};
        static_cast<void>(Ui::ComboControl("##BehaviorTreeQueryKey", &keyIndex, static_cast<int>(keys.size()) + 1, keyItems, fonts));
        selection.key = keyIndex == 0 ? AI::BlackboardKeyId{} : keys[static_cast<std::size_t>(keyIndex - 1)].first;
        label("workspace.decision_graph.query_timeout");
        static_cast<void>(Ui::InputTextControl("##BehaviorTreeQueryTimeout", selection.timeout, 11, fonts));
        std::uint32_t timeout{};
        const auto *end = selection.timeout.data() + selection.timeout.size();
        const auto parsed = std::from_chars(selection.timeout.data(), end, timeout);
        if (selection.key.IsValid() && parsed.ec == std::errc{} && parsed.ptr == end && timeout != 0)
            return timeout;
        return std::nullopt;
    }

    /** @copydoc EditorWorkspaceView::DrawBehaviorTreeTopology */
    void EditorWorkspaceView::DrawBehaviorTreeTopology(const EditorWorkspaceViewModel &viewModel,
                                                       const DecisionGraphWorkspaceDocument &document,
                                                       EditorWorkspaceViewCommandData &outCommand) const {
        const auto &tree = *std::get<std::shared_ptr<const AI::BehaviorTreeAsset>>(document.graph->body);
        auto &selection = m_behaviorTreeSelection;
        if (selection.instance != document.identity.instance)
            selection = {.instance = document.identity.instance};
        const auto text = [&](std::string_view key) -> const std::string & {
            return m_context.localization.Get("editor", key);
        };
        const auto button = [&](std::string_view key, bool enabled) {
            return Ui::Button({.label = text(key).c_str(), .enabled = enabled, .font = m_context.theme.fonts.sans});
        };
        const auto emit = [&](BehaviorTreeEditOperation operation, std::optional<DecisionTaskSelection> task = {},
                              std::optional<Assets::AssetId> target = {}, std::optional<BehaviorTreeSubtreeReview> subtree = {}) {
            outCommand = {.command = EditorWorkspaceViewCommand::EditDecisionBehaviorTree,
                          .behaviorTreeEdit = BehaviorTreeEditRequest{document.identity.instance, document.revision, operation,
                                                                      selection.node, task, target, subtree}};
        };
        using enum DecisionHistoryAction;
        if (button("workspace.decision_graph.edit_save", document.dirty))
            outCommand = MakeDecisionHistoryCommand(document, Save);
        if (button("workspace.decision_graph.edit_undo", document.canUndo))
            outCommand = MakeDecisionHistoryCommand(document, Undo);
        if (button("workspace.decision_graph.edit_redo", document.canRedo))
            outCommand = MakeDecisionHistoryCommand(document, Redo);

        const auto nodes = tree.Nodes();
        if (nodes.empty())
            return;
        const auto found = std::ranges::find_if(nodes, [&](const auto &node) {
            return node.id.Value() == selection.node.Value();
        });
        auto index = found == nodes.end() ? 0 : static_cast<int>(found - nodes.begin());
        std::array<char, 24> id{};
        const Ui::ComboItemSource nodeItems{.label = [&](int slot) -> const char * {
            const auto &node = nodes[static_cast<std::size_t>(slot)];
            if (!node.displayName.empty())
                return node.displayName.c_str();
            const auto printed = std::to_chars(id.data(), id.data() + id.size() - 1, node.id.Value());
            *printed.ptr = char{};
            return id.data();
        }};
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, text("workspace.decision_graph.edit_tree_node"), Theme::Text(),
                        m_context.theme.fonts);
        static_cast<void>(Ui::ComboControl("##BehaviorTreeNode", &index, static_cast<int>(nodes.size()), nodeItems, m_context.theme.fonts));
        const auto &node = nodes[static_cast<std::size_t>(index)];
        selection.node = AI::DecisionNodeId::Create(node.id.Value()).Value();
        const auto edges = tree.Edges();
        const auto incoming = std::ranges::find(edges, node.id, &AI::BehaviorTreeEdge::targetNode);
        if (incoming == edges.end())
            return;
        const bool hasEarlier = std::ranges::any_of(edges, [&](const auto &edge) {
            return edge.sourceNode == incoming->sourceNode && edge.order < incoming->order;
        });
        const bool hasLater = std::ranges::any_of(edges, [&](const auto &edge) {
            return edge.sourceNode == incoming->sourceNode && edge.order > incoming->order;
        });
        const auto ordinal = std::ranges::count_if(edges, [&](const auto &edge) {
            return edge.sourceNode == incoming->sourceNode && edge.order < incoming->order;
        }) + 1;
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, std::format("{} {}", text("workspace.decision_graph.edit_tree_order"), ordinal),
                        Theme::Text(), m_context.theme.fonts);
        const bool room = nodes.size() < AI::BehaviorTreeAssetHardLimits::Nodes && edges.size() < AI::BehaviorTreeAssetHardLimits::Edges;
        if (button("workspace.decision_graph.edit_tree_wrap_sequence", room))
            emit(BehaviorTreeEditOperation::WrapSequence);
        if (button("workspace.decision_graph.edit_tree_wrap_selector", room))
            emit(BehaviorTreeEditOperation::WrapSelector);
        if (button("workspace.decision_graph.edit_tree_move_earlier", hasEarlier))
            emit(BehaviorTreeEditOperation::MoveEarlier);
        if (button("workspace.decision_graph.edit_tree_move_later", hasLater))
            emit(BehaviorTreeEditOperation::MoveLater);
        const auto children = static_cast<std::size_t>(std::ranges::count(edges, node.id, &AI::BehaviorTreeEdge::sourceNode));
        const bool isSubtree = node.control.kind == AI::DecisionControlKind::Subplan && node.call.has_value();
        if (isSubtree || node.kind == AI::BehaviorTreeNodeKind::Composite) {
            const auto &choices = viewModel.behaviorTreeSubtreeChoices;
            const auto chosen = std::ranges::find(choices, selection.subtree, &DecisionSchemaChoice::asset);
            auto targetIndex = chosen == choices.end() ? 0 : static_cast<int>(chosen - choices.begin()) + 1;
            const auto &empty = text("workspace.decision_graph.select_subtree");
            const Ui::ComboItemSource targets{
                .label = [&](int slot) -> const char * {
                return slot == 0 ? empty.c_str() : choices[static_cast<std::size_t>(slot - 1)].path.c_str();
            },
                .enabled =
                    [&](int slot) {
                return slot == 0 || choices[static_cast<std::size_t>(slot - 1)].asset != document.asset;
            },
            };
            const auto &fonts = m_context.theme.fonts;
            if (isSubtree)
                Ui::WrappedText(ImGui::GetContentRegionAvail().x,
                                std::format("{} {}", text("workspace.decision_graph.subtree_bound"), node.call->asset.Value()),
                                Theme::Text(), fonts);
            static_cast<void>(
                Ui::ComboControl("##BehaviorTreeSubtree", &targetIndex, static_cast<int>(choices.size()) + 1, targets, fonts));
            selection.subtree = targetIndex == 0 ? Assets::AssetId{} : choices[static_cast<std::size_t>(targetIndex - 1)].asset;
            if (button("workspace.decision_graph.review_subtree", selection.subtree.IsValid() && selection.subtree != document.asset))
                emit(BehaviorTreeEditOperation::ReviewSubtree, {}, selection.subtree);
            const auto &review = document.subtreeReview;
            const bool stale =
                document.diagnostic && document.diagnostic->code.Value() == Application::DecisionCookErrors::Stale.code.Value();
            const bool reviewed = review && !stale && review->asset == selection.subtree && review->documentRevision == document.revision &&
                                  review->catalogRevision == viewModel.availableBehaviorsRevision &&
                                  review->registryRevision == viewModel.assetRegistryRevision.value;
            if (reviewed)
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, text("workspace.decision_graph.subtree_reviewed"), Theme::Text(), fonts);
            if (isSubtree) {
                if (button("workspace.decision_graph.retarget_subtree", reviewed))
                    emit(BehaviorTreeEditOperation::RetargetSubtree, {}, {}, review);
            } else if (button("workspace.decision_graph.append_subtree", reviewed && room && children < node.maximumChildren)) {
                emit(BehaviorTreeEditOperation::AppendSubtree, {}, {}, review);
            }
        }
        if (node.kind == AI::BehaviorTreeNodeKind::Task) {
            DrawBehaviorTreeQueryOutput(viewModel, document, node, outCommand);
            const auto *parent = tree.FindNode(incoming->sourceNode);
            const auto siblings = std::ranges::count(edges, incoming->sourceNode, &AI::BehaviorTreeEdge::sourceNode);
            if (button("workspace.decision_graph.edit_tree_delete", parent && static_cast<std::size_t>(siblings) > parent->minimumChildren))
                emit(BehaviorTreeEditOperation::DeleteTask);
            return;
        }
        if (button("workspace.decision_graph.edit_tree_unwrap", children == 1))
            emit(BehaviorTreeEditOperation::UnwrapComposite);
        const auto &choices = viewModel.behaviorTreeTaskChoices;
        if (choices.empty())
            return;
        const auto chosen = std::ranges::find(choices, selection.task, &DecisionTaskChoice::type);
        int taskIndex = chosen == choices.end() ? 0 : static_cast<int>(chosen - choices.begin());
        const Ui::ComboItemSource taskItems{.label = [&](int slot) {
            return choices[static_cast<std::size_t>(slot)].label.c_str();
        }};
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, text("workspace.decision_graph.create_task"), Theme::Text(),
                        m_context.theme.fonts);
        static_cast<void>(
            Ui::ComboControl("##BehaviorTreeTask", &taskIndex, static_cast<int>(choices.size()), taskItems, m_context.theme.fonts));
        const auto &task = choices[static_cast<std::size_t>(taskIndex)];
        selection.task = task.type;
        const DecisionTaskSelection selected{task.type, task.provider, task.version, viewModel.availableBehaviorsRevision};
        if (task.queryOutput) {
            if (room && children < node.maximumChildren)
                DrawBehaviorTreeQueryOutput(viewModel, document, node, outCommand, selected);
            return;
        }
        if (button("workspace.decision_graph.edit_tree_append_task", room && children < node.maximumChildren))
            emit(BehaviorTreeEditOperation::AppendTask, selected);
    }

    /** @copydoc EditorWorkspaceView::ApplyDecisionCanvasInteraction */
    void EditorWorkspaceView::ApplyDecisionCanvasInteraction(const DecisionGraphWorkspaceDocument &document,
                                                             const DecisionCanvasInteraction &interaction,
                                                             EditorWorkspaceViewCommandData &outCommand) const {
        const auto instance = document.identity.instance;
        using enum AI::DecisionPlanKind;
        switch (document.family) {
            case StateMachine:
                if (m_stateMachineSelection.instance != instance)
                    m_stateMachineSelection = {.instance = instance};
                if (interaction.node)
                    m_stateMachineSelection.state = *interaction.node;
                if (interaction.transition)
                    m_stateMachineSelection.transition = *interaction.transition;
                break;
            case BehaviorTree:
                if (m_behaviorTreeSelection.instance != instance)
                    m_behaviorTreeSelection = {.instance = instance};
                if (interaction.node)
                    m_behaviorTreeSelection.node = *interaction.node;
                break;
            case Utility:
                if (interaction.node && *interaction.node != std::get<AI::UtilityDecisionSource>(document.graph->body).root &&
                    (m_utilityGraphSelection.instance != instance || m_utilityGraphSelection.option != *interaction.node))
                    m_utilityGraphSelection = {.instance = instance, .option = *interaction.node};
                break;
            default:
                break;
        }
        if (interaction.edit)
            outCommand = {.command = EditorWorkspaceViewCommand::EditDecisionStateMachine, .stateMachineEdit = interaction.edit};
        else if (interaction.layout)
            outCommand = {.command = EditorWorkspaceViewCommand::EditDecisionLayout, .decisionLayoutEdit = interaction.layout};
    }

    /** @copydoc EditorWorkspaceView::DrawDecisionGraphDocument */
    bool EditorWorkspaceView::DrawDecisionGraphDocument(const EditorWorkspaceViewModel &viewModel,
                                                        EditorWorkspaceViewCommandData &outCommand) const {
        m_decisionGraphCanvas.Retain(viewModel.decisionGraphDocuments);
        const auto opened = std::ranges::find_if(viewModel.decisionGraphDocuments, [&](const auto &document) {
            return document.identity.instance == viewModel.workspacePanelHost.ActiveDocument();
        });
        if (opened == viewModel.decisionGraphDocuments.end())
            return false;
        const auto summary = Summarize(*opened->graph);
        const auto text = [&](std::string_view key) -> const std::string & {
            return m_context.localization.Get("editor", key);
        };
        const auto label = [&](std::string_view value, ImVec4 color) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, value, color, m_context.theme.fonts);
        };
        label(text(summary.familyLabel), Theme::Text());
        label(std::format("{} {}", text("workspace.decision_graph.nodes"), summary.nodes), Theme::Text());
        if (summary.unknown)
            label(std::format("{} {}", text("workspace.decision_graph.unknown"), summary.unknown), Theme::Muted());
        if (opened->diagnostic) {
            const auto code = opened->diagnostic->code.Value();
            std::string_view key = opened->conflict ? "workspace.decision_graph.edit_conflict" : "workspace.decision_graph.edit_refused";
            if (code == AI::AIErrors::DecisionAssetDependencyCycle.code.Value())
                key = "workspace.decision_graph.subtree_cycle";
            else if (code == AI::AIErrors::DecisionAssetSubtreeMissing.code.Value())
                key = "workspace.decision_graph.subtree_missing";
            else if (opened->subtreeReview && opened->subtreeReview->documentRevision == opened->revision &&
                     code == Application::DecisionCookErrors::Stale.code.Value())
                key = "workspace.decision_graph.subtree_stale";
            label(text(key), Theme::Warn());
        }

        const auto drawTransitionOrder = [&](const AI::StateMachineAssetDescriptor &machine, const AI::StateMachineTransition &transition) {
            const auto order = ProjectStateMachineTransitionOrder(machine, transition);
            label(std::format("{} → {}", transition.source.Value(), transition.target.Value()), Theme::Text());
            if (!opened->editable)
                label(std::format("{} {}", text("workspace.decision_graph.transition_priority"), order.priority), Theme::Text());
            label(std::format("{} {}", text("workspace.decision_graph.transition_order"), order.rank), Theme::Text());
            label(text("workspace.decision_graph.transition_precedence"), Theme::Muted());
        };
        if (!opened->editable) {
            label(text("workspace.decision_graph.read_only"), Theme::Muted());
            const auto canvas = m_decisionGraphCanvas.Draw(*opened, m_context, viewModel.decisionProviderMetadata);
            if (const auto *machine = std::get_if<AI::StateMachineAssetDescriptor>(&opened->graph->body)) {
                auto &selection = m_stateMachineSelection;
                if (selection.instance != opened->identity.instance)
                    selection = {.instance = opened->identity.instance};
                if (canvas.transition)
                    selection.transition = *canvas.transition;
                const auto transition = std::ranges::find(machine->transitions, selection.transition, &AI::StateMachineTransition::id);
                if (transition != machine->transitions.end())
                    drawTransitionOrder(*machine, *transition);
            }
            return true;
        }
        AI::DecisionNodeId selected;
        const auto instance = opened->identity.instance;
        if (opened->family == AI::DecisionPlanKind::StateMachine && m_stateMachineSelection.instance == instance)
            selected = m_stateMachineSelection.state;
        else if (opened->family == AI::DecisionPlanKind::BehaviorTree && m_behaviorTreeSelection.instance == instance)
            selected = m_behaviorTreeSelection.node;
        else if (opened->family == AI::DecisionPlanKind::Utility && m_utilityGraphSelection.instance == instance)
            selected = m_utilityGraphSelection.option;
        if (Ui::Button({.label = text("workspace.decision_canvas.locate_selection").c_str(),
                        .enabled = selected.IsValid(),
                        .font = m_context.theme.fonts.sans}))
            m_decisionGraphCanvas.RequestFocus({instance, opened->revision}, selected);

        const auto canvas = m_decisionGraphCanvas.Draw(*opened, m_context, viewModel.decisionProviderMetadata);
        ApplyDecisionCanvasInteraction(*opened, canvas, outCommand);
        if (canvas.edit || canvas.layout)
            return true;
        if (opened->family == AI::DecisionPlanKind::BehaviorTree) {
            DrawBehaviorTreeTopology(viewModel, *opened, outCommand);
            return true;
        }

        if (opened->family == AI::DecisionPlanKind::Utility) {
            DrawUtilityGraph(viewModel, *opened, outCommand);
            return true;
        }

        const auto &machine = std::get<AI::StateMachineAssetDescriptor>(opened->graph->body);
        auto &selection = m_stateMachineSelection;
        if (selection.instance != opened->identity.instance)
            selection = {.instance = opened->identity.instance};
        const auto emit = [&](StateMachineEditOperation operation, AI::DecisionNodeId node = {}, AI::DecisionNodeId target = {},
                              AI::DecisionStateTerminal terminal = AI::DecisionStateTerminal::None) {
            outCommand = {.command = EditorWorkspaceViewCommand::EditDecisionStateMachine,
                          .stateMachineEdit =
                              StateMachineEditRequest{opened->identity.instance, opened->revision, operation, node, target, terminal}};
        };
        const auto button = [&](std::string_view key, bool enabled) {
            const auto &caption = text(key);
            return Ui::Button({.label = caption.c_str(), .enabled = enabled, .font = m_context.theme.fonts.sans});
        };
        using enum StateMachineEditOperation;
        if (button("workspace.decision_graph.edit_save", opened->dirty))
            emit(Save);
        if (button("workspace.decision_graph.edit_undo", opened->canUndo))
            emit(Undo);
        if (button("workspace.decision_graph.edit_redo", opened->canRedo))
            emit(Redo);

        const auto select = [&](const char *key, const auto &records, AI::DecisionNodeId &selected) {
            if (records.empty()) {
                selected = {};
                return;
            }
            label(text(key), Theme::Text());
            const auto found = std::ranges::find_if(records, [&](const auto &record) {
                return record.id == selected;
            });
            auto index = found == records.end() ? 0 : static_cast<int>(std::distance(records.begin(), found));
            std::array<char, 24> storage{};
            const Ui::ComboItemSource items{.label = [&](const int slot) -> const char * {
                const auto &record = records[static_cast<std::size_t>(slot)];
                const auto converted = std::to_chars(storage.data(), storage.data() + storage.size() - 1, record.id.Value());
                *converted.ptr = '\0';
                return storage.data();
            }};
            static_cast<void>(Ui::ComboControl(key, &index, static_cast<int>(records.size()), items, m_context.theme.fonts));
            selected = records[static_cast<std::size_t>(index)].id;
        };
        select("workspace.decision_graph.edit_state", machine.states, selection.state);
        const auto state = std::ranges::find(machine.states, selection.state, &AI::StateMachineState::id);
        const bool canConnect = state != machine.states.end() && state->terminal == AI::DecisionStateTerminal::None &&
                                machine.transitions.size() < AI::StateMachineLimits::Transitions;
        if (state != machine.states.end()) {
            constexpr std::array Keys{"workspace.decision_graph.edit_running", "workspace.decision_graph.succeeded",
                                      "workspace.decision_graph.failed"};
            const std::array labels{text(Keys[0]).c_str(), text(Keys[1]).c_str(), text(Keys[2]).c_str()};
            int outcome = static_cast<int>(state->terminal);
            if (Ui::ComboControl("##DecisionStateOutcome", &outcome, labels.data(), static_cast<int>(labels.size()), m_context.theme.fonts))
                emit(SetTerminal, state->id, {}, static_cast<AI::DecisionStateTerminal>(outcome));
            if (state->terminal != AI::DecisionStateTerminal::None)
                label(text("workspace.decision_graph.edit_terminal_connection_hint"), Theme::Muted());
            if (button("workspace.decision_graph.edit_add_state", canConnect && machine.states.size() < AI::StateMachineLimits::States))
                emit(AddState, state->id);
            if (button("workspace.decision_graph.edit_remove_state", CanRemoveMachineState(machine, state->id)))
                emit(RemoveState, state->id);
        }
        select("workspace.decision_graph.edit_target", machine.states, selection.target);
        if (button("workspace.decision_graph.edit_connect", canConnect && selection.target.IsValid()))
            emit(Connect, selection.state, selection.target);

        select("workspace.decision_graph.edit_transition", machine.transitions, selection.transition);
        const auto transition = std::ranges::find(machine.transitions, selection.transition, &AI::StateMachineTransition::id);
        if (transition != machine.transitions.end()) {
            drawTransitionOrder(machine, *transition);
            if (!selection.priority || selection.priority->transition != transition->id || selection.priority->revision != opened->revision)
                selection.priority = StateMachinePriorityDraft{transition->id, opened->revision, transition->priority,
                                                               std::format("##DecisionTransitionPriority/{}/{}/{}", instance.Value(),
                                                                           transition->id.Value(), opened->revision)};
            auto &draft = *selection.priority;
            static_cast<void>(Ui::DrawIntegerPropRow(text("workspace.decision_graph.transition_priority").c_str(), draft.widgetId.c_str(),
                                                     draft.value, m_context.theme.fonts));
            if (button("workspace.decision_graph.transition_apply_priority", draft.value != transition->priority))
                outCommand = {.command = EditorWorkspaceViewCommand::EditDecisionStateMachine,
                              .stateMachineEdit = StateMachineEditRequest{.instance = instance,
                                                                          .revision = draft.revision,
                                                                          .operation = SetTransitionPriority,
                                                                          .node = draft.transition,
                                                                          .priority = draft.value}};
            if (button("workspace.decision_graph.edit_remove_transition", true))
                emit(RemoveTransition, transition->id);
        } else {
            selection.priority.reset();
        }
        return true;
    }
}  // namespace Horo::Editor
