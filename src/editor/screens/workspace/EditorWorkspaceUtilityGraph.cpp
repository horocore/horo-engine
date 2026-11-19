#include "BlackboardScalarInput.h"
#include "DecisionTaskPalette.h"
#include "DecisionTaskPropertiesView.h"
#include "Horo/Application/DecisionAssetCookHost.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "UtilityGraphAuthoring.h"
#include "editor/screens/workspace/AuthoredAiSourceInventory.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <set>
#include <tuple>

namespace Horo::Editor {
    namespace {
        /** @brief Collects the exact typed root binding set required by all authored scores. */
        std::set<AI::BlackboardKeyId> UtilityScoreKeys(const AI::UtilityDecisionSource &source) {
            std::set<AI::BlackboardKeyId> keys;
            for (const auto &option : source.options)
                for (const auto &consideration : option.option.considerations)
                    keys.insert(consideration.key);
            return keys;
        }

        /** @brief Preserves surviving authored bindings and replaces only removed score bindings. */
        void RefreshUtilityScoreBindings(AI::UtilityDecisionSource &source, const SourceLocation &location) {
            const auto keys = UtilityScoreKeys(source);
            std::erase_if(source.requirements, [&](const auto &binding) {
                return !keys.contains(binding.key);
            });
            for (const auto key : keys) {
                if (std::ranges::find(source.requirements, key, &AI::DecisionBlackboardRequirement::key) == source.requirements.end())
                    source.requirements.push_back({.key = key, .kind = AI::BlackboardValueKind::Scalar, .source = location});
            }
        }

        /** @brief Builds a real registered Task option with explicit scoring and stable source identities. */
        Result<AI::UtilityDecisionOption> NewUtilityOption(const AI::DecisionGraphSource &graph, const AI::UtilityDecisionSource &source,
                                                           const UtilityGraphEditRequest &request, const AI::DecisionNodeContract &task) {
            using Option = Result<AI::UtilityDecisionOption>;
            std::set<AI::DecisionNodeId> nodes{source.root};
            std::set<AI::TaskId> actions{graph.executionTask};
            for (const auto &option : source.options) {
                nodes.insert(option.option.id);
                actions.insert(option.option.action);
            }
            const auto id = AuthoredAiSources::Issue(nodes);
            const auto action = AuthoredAiSources::Issue(actions);
            if (id.HasError() || action.HasError())
                return Option::Failure(MakeError(AI::AIErrors::DescriptorConflict));
            const auto version = task.descriptor.origin.version;
            return Option::Success({
                .option = {.id = id.Value(),
                           .action = action.Value(),
                           .priority = request.priority,
                           .compensation = request.compensation,
                           .considerations = {request.score->score}},
                .provider =
                    {.id = id.Value(), .type = task.descriptor.type, .descriptorVersion = {version, version}, .source = graph.location},
            });
        }

        /** @brief Detects a no-op without relying on presentation strings or rounded values. */
        auto ConsiderationValues(const AI::UtilityConsideration &score) {
            return std::tie(score.key, score.minimum, score.maximum, score.weight, score.curve, score.parameter, score.invert);
        }

        /** @brief Changes one exact score slot and keeps at least one consideration for the option. */
        Result<bool> EditUtilityScore(AI::UtilityOption &option, const UtilityGraphEditRequest &request) {
            using Edited = Result<bool>;
            using enum UtilityGraphEditOperation;
            const auto fail = [] {
                return Edited::Failure(MakeError(Application::DecisionCookErrors::Invalid));
            };
            auto &scores = option.considerations;
            if (request.operation == AddConsideration) {
                if (!request.score || scores.size() >= AI::UtilitySelector::MaximumConsiderations)
                    return fail();
                scores.push_back(request.score->score);
                return Edited::Success(true);
            }
            if (!request.slot || request.slot->index >= scores.size() || scores[request.slot->index].key != request.slot->priorKey)
                return fail();
            const auto found = scores.begin() + static_cast<std::ptrdiff_t>(request.slot->index);
            if (request.operation == DeleteConsideration) {
                if (scores.size() <= 1)
                    return fail();
                scores.erase(found);
                return Edited::Success(true);
            }
            if (request.operation != SetConsideration || !request.score)
                return fail();
            if (ConsiderationValues(*found) == ConsiderationValues(request.score->score))
                return Edited::Success(false);
            *found = request.score->score;
            return Edited::Success(true);
        }

