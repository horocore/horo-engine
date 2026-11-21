#include "BlackboardScalarInput.h"
#include "DecisionGraphCanvasModel.h"
#include "DecisionProblemsView.h"
#include "DecisionTaskPalette.h"
#include "DecisionTaskPropertiesView.h"
#include "EditorWorkspaceView.h"
#include "Horo/AI/BlackboardUsageInspection.h"
#include "Horo/AI/DecisionAssetValidation.h"
#include "Horo/AI/DecisionNodeRegistration.h"
#include "Horo/Application/DecisionAssetCookHost.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief Renders supported source values through shared editor text controls. */
        class DecisionPropertyValues final {
        public:
            explicit DecisionPropertyValues(const EditorGuiContext &context) : context_(context) {}

            void Draw(const AI::BehaviorTreePropertyValue &value) const {
                std::visit([this](const auto &entry) {
                    Show(entry);
                }, value);
            }

            void Label(std::string_view label) const {
                Text(label, Theme::Muted());
            }

        private:
            const std::string &Localized(const char *key) const {
                return context_.localization.Get("editor", key);
            }

            void Text(std::string_view value, ImVec4 color) const {
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, value, color, context_.theme.fonts);
            }

            template <typename Number> void NumberValue(Number value) const {
                std::array<char, 64> buffer;
                const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
                if (result.ec == std::errc{})
                    Text(std::string_view(buffer.data(), result.ptr), Theme::Text());
            }

            template <typename Number> void LabeledNumber(const char *key, Number value) const {
                Label(Localized(key));
                NumberValue(value);
            }

            void Show(std::monostate) const {
                Label(Localized("workspace.decision_graph.task_property_unavailable"));
            }

            void Show(bool value) const {
                Text(Localized(value ? "workspace.blackboard.value_true" : "workspace.blackboard.value_false"), Theme::Text());
            }

            void Show(std::int64_t value) const {
                NumberValue(value);
            }

            void Show(double value) const {
                NumberValue(value);
            }

            void Show(std::string_view value) const {
                Text(value, Theme::Text());
            }

            void Show(AI::BehaviorTreeNodeId value) const {
                NumberValue(value.Value());
            }

            void Show(const AI::BehaviorTreeOpaqueProperty &value) const {
                Label(Localized("workspace.decision_graph.task_property_opaque"));
                LabeledNumber("workspace.decision_graph.task_property_version", value.contractVersion);
            }

            void Show(const AI::BehaviorTreeQueryOutput &value) const {
                LabeledNumber("workspace.decision_graph.bound_query", value.query.Value());
                LabeledNumber("workspace.decision_graph.task_property_result", value.result.Value());
                LabeledNumber("workspace.decision_graph.task_property_version", value.resultVersion);
                LabeledNumber("workspace.decision_graph.task_property_item", value.item.Value());
                LabeledNumber("workspace.decision_graph.query_output_key", value.key.Value());
                LabeledNumber("workspace.decision_graph.query_timeout", value.timeoutTicks);
            }

            const EditorGuiContext &context_;
        };

        /** @brief Shows exact authored property sets for the selected state's registered action phases. */
        void DrawStateActionProperties(const AI::StateMachineState &state, const EditorGuiContext &context) {
            if (state.entryProvider)
                DrawDecisionTaskProperties(state.entryProvider->properties, context, "workspace.decision_graph.task_properties_entry");
            if (state.updateProvider)
                DrawDecisionTaskProperties(state.updateProvider->properties, context, "workspace.decision_graph.task_properties_update");
            if (state.exitProvider)
                DrawDecisionTaskProperties(state.exitProvider->properties, context, "workspace.decision_graph.task_properties_exit");
        }

        /** @brief Resolves one read-only canvas selection to the current immutable source properties. */
        void DrawReadOnlyDecisionProperties(const AI::DecisionGraphSource &source, AI::DecisionNodeId selected,
                                            const EditorGuiContext &context) {
            if (!selected.IsValid())
                return;
            if (const auto *machine = std::get_if<AI::StateMachineAssetDescriptor>(&source.body)) {
                const auto state = std::ranges::find(machine->states, selected, &AI::StateMachineState::id);
                if (state != machine->states.end())
                    DrawStateActionProperties(*state, context);
            } else if (const auto *tree = std::get_if<std::shared_ptr<const AI::BehaviorTreeAsset>>(&source.body); tree && *tree) {
                if (const auto *node = (*tree)->FindNode(AI::BehaviorTreeNodeId::Create(selected.Value()).Value()))
                    DrawDecisionTaskProperties(node->properties, context);
            } else if (const auto *utility = std::get_if<AI::UtilityDecisionSource>(&source.body)) {
                const auto option = std::ranges::find(utility->options, selected, [](const auto &entry) {
                    return entry.option.id;
                });
                if (option != utility->options.end())
                    DrawDecisionTaskProperties(option->properties, context);
            }
        }

        /** @brief Matches literal metadata text, folding ASCII without changing UTF-8 bytes. */
        bool MatchesTaskLabel(std::string_view label, std::string_view search) {
            const auto fold = [](char value) {
                return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
            };
            return search.empty() || !std::ranges::search(label, search, {}, fold, fold).empty();
        }

        /** @brief Distinguishes a disabled task from one awaiting authoritative compatibility checks. */
        struct TaskAvailability final {
            const char *diagnostic{};
            bool blocked{};

            [[nodiscard]] bool IsVerifiedCompatible() const noexcept {
                return !blocked && diagnostic == nullptr;
            }
        };

        /** @brief Displays the first exact compiler incompatibility for a declared Blackboard requirement. */
        const char *BindingDiagnostic(const AI::DecisionBlackboardBindingIssues &issues) {
            if (issues.missingKey)
                return "workspace.decision_graph.task_missing_key";
            if (issues.typeMismatch)
                return "workspace.decision_graph.task_key_type";
            if (issues.accessMismatch)
                return "workspace.decision_graph.task_key_access";
            if (issues.presenceMismatch)
                return "workspace.decision_graph.task_key_presence";
            if (issues.missingDefault)
                return "workspace.decision_graph.task_key_default";
            return nullptr;
        }

        /** @brief Reports family-independent restrictions from the exact authoring generation. */
        const char *TaskAdmissionDiagnostic(DecisionPaletteAdmission admission) noexcept {
            using enum DecisionPaletteAdmission;
            switch (admission) {
                case Allowed:
                    return nullptr;
                case UnsupportedRole:
                    return "workspace.decision_graph.palette_role_unavailable";
                case CapabilitiesUnavailable:
                    return "workspace.decision_graph.palette_capability_unavailable";
                case PortWiringRequired:
                    return "workspace.decision_graph.palette_ports_required";
                case UnsupportedProperties:
                    return "workspace.decision_graph.palette_properties_unavailable";
                case ImplementationUnavailable:
                    return "workspace.decision_graph.palette_implementation_unavailable";
            }
            return "workspace.decision_graph.palette_implementation_unavailable";
        }

        /** @brief Checks the capacity and wiring offered by the active authoring entry point. */
        const char *TaskPlacementDiagnostic(const DecisionTaskPaletteCatalog &catalog, const DecisionTaskChoice &choice) noexcept {
            if (!catalog.allowQueryOutput && choice.queryOutput)
                return "workspace.decision_graph.palette_query_requires_tree";
            if (catalog.initialFamily && choice.queryOutput)
                return "workspace.decision_graph.palette_query_requires_wiring";
            if (catalog.initialFamily == AI::DecisionPlanKind::BehaviorTree && choice.version > std::numeric_limits<std::uint16_t>::max())
                return "workspace.decision_canvas.provider_version_mismatch";
            return nullptr;
        }

        /** @brief Reports the first incompatibility against a verified saved Blackboard schema. */
        TaskAvailability InspectTaskSchema(const AI::BlackboardSchema *schema,
                                           std::span<const AI::DecisionBlackboardRequirement> requirements) {
            if (requirements.empty())
                return {};
            if (!schema)
                return {"workspace.decision_graph.task_schema_unverified"};
            const auto keys = schema->Keys();
            for (const auto &requirement : requirements) {
                const auto key = std::ranges::lower_bound(keys, requirement.key, {}, &AI::BlackboardKeyDescriptor::key);
                const auto issues = AI::InspectDecisionBlackboardBinding(requirement, key == keys.end() ? nullptr : &*key);
                if (const auto *reason = BindingDiagnostic(issues))
                    return {reason, true};
            }
            return {};
        }

        /** @brief Uses exact provider and schema evidence without calling a provider or weakening final compiler validation. */
        TaskAvailability InspectPaletteTask(const DecisionTaskPaletteCatalog &catalog, const DecisionTaskChoice &choice) {
            if (const auto *reason = TaskAdmissionDiagnostic(choice.admission))
                return {reason, true};
            if (const auto *reason = TaskPlacementDiagnostic(catalog, choice))
                return {reason, true};
            if (!catalog.providers)
                return {"workspace.decision_canvas.provider_catalog_unavailable"};
            const auto descriptors = catalog.providers->Descriptors();
            const auto descriptor = std::ranges::lower_bound(descriptors, choice.type, {}, &AI::DecisionNodeDescriptor::type);
            if (descriptor == descriptors.end() || descriptor->type != choice.type || descriptor->origin.provider != choice.provider ||
                descriptor->origin.kind != choice.source)
                return {"workspace.decision_canvas.provider_missing", true};
            if (descriptor->origin.version != choice.version)
                return {"workspace.decision_canvas.provider_version_mismatch", true};
            return InspectTaskSchema(catalog.schema, descriptor->requirements);
        }

        /** @brief Resolves a persisted node identity against the authored family currently shown by the canvas. */
        bool HasProblemNode(const AI::DecisionGraphSource &graph, const AI::DecisionNodeId node) {
            if (!node.IsValid())
                return false;
            return std::visit([node](const auto &source) {
                using Source = std::remove_cvref_t<decltype(source)>;
                if constexpr (std::is_same_v<Source, std::shared_ptr<const AI::BehaviorTreeAsset>>) {
                    return source && source->FindNode(AI::BehaviorTreeNodeId::Create(node.Value()).Value());
                } else if constexpr (std::is_same_v<Source, AI::StateMachineAssetDescriptor>) {
                    return std::ranges::find(source.states, node, &AI::StateMachineState::id) != source.states.end();
                } else {
                    return source.root == node || std::ranges::any_of(source.options, [node](const auto &entry) {
                        return entry.option.id == node;
                    });
                }
            }, graph.body);
        }

        /** @brief Uses the same localized severity vocabulary as Build Output. */
        const char *ProblemSeverityLabel(const DiagnosticSeverity severity) {
            using enum DiagnosticSeverity;
            switch (severity) {
                case Warning:
                    return "workspace.global_dock.build_output.row_status.warning";
                case Error:
                    return "workspace.global_dock.console.row.error";
                case Fatal:
                    return "workspace.global_dock.build_output.row_status.fatal";
                default:
                    return "workspace.global_dock.build_output.row_status.info";
            }
        }

        /** @brief Formats the exact source span without interpreting the diagnostic's semantic path. */
        std::string ProblemSourceLabel(const SourceLocation &source) {
            if (!source.line)
                return source.source;
            if (!source.column)
                return std::format("{}:{}", source.source, source.line);
            return std::format("{}:{}:{}", source.source, source.line, source.column);
        }

        /** @brief Renders one finding and admits only actions owned by that finding and the active graph. */
        AI::DecisionNodeId DrawDecisionProblem(const DecisionGraphWorkspaceDocument &document,
                                               const AI::DecisionAssetValidationDiagnostic &finding, const EditorGuiContext &context,
                                               EditorWorkspaceViewCommandData &command) {
            const auto text = [&](const char *key) -> const std::string & {
                return context.localization.Get("editor", key);
            };
            const auto label = [&](std::string_view value, ImVec4 color) {
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, value, color, context.theme.fonts);
            };
            const auto severity = finding.severity >= DiagnosticSeverity::Error
                                      ? Theme::Err()
                                      : (finding.severity == DiagnosticSeverity::Warning ? Theme::Warn() : Theme::Text());
            label(std::format("{} · {} · {}", text(ProblemSeverityLabel(finding.severity)), finding.code.Value(), finding.message),
                  severity);
            if (!finding.source.source.empty())
                label(ProblemSourceLabel(finding.source), Theme::Muted());
            const auto &target = finding.target;
            if (target.asset != document.graph->asset)
                return {};
            const DecisionGraphDocumentFence fence{document.identity.instance, document.revision};
            if (target.blackboard == document.graph->blackboard && target.key.IsValid() &&
                Ui::Button({.label = text("workspace.decision_graph.open_diagnostic_key").c_str(), .font = context.theme.fonts.sans})) {
                command.command = EditorWorkspaceViewCommand::OpenDecisionDiagnosticKey;
                command.decisionDiagnosticKey = DecisionDiagnosticKeyRequest{fence, target.blackboard, target.key};
            }
            if (target.node.IsValid()) {
                if (!HasProblemNode(*document.graph, target.node))
                    label(text("workspace.decision_graph.problem_node_missing"), Theme::Muted());
                else if (Ui::Button(
                             {.label = text("workspace.decision_graph.locate_problem_node").c_str(), .font = context.theme.fonts.sans}))
                    return target.node;
            }
            return {};
        }

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

    /** @copydoc EditorWorkspaceView::DecisionTaskPropertyDraft::Select */
    void EditorWorkspaceView::DecisionTaskPropertyDraft::Select(const AI::BehaviorTreeProperty &effective) {
        property = effective.id;
        fieldScope = std::format("{}/{}/{}/{}/{}", instance.Value(), node.Value(), revision, catalogRevision, property.Value());
        text.fill('\0');
        if (const auto *string = std::get_if<std::string>(&effective.value)) {
            std::ranges::copy(*string, text.begin());
        } else if (const auto *boolean = std::get_if<bool>(&effective.value)) {
            checked = *boolean;
        } else if (const auto *integer = std::get_if<std::int64_t>(&effective.value)) {
            static_cast<void>(std::to_chars(text.data(), text.data() + text.size() - 1, *integer));
        } else if (const auto *number = std::get_if<double>(&effective.value)) {
            static_cast<void>(std::to_chars(text.data(), text.data() + text.size() - 1, *number));
        } else if (const auto *target = std::get_if<AI::BehaviorTreeNodeId>(&effective.value)) {
            reference = *target;
        }
    }

    namespace {
        /** @brief Draws stable references from exactly the current tree; an unresolved default remains unselected. */
        AI::BehaviorTreeNodeId DrawDecisionTreeReference(std::span<const AI::BehaviorTreeNode> nodes, AI::BehaviorTreeNodeId selected,
                                                         const EditorGuiContext &context) {
            const auto found = std::ranges::lower_bound(nodes, selected, {}, &AI::BehaviorTreeNode::id);
            auto row = found == nodes.end() || found->id != selected ? 0 : static_cast<int>(found - nodes.begin()) + 1;
            const auto &placeholder = context.localization.Get("editor", "workspace.decision_graph.edit_property_select_node");
            std::array<char, AI::BehaviorTreeAssetHardLimits::DisplayNameBytes + 25> label;
            const Ui::ComboItemSource options{.label = [&](int index) -> const char * {
                if (index == 0)
                    return placeholder.c_str();
                const auto &node = nodes[static_cast<std::size_t>(index - 1)];
                auto *end = label.data() + label.size() - 1;
                if (node.displayName.empty())
                    *std::to_chars(label.data(), end, node.id.Value()).ptr = '\0';
                else
                    *std::format_to_n(label.data(), end - label.data(), "{} ({})", node.displayName, node.id.Value()).out = '\0';
                return label.data();
            }};
            static_cast<void>(
                Ui::ComboControl("##DecisionTaskReference", &row, static_cast<int>(nodes.size()) + 1, options, context.theme.fonts));
            return row == 0 ? AI::BehaviorTreeNodeId{} : nodes[static_cast<std::size_t>(row - 1)].id;
        }
    }  // namespace

    /** @copydoc EditorWorkspaceView::DrawDecisionTaskPropertyValue */
    std::optional<AI::BehaviorTreePropertyValue> EditorWorkspaceView::DrawDecisionTaskPropertyValue(
        const AI::BehaviorTreeProperty &effective, std::string_view label, std::size_t textLimit,
        std::span<const AI::BehaviorTreeNode> referenceNodes, DecisionTaskPropertyDraft &draft,
        EditorWorkspaceViewCommandData &outCommand) const {
        const auto &fonts = m_context.theme.fonts;
        const auto text = [&](const char *key) -> const std::string & {
            return m_context.localization.Get("editor", key);
        };
        ImGui::PushID(draft.fieldScope.c_str());
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, label, Theme::Text(), fonts);
        std::optional<AI::BehaviorTreePropertyValue> value;
        bool changed = false;
        if (const auto *original = std::get_if<AI::BehaviorTreeNodeId>(&effective.value)) {
            draft.reference = DrawDecisionTreeReference(referenceNodes, draft.reference, m_context);
            value = draft.reference;
            changed = draft.reference.IsValid() && draft.reference != *original;
        } else if (const auto *original = std::get_if<bool>(&effective.value)) {
            static_cast<void>(Ui::CheckboxControl(text("workspace.decision_graph.edit_property_enabled").c_str(), &draft.checked, fonts));
            value = draft.checked;
            changed = draft.checked != *original;
        } else {
            static_cast<void>(Ui::InputTextControl("##DecisionTaskPropertyValue", draft.text.data(), draft.text.size(), fonts));
            outCommand.sourceTextInputOwned = outCommand.sourceTextInputOwned || ImGui::IsItemActive();
            if (const auto *original = std::get_if<std::string>(&effective.value)) {
                const std::string_view edited{draft.text.data()};
                if (edited.size() <= textLimit) {
                    changed = edited != *original;
                } else {
                    Ui::WrappedText(ImGui::GetContentRegionAvail().x, text("workspace.decision_graph.edit_property_text_too_long"),
                                    Theme::Warn(), fonts);
                }
            } else {
                const auto kind = effective.kind == AI::BehaviorTreePropertyKind::SignedInteger ? AI::BlackboardValueKind::SignedInteger
                                                                                                : AI::BlackboardValueKind::Scalar;
                const auto number = ReadBlackboardNumericDefault(kind, draft.text.data());
                if (number) {
                    value = kind == AI::BlackboardValueKind::SignedInteger ? AI::BehaviorTreePropertyValue{std::get<std::int64_t>(*number)}
                                                                           : AI::BehaviorTreePropertyValue{std::get<double>(*number)};
                    changed = *value != effective.value;
                } else {
                    Ui::WrappedText(ImGui::GetContentRegionAvail().x, text("workspace.decision_graph.edit_property_invalid_number"),
                                    Theme::Warn(), fonts);
                }
            }
        }
        const bool apply =
            Ui::Button({.label = text("workspace.decision_graph.edit_property_apply").c_str(), .enabled = changed, .font = fonts.sans});
        ImGui::PopID();
        if (!apply)
            return std::nullopt;
        if (!value)
            value = std::string{draft.text.data()};
        return value;
    }

    /** @copydoc EditorWorkspaceView::DrawDecisionTaskPropertyEditor */
    std::optional<EditorWorkspaceView::DecisionTaskPropertyIntent> EditorWorkspaceView::DrawDecisionTaskPropertyEditor(
        const EditorWorkspaceViewModel &viewModel, const DecisionGraphWorkspaceDocument &document,
        const DecisionTaskPropertyBinding &binding, DecisionTaskPropertyDraft &draft, EditorWorkspaceViewCommandData &outCommand) const {
        const auto &choices = viewModel.behaviorTreeTaskChoices;
        const auto contract = std::ranges::find_if(choices, [&](const DecisionTaskChoice &choice) {
            return choice.type == binding.type && binding.versions.Contains(choice.version);
        });
        if (contract == choices.end() || contract->editableProperties.empty())
            return std::nullopt;

        const auto instance = document.identity.instance;
        const auto catalogRevision = viewModel.availableBehaviorsRevision;
        if (draft.instance != instance || draft.node != binding.node || draft.revision != document.revision ||
            draft.catalogRevision != catalogRevision) {
            draft = {.instance = instance, .node = binding.node, .revision = document.revision, .catalogRevision = catalogRevision};
        }
        const auto &fonts = m_context.theme.fonts;
        const auto text = [&](const char *key) -> const std::string & {
            return m_context.localization.Get("editor", key);
        };
        std::array<const AI::BehaviorTreeProperty *, AI::BehaviorTreeAssetHardLimits::PropertiesPerNode> fields;
        std::size_t count = 0;
        int row = 0;
        for (const auto &property : contract->editableProperties) {
            if (!CanEditDecisionTaskProperty(property.kind, document.family))
                continue;
            fields[count++] = &property;
            if (property.id == draft.property)
                row = static_cast<int>(count);
        }
        if (count == 0)
            return std::nullopt;
        const auto &placeholder = text("workspace.decision_graph.edit_property_select");
        const Ui::ComboItemSource items{.label = [&](int index) {
            return index == 0 ? placeholder.c_str() : fields[static_cast<std::size_t>(index - 1)]->displayName.c_str();
        }};
        Ui::FieldLabel(text("workspace.decision_graph.edit_property").c_str(), fonts);
        static_cast<void>(Ui::ComboControl("##DecisionTaskProperty", &row, static_cast<int>(count) + 1, items, fonts));
        if (row == 0) {
            draft.property = {};
            return std::nullopt;
        }
        const auto &declaration = *fields[static_cast<std::size_t>(row - 1)];
        const auto authored = std::ranges::find(binding.properties, declaration.id, &AI::BehaviorTreeProperty::id);
        if (authored != binding.properties.end() && authored->kind != declaration.kind)
            return std::nullopt;
        const auto &property = authored != binding.properties.end() ? *authored : declaration;
        if (draft.property != property.id)
            draft.Select(property);
        const auto textLimit = document.family == AI::DecisionPlanKind::BehaviorTree ? AI::BehaviorTreeAssetHardLimits::DisplayNameBytes
                                                                                     : AI::BehaviorTreeAssetHardLimits::PropertyBytes;
        const auto *tree = std::get_if<std::shared_ptr<const AI::BehaviorTreeAsset>>(&document.graph->body);
        const auto references = tree && *tree ? (*tree)->Nodes() : std::span<const AI::BehaviorTreeNode>{};
        if (auto value = DrawDecisionTaskPropertyValue(property, declaration.displayName, textLimit, references, draft, outCommand)) {
            return DecisionTaskPropertyIntent{{contract->type, contract->provider, contract->version, catalogRevision},
                                              {property.id, std::move(*value)}};
        }
        return std::nullopt;
    }

    /** @copydoc EditorWorkspaceView::DrawStateMachineTaskPropertyEditors */
    void EditorWorkspaceView::DrawStateMachineTaskPropertyEditors(const EditorWorkspaceViewModel &viewModel,
                                                                  const DecisionGraphWorkspaceDocument &document,
                                                                  const AI::StateMachineState &state,
                                                                  EditorWorkspaceViewCommandData &outCommand) const {
        constexpr std::array headings{"workspace.decision_graph.task_properties_entry", "workspace.decision_graph.task_properties_update",
                                      "workspace.decision_graph.task_properties_exit"};
        for (std::size_t index = 0; index < headings.size(); ++index) {
            const auto phase = static_cast<StateMachineTaskPhase>(index);
            const auto &action = state.*StateMachineTaskSlot(phase);
            if (!action)
                continue;
            const DecisionTaskPropertyBinding binding{action->binding.id, action->binding.type, action->binding.descriptorVersion,
                                                      action->properties};
            ImGui::PushID(headings[index]);
            DrawDecisionTaskProperties(action->properties, m_context, headings[index]);
            Ui::FieldLabel(m_context.localization.Get("editor", headings[index]).c_str(), m_context.theme.fonts);
            auto edit = DrawDecisionTaskPropertyEditor(viewModel, document, binding, m_stateTaskPropertyDrafts[index], outCommand);
            ImGui::PopID();
            if (edit) {
                outCommand = {.command = EditorWorkspaceViewCommand::EditDecisionStateMachine,
                              .stateMachineEdit = StateMachineEditRequest{.instance = document.identity.instance,
                                                                          .revision = document.revision,
                                                                          .operation = StateMachineEditOperation::SetTaskProperty,
                                                                          .node = state.id,
                                                                          .taskPhase = phase,
                                                                          .task = edit->task,
                                                                          .property = std::move(edit->property)}};
            }
        }
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
        DrawDecisionTaskProperties(node.properties, m_context);
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
            const auto type = AI::DecisionNodeTypeId::Create(node.type.Value());
            if (node.control.kind == AI::DecisionControlKind::Task && type.HasValue()) {
                const AI::DecisionVersionRange version{node.typeVersion.major, node.typeVersion.major};
                const DecisionTaskPropertyBinding binding{selection.node, type.Value(), version, node.properties};
                if (auto edit = DrawDecisionTaskPropertyEditor(viewModel, document, binding, m_behaviorTreeTaskPropertyDraft, outCommand)) {
                    outCommand = {.command = EditorWorkspaceViewCommand::EditDecisionBehaviorTree,
                                  .behaviorTreeEdit = BehaviorTreeEditRequest{.instance = document.identity.instance,
                                                                              .revision = document.revision,
                                                                              .operation = BehaviorTreeEditOperation::SetTaskProperty,
                                                                              .node = selection.node,
                                                                              .task = edit->task,
                                                                              .property = std::move(edit->property)}};
                }
            }
            DrawBehaviorTreeQueryOutput(viewModel, document, node, outCommand);
            const auto *parent = tree.FindNode(incoming->sourceNode);
            const auto siblings = std::ranges::count(edges, incoming->sourceNode, &AI::BehaviorTreeEdge::sourceNode);
            if (button("workspace.decision_graph.edit_tree_delete", parent && static_cast<std::size_t>(siblings) > parent->minimumChildren))
                emit(BehaviorTreeEditOperation::DeleteTask);
            return;
        }
        if (button("workspace.decision_graph.edit_tree_unwrap", children == 1))
            emit(BehaviorTreeEditOperation::UnwrapComposite);
        const auto *schema = FindSavedDecisionSchema(viewModel, *document.graph);
        const auto *task = DrawDecisionTaskPalette({viewModel.decisionTaskPaletteChoices, viewModel.availableBehaviorsRevision,
                                                    viewModel.decisionProviderMetadata.get(), schema ? schema->schema.get() : nullptr},
                                                   selection.palette, m_context, outCommand);
        if (!task)
            return;
        const DecisionTaskSelection selected{task->type, task->provider, task->version, viewModel.availableBehaviorsRevision};
        if (task->queryOutput) {
            if (room && children < node.maximumChildren)
                DrawBehaviorTreeQueryOutput(viewModel, document, node, outCommand, selected);
            return;
        }
        if (button("workspace.decision_graph.edit_tree_append_task", room && children < node.maximumChildren))
            emit(BehaviorTreeEditOperation::AppendTask, selected);
    }

    /** @copydoc DrawDecisionTaskProperties */
    void DrawDecisionTaskProperties(std::span<const AI::BehaviorTreeProperty> properties, const EditorGuiContext &context,
                                    const char *heading) {
        if (properties.empty())
            return;
        Ui::Card card({.id = heading});
        const auto &title = context.localization.Get("editor", heading);
        card.DrawTitleBar({.id = "##PropertyHeading", .title = title.c_str(), .fonts = context.theme.fonts});
        if (!card.BeginBody())
            return;
        const DecisionPropertyValues view{context};
        for (const auto &property : properties) {
            view.Label(property.displayName);
            view.Draw(property.value);
        }
    }

    /** @copydoc DrawDecisionPackageTrustRequired */
    void DrawDecisionPackageTrustRequired(bool required, const EditorGuiContext &context) {
        if (required) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x,
                            context.localization.Get("editor", "workspace.packages.execution_trust_required"), Theme::Warn(),
                            context.theme.fonts);
        }
    }

    /** @copydoc FindSavedDecisionSchema */
    const BlackboardSchemaDocumentSnapshot *FindSavedDecisionSchema(const EditorWorkspaceViewModel &model,
                                                                    const AI::DecisionGraphSource &source) {
        const BlackboardSchemaDocumentSnapshot *matched{};
        for (const auto &document : model.blackboardDocuments) {
            const auto &snapshot = document.snapshot;
            if (!snapshot.source || !snapshot.schema || !snapshot.sourceDigest || snapshot.dirty || snapshot.conflict)
                continue;
            if (snapshot.schema->Identity() != source.blackboard || !source.blackboardVersion.Contains(snapshot.schema->Version()))
                continue;
            if (matched)
                return nullptr;
            matched = &snapshot;
        }
        return matched;
    }

    namespace {
        /** @brief Shows only filter values contributed by this family and keeps selection in typed state. */
        template <typename Kind, std::size_t Size>
        void DrawTaskFilter(const EditorGuiContext &context, const char *id, const char *label, const std::array<bool, Size> &present,
                            const std::array<const char *, Size> &names, std::optional<Kind> &filter) {
            std::array<std::size_t, Size> choices;
            int count{};
            int selected{};
            for (std::size_t index = 0; index < Size; ++index) {
                if (present[index]) {
                    choices[static_cast<std::size_t>(count++)] = index;
                    if (filter == static_cast<Kind>(index))
                        selected = count;
                }
            }
            if (selected == 0)
                filter.reset();
            if (count == 0)
                return;
            const auto &fonts = context.theme.fonts;
            const auto &localization = context.localization;
            Ui::FieldLabel(localization.Get("editor", label).c_str(), fonts);
            const Ui::ComboItemSource items{.label = [&](int row) {
                const auto *key = row == 0 ? "workspace.decision_graph.task_filter_all" : names[choices[static_cast<std::size_t>(row - 1)]];
                return localization.Get("editor", key).c_str();
            }};
            if (Ui::ComboControl(id, &selected, count + 1, items, fonts))
                filter = selected == 0 ? std::nullopt : std::optional{static_cast<Kind>(choices[static_cast<std::size_t>(selected - 1)])};
        }

        /** @brief Projects capability and provider origin values from the existing admitted task choices. */
        void DrawTaskMetadataFilters(const EditorGuiContext &context, std::span<const DecisionTaskChoice> choices,
                                     DecisionTaskPaletteDraft &draft) {
            constexpr auto CapabilityCount = static_cast<std::size_t>(AI::AiCapability::Count);
            constexpr auto OriginCount = static_cast<std::size_t>(AI::DecisionDescriptorSourceKind::Count);
            constexpr auto RoleCount = static_cast<std::size_t>(AI::DecisionNodeRole::Count);
            constexpr std::array CapabilityLabels{"workspace.decision_graph.task_filter_behavior",
                                                  "workspace.decision_graph.task_filter_navigation",
                                                  "workspace.decision_graph.task_filter_perception"};
            constexpr std::array OriginLabels{"workspace.decision_graph.task_filter_native", "workspace.decision_graph.task_filter_package",
                                              "workspace.decision_graph.task_filter_script"};
            constexpr std::array RoleLabels{"workspace.decision_graph.task_filter_role_task",
                                            "workspace.decision_graph.task_filter_role_condition",
                                            "workspace.decision_graph.task_filter_role_service"};
            AI::AiCapabilitySet capabilities;
            std::array<bool, OriginCount> origins{};
            std::array<bool, RoleCount> roles{};
            for (const auto &choice : choices) {
                capabilities = capabilities.Union(choice.capabilities);
                const auto origin = static_cast<std::size_t>(choice.source);
                if (origin < origins.size())
                    origins[origin] = true;
                const auto role = static_cast<std::size_t>(choice.role);
                if (role < roles.size())
                    roles[role] = true;
            }
            std::array<bool, CapabilityCount> present{};
            for (std::size_t index = 0; index < present.size(); ++index)
                present[index] = capabilities.Contains(static_cast<AI::AiCapability>(index));
            DrawTaskFilter(context, "##DecisionTaskCapability", "workspace.decision_graph.task_filter_capability", present,
                           CapabilityLabels, draft.capability);
            if (std::ranges::count(origins, true) > 1)
                DrawTaskFilter(context, "##DecisionTaskOrigin", "workspace.decision_graph.task_filter_origin", origins, OriginLabels,
                               draft.origin);
            else
                draft.origin.reset();
            if (std::ranges::count(roles, true) > 1)
                DrawTaskFilter(context, "##DecisionTaskRole", "workspace.decision_graph.task_filter_role", roles, RoleLabels, draft.role);
            else
                draft.role.reset();
        }

        /** @brief Applies exact descriptor filters without changing the catalog or the current selected type. */
        bool MatchesTaskMetadata(const DecisionTaskChoice &choice, const DecisionTaskPaletteDraft &draft) noexcept {
            return (!draft.capability || choice.capabilities.Contains(*draft.capability)) &&
                   (!draft.origin || choice.source == *draft.origin) && (!draft.role || choice.role == *draft.role);
        }
    }  // namespace

    /** @copydoc DrawDecisionTaskPalette */
    const DecisionTaskChoice *DrawDecisionTaskPalette(const DecisionTaskPaletteCatalog &catalog, DecisionTaskPaletteDraft &draft,
                                                      const EditorGuiContext &context, EditorWorkspaceViewCommandData &command) {
        if (draft.catalogRevision != catalog.revision) {
            draft.catalogRevision = catalog.revision;
            draft.selected = {};
        }
        const auto choices = catalog.choices;
        if (choices.empty()) {
            draft.capability.reset();
            draft.origin.reset();
            draft.role.reset();
            return nullptr;
        }

        const auto text = [&](std::string_view key) -> const std::string & {
            return context.localization.Get("editor", key);
        };
        const auto &fonts = context.theme.fonts;
        Ui::FieldLabel(text("workspace.decision_graph.palette_registered_nodes").c_str(), fonts);
        static_cast<void>(Ui::InputTextControl("##DecisionTaskSearch", draft.search.data(), draft.search.size(), fonts,
                                               {.hint = text("workspace.source_edit.find").c_str()}));
        command.sourceTextInputOwned = command.sourceTextInputOwned || ImGui::IsItemActive();
        DrawTaskMetadataFilters(context, choices, draft);
        static_cast<void>(Ui::CheckboxControl(text("workspace.decision_graph.task_filter_verified_compatible").c_str(),
                                              &draft.verifiedCompatibleOnly, fonts));

        struct PaletteRow final {
            std::size_t choice{};
            std::optional<TaskAvailability> evidence; /**< Reused by filtering, the selected row and shared combo callbacks. */
        };

        constexpr auto Maximum = AI::DecisionNodeGeneration::MaximumNodes;
        std::array<PaletteRow, Maximum> visible;
        const std::string_view search(draft.search.begin(), std::ranges::find(draft.search, '\0'));
        int count = 0;
        int selection = 0;
        for (std::size_t index = 0; index < std::min(choices.size(), Maximum); ++index) {
            if (!MatchesTaskMetadata(choices[index], draft) || !MatchesTaskLabel(choices[index].label, search))
                continue;
            PaletteRow row{.choice = index};
            if (draft.verifiedCompatibleOnly) {
                row.evidence = InspectPaletteTask(catalog, choices[index]);
                if (!row.evidence->IsVerifiedCompatible())
                    continue;
            }
            visible[static_cast<std::size_t>(count++)] = row;
            if (choices[index].type == draft.selected)
                selection = count;
        }
        if (count == 0) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, text("workspace.source_edit.no_match"), Theme::Text(), fonts);
            return nullptr;
        }
        const auto &placeholder = text("workspace.decision_graph.select_task");
        const auto choice = [&](int row) -> const DecisionTaskChoice & {
            return choices[visible[static_cast<std::size_t>(row - 1)].choice];
        };
        const auto availability = [&](int row) {
            if (row == 0)
                return TaskAvailability{};
            auto &entry = visible[static_cast<std::size_t>(row - 1)];
            if (!entry.evidence)
                entry.evidence = InspectPaletteTask(catalog, choices[entry.choice]);
            return *entry.evidence;
        };
        auto selectedAvailability = availability(selection);
        if (selectedAvailability.blocked)
            selection = 0;
        const Ui::ComboItemSource items{
            .label =
                [&](int row) {
            return row == 0 ? placeholder.c_str() : choice(row).label.c_str();
        },
            .enabled =
                [&](int row) {
            return !availability(row).blocked;
        },
            .disabledTooltip = [&](int row) -> const char * {
            const auto *diagnostic = availability(row).diagnostic;
            return diagnostic ? text(diagnostic).c_str() : nullptr;
        },
        };
        if (Ui::ComboControl("##DecisionTaskChoice", &selection, count + 1, items, fonts)) {
            draft.selected = selection == 0 ? AI::DecisionNodeTypeId{} : choice(selection).type;
            selectedAvailability = availability(selection);
        }
        if (selectedAvailability.diagnostic)
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, text(selectedAvailability.diagnostic), Theme::Muted(), fonts);
        return selection == 0 ? nullptr : &choice(selection);
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

    /** @copydoc DrawDecisionProblems */
    AI::DecisionNodeId DrawDecisionProblems(const DecisionGraphWorkspaceDocument &document,
                                            std::span<const AI::DecisionAssetValidationDiagnostic> findings,
                                            const EditorGuiContext &context, EditorWorkspaceViewCommandData &command) {
        if (findings.empty())
            return {};
        const auto text = [&](const char *key) -> const std::string & {
            return context.localization.Get("editor", key);
        };
        const auto documentId = std::to_string(document.identity.instance.Value());
        ImGui::PushID(documentId.c_str());
        auto &storage = *ImGui::GetStateStorage();
        const auto pageId = ImGui::GetID("##DecisionProblemsPage");
        const auto heading = std::format("{} ({})", text("workspace.decision_graph.problems"), findings.size());
        Ui::Card card({.id = "##DecisionProblems"});
        card.DrawTitleBar({.id = "##title", .title = heading.c_str(), .fonts = context.theme.fonts});
        AI::DecisionNodeId focus;
        if (card.BeginBody()) {
            constexpr std::size_t PageSize = 6;
            const auto lastPage = static_cast<int>((findings.size() - 1) / PageSize);
            auto page = std::clamp(storage.GetInt(pageId), 0, lastPage);
            const auto start = static_cast<std::size_t>(page) * PageSize;
            const auto visible = findings.subspan(start, std::min(PageSize, findings.size() - start));
            for (std::size_t index = 0; index < visible.size(); ++index) {
                ImGui::PushID(static_cast<int>(start + index));
                if (const auto node = DrawDecisionProblem(document, visible[index], context, command); node.IsValid())
                    focus = node;
                ImGui::PopID();
                if (index + 1 < visible.size())
                    ImGui::Separator();
            }
            if (lastPage > 0) {
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, std::format("{} / {}", page + 1, lastPage + 1), Theme::Muted(),
                                context.theme.fonts);
                if (Ui::Button({.label = text("workspace.blackboard.usage_previous").c_str(),
                                .enabled = page > 0,
                                .font = context.theme.fonts.sans}))
                    --page;
                if (Ui::Button({.label = text("workspace.blackboard.usage_next").c_str(),
                                .enabled = page < lastPage,
                                .font = context.theme.fonts.sans}))
                    ++page;
                storage.SetInt(pageId, page);
            }
        }
        card.Finish();
        ImGui::PopID();
        return focus;
    }

    namespace {
        /** @brief Presents explicit recovery actions for the currently projected checkpoint. */
        bool DrawDecisionRecovery(const DecisionGraphWorkspaceDocument &document, const EditorGuiContext &context,
                                  EditorWorkspaceViewCommandData &outCommand) {
            if (!document.recovery)
                return false;
            constexpr GuiLocalizationScope text{"workspace.decision_graph"};
            const auto &fonts = context.theme.fonts;
            const bool restore = Ui::Button({.label = text.Text(context.localization, "restore_recovery").c_str(),
                                             .enabled = !document.dirty && !document.conflict,
                                             .font = fonts.sans});
            const bool discard = Ui::Button({.label = text.Text(context.localization, "discard_recovery").c_str(), .font = fonts.sans});
            if (!restore && !discard)
                return false;
            outCommand = {.command = restore ? EditorWorkspaceViewCommand::RestoreDecisionGraphRecovery
                                             : EditorWorkspaceViewCommand::DiscardDecisionGraphRecovery,
                          .decisionGraphFence = DecisionGraphDocumentFence{document.identity.instance, document.revision},
                          .decisionGraphRecovery = document.recovery};
            return true;
        }
    }  // namespace

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
        DrawDecisionPackageTrustRequired(viewModel.decisionPackageActivationTrustRequired, m_context);
        label(std::format("{} {}", text("workspace.decision_graph.nodes"), summary.nodes), Theme::Text());
        if (summary.unknown)
            label(std::format("{} {}", text("workspace.decision_graph.unknown"), summary.unknown), Theme::Muted());
        if (opened->diagnostic) {
            const auto code = opened->diagnostic->code.Value();
            std::string_view key = opened->conflict ? "workspace.decision_graph.edit_conflict" : "workspace.decision_graph.edit_refused";
            if (!opened->conflict && opened->recovery && code == Assets::AssetSourceErrors::Conflict.code.Value())
                key = "workspace.decision_graph.recovery_conflict";
            else if (code == AI::AIErrors::DecisionAssetDependencyCycle.code.Value())
                key = "workspace.decision_graph.subtree_cycle";
            else if (code == AI::AIErrors::DecisionAssetSubtreeMissing.code.Value())
                key = "workspace.decision_graph.subtree_missing";
            else if (opened->subtreeReview && opened->subtreeReview->documentRevision == opened->revision &&
                     code == Application::DecisionCookErrors::Stale.code.Value())
                key = "workspace.decision_graph.subtree_stale";
            label(text(key), Theme::Warn());
            if (!opened->validation && !opened->diagnostic->diagnostics.empty()) {
                const auto &finding = opened->diagnostic->diagnostics.front();
                label(std::format("{} · {}", finding.code.Value(), finding.message), Theme::Text());
            }
        }
        if (opened->validation) {
            const auto node = DrawDecisionProblems(*opened, opened->validation->Diagnostics(), m_context, outCommand);
            if (node.IsValid())
                m_decisionGraphCanvas.RequestFocus({opened->identity.instance, opened->revision}, node);
        }
        const auto &feedback = viewModel.contentBrowserOperationError;
        if (feedback == "workspace.decision_graph.diagnostic_key_missing" ||
            feedback == "workspace.decision_graph.diagnostic_key_unavailable" ||
            (opened->conflict && feedback == "workspace.decision_graph.invalid"))
            label(text(feedback), Theme::Warn());
        if (opened->conflict &&
            Ui::Button({.label = text("workspace.decision_graph.reload_external").c_str(), .font = m_context.theme.fonts.sans})) {
            outCommand = {.command = EditorWorkspaceViewCommand::ReloadExternalDecisionGraph,
                          .decisionGraphFence = DecisionGraphDocumentFence{opened->identity.instance, opened->revision}};
            return true;
        }
        if (DrawDecisionRecovery(*opened, m_context, outCommand))
            return true;

        const auto *propertyMachine = std::get_if<AI::StateMachineAssetDescriptor>(&opened->graph->body);
        const bool propertyOnly = opened->editable && propertyMachine && !CanEditStateMachineTopology(*propertyMachine);
        const auto drawTransitionOrder = [&](const AI::StateMachineAssetDescriptor &machine, const AI::StateMachineTransition &transition) {
            const auto order = ProjectStateMachineTransitionOrder(machine, transition);
            label(std::format("{} → {}", transition.source.Value(), transition.target.Value()), Theme::Text());
            if (!opened->editable || propertyOnly)
                label(std::format("{} {}", text("workspace.decision_graph.transition_priority"), order.priority), Theme::Text());
            label(std::format("{} {}", text("workspace.decision_graph.transition_order"), order.rank), Theme::Text());
            label(text("workspace.decision_graph.transition_precedence"), Theme::Muted());
        };
        const auto instance = opened->identity.instance;
        const auto selectedNode = [&]() -> AI::DecisionNodeId {
            using enum AI::DecisionPlanKind;
            if (opened->family == StateMachine && m_stateMachineSelection.instance == instance)
                return m_stateMachineSelection.state;
            if (opened->family == BehaviorTree && m_behaviorTreeSelection.instance == instance)
                return m_behaviorTreeSelection.node;
            if (opened->family == Utility && m_utilityGraphSelection.instance == instance)
                return m_utilityGraphSelection.option;
            return {};
        };
        if (!opened->editable || propertyOnly) {
            label(text(propertyOnly ? "workspace.decision_graph.provider_machine_properties_only" : "workspace.decision_graph.read_only"),
                  Theme::Muted());
            if (propertyOnly) {
                const auto history = [&](const char *key, bool enabled, DecisionHistoryAction action) {
                    if (Ui::Button({.label = text(key).c_str(), .enabled = enabled, .font = m_context.theme.fonts.sans}))
                        outCommand = MakeDecisionHistoryCommand(*opened, action);
                };
                history("workspace.decision_graph.edit_save", opened->dirty, DecisionHistoryAction::Save);
                history("workspace.decision_graph.edit_undo", opened->canUndo, DecisionHistoryAction::Undo);
                history("workspace.decision_graph.edit_redo", opened->canRedo, DecisionHistoryAction::Redo);
            }
            const auto canvas = m_decisionGraphCanvas.Draw(*opened, m_context, viewModel.decisionProviderMetadata);
            ApplyDecisionCanvasInteraction(*opened, {.node = canvas.node, .transition = canvas.transition}, outCommand);
            if (propertyOnly) {
                const auto selected = selectedNode();
                const auto state = std::ranges::find(propertyMachine->states, selected.IsValid() ? selected : propertyMachine->initial,
                                                     &AI::StateMachineState::id);
                if (state != propertyMachine->states.end())
                    DrawStateMachineTaskPropertyEditors(viewModel, *opened, *state, outCommand);
            } else {
                DrawReadOnlyDecisionProperties(*opened->graph, selectedNode(), m_context);
            }
            if (const auto *machine = propertyMachine) {
                const auto &selection = m_stateMachineSelection;
                const auto transition = std::ranges::find(machine->transitions, selection.transition, &AI::StateMachineTransition::id);
                if (transition != machine->transitions.end())
                    drawTransitionOrder(*machine, *transition);
            }
            return true;
        }
        const auto selected = selectedNode();
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
            DrawStateActionProperties(*state, m_context);
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