        /** @brief Formats stable numeric identities through a bounded label buffer. */
        const char *NumericLabel(std::array<char, 24> &text, std::uint64_t value) {
            const auto result = std::to_chars(text.data(), text.data() + text.size() - 1, value);
            *result.ptr = char{};
            return text.data();
        }

        /** @brief Reads a finite number through the existing full-precision Blackboard parser. */
        std::optional<double> ReadNumber(std::string_view text) {
            const auto parsed = ReadBlackboardNumericDefault(AI::BlackboardValueKind::Scalar, text);
            return parsed ? std::optional{std::get<double>(*parsed)} : std::nullopt;
        }

        /** @brief Projects a saved schema's bounded scalar key choices and returns the explicit selected identity. */
        AI::BlackboardKeyId DrawUtilityScoreKey(const BlackboardSchemaDocumentSnapshot &schema, AI::BlackboardKeyId selected,
                                                const EditorGuiContext &context) {
            std::array<const BlackboardSchemaRow *, AI::MaximumBlackboardKeys> scalar{};
            std::size_t count{};
            int index{};
            for (const auto &row : schema.source->rows) {
                if (row.descriptor.kind != AI::BlackboardValueKind::Scalar ||
                    row.descriptor.cardinality != AI::BlackboardValueCardinality::Scalar)
                    continue;
                scalar[count++] = &row;
                if (row.descriptor.key == selected)
                    index = static_cast<int>(count);
            }
            const auto &empty = context.localization.Get("editor", "workspace.decision_graph.select_scoring_key");
            std::array<char, 24> buffer{};
            const Ui::ComboItemSource choices{.label = [&](int item) -> const char * {
                if (item == 0)
                    return empty.c_str();
                const auto &row = *scalar[static_cast<std::size_t>(item - 1)];
                return row.displayName ? row.displayName->c_str() : NumericLabel(buffer, row.descriptor.key.Value());
            }};
            static_cast<void>(Ui::ComboControl("##UtilityAuthoringKey", &index, static_cast<int>(count) + 1, choices, context.theme.fonts));
            return index == 0 ? AI::BlackboardKeyId{} : scalar[static_cast<std::size_t>(index - 1)]->descriptor.key;
        }
    }  // namespace

    /** @copydoc CanAuthorUtilityGraph */
    bool CanAuthorUtilityGraph(const AI::UtilityDecisionSource &source) {
        const auto keys = UtilityScoreKeys(source);
        return std::ranges::all_of(source.requirements, [&](const auto &binding) {
            return keys.contains(binding.key) && binding.kind == AI::BlackboardValueKind::Scalar &&
                   binding.cardinality == AI::BlackboardValueCardinality::Scalar && binding.access == AI::BlackboardKeyAccess::ReadOnly &&
                   binding.presence == AI::BlackboardKeyPresence::Optional && !binding.requireDefault;
        }) && std::ranges::none_of(source.options, [](const auto &option) {
            return std::ranges::any_of(option.properties, [](const auto &property) {
                return property.kind == AI::BehaviorTreePropertyKind::Opaque;
            });
        });
    }

    /** @copydoc EditUtilityGraphCandidate */
    Result<bool> EditUtilityGraphCandidate(AI::DecisionGraphSource &graph, const UtilityGraphEditRequest &request,
                                           const AI::DecisionNodeContract *admittedTask) {
        using Edited = Result<bool>;
        using enum UtilityGraphEditOperation;
        const auto fail = [] {
            return Edited::Failure(MakeError(Application::DecisionCookErrors::Invalid));
        };
        auto *source = std::get_if<AI::UtilityDecisionSource>(&graph.body);
        if (!source || !CanAuthorUtilityGraph(*source))
            return fail();
        const auto found = std::ranges::find_if(source->options, [&](const auto &option) {
            return option.option.id == request.option;
        });
        if (request.operation == AddOption) {
            if (!admittedTask || !request.score || source->options.size() >= AI::UtilitySelector::MaximumOptions)
                return fail();
            auto option = NewUtilityOption(graph, *source, request, *admittedTask);
            if (option.HasError())
                return Edited::Failure(option.ErrorValue());
            source->options.push_back(std::move(option).Value());
        } else if (found == source->options.end()) {
            return fail();
        } else if (request.operation == SetOptionPolicy) {
            if (found->option.priority == request.priority && found->option.compensation == request.compensation)
                return Edited::Success(false);
            found->option.priority = request.priority;
            found->option.compensation = request.compensation;
        } else if (request.operation == DeleteOption) {
            if (source->options.size() <= 1)
                return fail();
            source->options.erase(found);
        } else {
            const auto edited = EditUtilityScore(found->option, request);
            if (edited.HasError() || !edited.Value())
                return edited;
        }
        RefreshUtilityScoreBindings(*source, graph.location);
        return Edited::Success(true);
    }

    /** @copydoc EditorWorkspaceView::DrawUtilityGraph */
    void EditorWorkspaceView::DrawUtilityGraph(const EditorWorkspaceViewModel &viewModel, const DecisionGraphWorkspaceDocument &document,
                                               EditorWorkspaceViewCommandData &outCommand) const {
        const auto &source = std::get<AI::UtilityDecisionSource>(document.graph->body);
        if (source.options.empty())
            return;

        enum Field : std::size_t {
            Minimum,
            Maximum,
            Weight,
            Parameter,
            Priority,
            Compensation
        };

        const auto &fonts = m_context.theme.fonts;
        const auto text = [&](std::string_view key) -> const std::string & {
            return m_context.localization.Get("editor", key);
        };
        const auto button = [&](std::string_view key, bool enabled) {
            return Ui::Button({.label = text(key).c_str(), .enabled = enabled, .font = fonts.sans});
        };
        if (button("workspace.decision_graph.edit_save", document.dirty))
            outCommand = MakeDecisionHistoryCommand(document, DecisionHistoryAction::Save);
        if (button("workspace.decision_graph.edit_undo", document.canUndo))
            outCommand = MakeDecisionHistoryCommand(document, DecisionHistoryAction::Undo);
        if (button("workspace.decision_graph.edit_redo", document.canRedo))
            outCommand = MakeDecisionHistoryCommand(document, DecisionHistoryAction::Redo);

        auto &draft = m_utilityGraphSelection;
        if (draft.instance != document.identity.instance)
            draft = {.instance = document.identity.instance};
        const auto found = std::ranges::find_if(source.options, [&](const auto &option) {
            return option.option.id == draft.option;
        });
        auto optionIndex = found == source.options.end() ? 0 : static_cast<int>(found - source.options.begin());
        std::array<char, 24> labelBuffer{};
        const Ui::ComboItemSource options{.label = [&](int index) {
            return NumericLabel(labelBuffer, source.options[static_cast<std::size_t>(index)].option.id.Value());
        }};
        Ui::FieldLabel(text("workspace.decision_graph.edit_utility_option").c_str(), fonts);
        static_cast<void>(
            Ui::ComboControl("##UtilityAuthoringOption", &optionIndex, static_cast<int>(source.options.size()), options, fonts));
        const auto &selected = source.options[static_cast<std::size_t>(optionIndex)];
        DrawDecisionTaskProperties(selected.properties, m_context);
        const auto &option = selected.option;
        const auto resetScore = [&] {
            draft.score = draft.row ? option.considerations[*draft.row] : AI::UtilityConsideration{};
            draft.fields[Minimum] = std::format("{}", draft.score.minimum);
            draft.fields[Maximum] = std::format("{}", draft.score.maximum);
            draft.fields[Weight] = std::format("{}", draft.score.weight);
            draft.fields[Parameter] = std::format("{}", draft.score.parameter);
        };
        if (draft.option != option.id || draft.revision != document.revision) {
            draft.revision = document.revision;
            draft.option = option.id;
            draft.row = option.considerations.empty() ? std::nullopt : std::optional<std::size_t>{0};
            draft.fields[Priority] = std::format("{}", option.priority);
            draft.fields[Compensation] = std::format("{}", option.compensation);
            resetScore();
        }
        const auto numeric = [&](std::string_view key, const char *fieldId, Field field) {
            Ui::FieldLabel(text(key).c_str(), fonts);
            static_cast<void>(Ui::InputTextControl(fieldId, draft.fields[field], 64, fonts));
            return ReadNumber(draft.fields[field]);
        };
        const auto priority = numeric("workspace.decision_graph.edit_utility_priority", "##UtilityPriority", Priority);
        const auto compensation = numeric("workspace.decision_graph.edit_utility_compensation", "##UtilityCompensation", Compensation);
        const auto submit = [&](UtilityGraphEditOperation operation, std::optional<UtilityScoreEdit> score = {},
                                std::optional<DecisionTaskSelection> task = {}) {
            std::optional<UtilityConsiderationSlot> slot;
            if (draft.row)
                slot = UtilityConsiderationSlot{*draft.row, option.considerations[*draft.row].key};
            outCommand = {.command = EditorWorkspaceViewCommand::EditDecisionUtilityGraph,
                          .utilityGraphEdit = UtilityGraphEditRequest{document.identity.instance, document.revision, operation, option.id,
                                                                      task, score, slot, priority.value_or(option.priority),
                                                                      compensation.value_or(option.compensation)}};
        };
        using enum UtilityGraphEditOperation;
        if (button("workspace.decision_graph.edit_utility_apply_option", priority.has_value() && compensation.has_value()))
            submit(SetOptionPolicy);
        if (button("workspace.decision_graph.edit_utility_delete_option", source.options.size() > 1))
            submit(DeleteOption);

        const auto &newScore = text("workspace.decision_graph.edit_utility_new_score");
        const int newRow = static_cast<int>(option.considerations.size());
        auto rowIndex = draft.row ? static_cast<int>(*draft.row) : newRow;
        const Ui::ComboItemSource rows{.label = [&](int item) {
            return item == newRow ? newScore.c_str() : NumericLabel(labelBuffer, static_cast<std::uint64_t>(item) + 1);
        }};
        Ui::FieldLabel(text("workspace.decision_graph.edit_utility_score").c_str(), fonts);
        if (Ui::ComboControl("##UtilityAuthoringConsideration", &rowIndex, newRow + 1, rows, fonts)) {
            draft.row = rowIndex == newRow ? std::nullopt : std::optional{static_cast<std::size_t>(rowIndex)};
            resetScore();
        }
        if (button("workspace.decision_graph.edit_utility_delete_score", draft.row.has_value() && option.considerations.size() > 1))
            submit(DeleteConsideration);

        const auto *schema = FindSavedDecisionSchema(viewModel, *document.graph);
        if (!schema) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, text("workspace.decision_graph.open_scoring_schema"), Theme::Muted(), fonts);
            return;
        }
        Ui::FieldLabel(text("workspace.decision_graph.scoring_key").c_str(), fonts);
        draft.score.key = DrawUtilityScoreKey(*schema, draft.score.key, m_context);
        const auto minimum = numeric("workspace.decision_graph.score_minimum", "##UtilityMinimum", Minimum);
        const auto maximum = numeric("workspace.decision_graph.score_maximum", "##UtilityMaximum", Maximum);
        const auto weight = numeric("workspace.decision_graph.edit_utility_weight", "##UtilityWeight", Weight);
        const auto parameter = numeric("workspace.decision_graph.edit_utility_parameter", "##UtilityParameter", Parameter);
        const std::array curves{text("workspace.decision_graph.edit_utility_linear").c_str(),
                                text("workspace.decision_graph.edit_utility_power").c_str(),
                                text("workspace.decision_graph.edit_utility_logistic").c_str(),
                                text("workspace.decision_graph.edit_utility_step").c_str()};
        auto curve = static_cast<int>(draft.score.curve);
        Ui::FieldLabel(text("workspace.decision_graph.edit_utility_curve").c_str(), fonts);
        if (Ui::ComboControl("##UtilityAuthoringCurve", &curve, curves.data(), static_cast<int>(curves.size()), fonts))
            draft.score.curve = static_cast<AI::UtilityResponseCurve>(curve);
        static_cast<void>(Ui::CheckboxControl(text("workspace.decision_graph.edit_utility_invert").c_str(), &draft.score.invert, fonts));
        std::optional<UtilityScoreEdit> score;
        if (draft.score.key.IsValid() && minimum && maximum && weight && parameter) {
            auto value = draft.score;
            value.minimum = *minimum;
            value.maximum = *maximum;
            value.weight = *weight;
            value.parameter = *parameter;
            score = UtilityScoreEdit{value, *schema->sourceDigest};
        }
        if (draft.row) {
            if (button("workspace.decision_graph.edit_utility_apply_score", score.has_value()))
                submit(SetConsideration, score);
        } else if (button("workspace.decision_graph.edit_utility_add_score",
                          score && option.considerations.size() < AI::UtilitySelector::MaximumConsiderations)) {
            submit(AddConsideration, score);
        }

        const auto *task = DrawDecisionTaskPalette({viewModel.decisionTaskChoices, viewModel.availableBehaviorsRevision,
                                                    viewModel.decisionProviderMetadata.get(), schema->schema.get()},
                                                   draft.palette, m_context, outCommand);
        if (button("workspace.decision_graph.edit_utility_add_option",
                   task && score && priority && compensation && source.options.size() < AI::UtilitySelector::MaximumOptions)) {
            submit(AddOption, score,
                   DecisionTaskSelection{task->type, task->provider, task->version, viewModel.availableBehaviorsRevision});
        }
    }
}  // namespace Horo::Editor
