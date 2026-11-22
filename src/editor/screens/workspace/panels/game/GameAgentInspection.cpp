#include "Horo/AI/AISceneActivation.h"
#include "Horo/Assets/AssetId.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/game/GamePanel.h"

#include <algorithm>
#include <array>
#include <format>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        constexpr GuiLocalizationScope BlackboardText{"workspace.blackboard"};

        /** @brief Uses the existing localized Blackboard vocabulary for the runtime schema types. */
        std::string_view ValueType(const AI::BlackboardValueKind kind) {
            using enum AI::BlackboardValueKind;
            switch (kind) {
                case Boolean:
                    return "bool";
                case SignedInteger:
                    return "integer";
                case Scalar:
                    return "scalar";
                case EntityReference:
                    return "entity";
                case AssetReference:
                    return "asset";
                case WorldCoordinate:
                    return "coordinate";
                case Count:
                    return "type";
            }
            return "type";
        }

        /** @brief Displays only the value already disclosed by the runtime snapshot. */
        std::string ScalarText(const AI::BlackboardScalarValue &scalar, const ILocalizationService &localization) {
            return std::visit([&]<class T>(const T &value) -> std::string {
                if constexpr (std::is_same_v<T, bool>)
                    return BlackboardText.Text(localization, value ? "value_true" : "value_false");
                else if constexpr (std::is_same_v<T, AI::BlackboardStoredEntityReference>)
                    return std::format("{} / {}:{}", value.sceneIncarnation, value.slot, value.generation);
                else if constexpr (std::is_same_v<T, AI::BlackboardStoredAssetReference>)
                    return Assets::AssetId::FromBytes(value.canonicalBytes).ToString();
                else if constexpr (std::is_same_v<T, Math::WorldCoordinate64>) {
                    const auto position = value.Millimeters();
                    return std::format("{} / {} / {}", position[0], position[1], position[2]);
                } else
                    return std::format("{}", value);
            }, scalar);
        }

        /** @brief Formats the copied row without interpreting opaque or denied values. */
        std::string RowText(const AI::AiInspectionBlackboardRow &row, const ILocalizationService &localization) {
            const auto text = [&](const std::string_view key) -> const std::string & {
                return GamePanel::kTextScope.Text(localization, key);
            };
            using enum AI::AiInspectionValueStatus;
            if (row.status == Unset)
                return text("ai_inspection_unset");
            if (row.status == Unavailable)
                return text("ai_inspection_unavailable");
            if (row.status != Present || !row.value)
                return text("ai_inspection_redacted");
            if (const auto *scalar = std::get_if<AI::BlackboardScalarValue>(&*row.value))
                return ScalarText(*scalar, localization);
            if (const auto *collection = std::get_if<AI::BlackboardCollectionValue>(&*row.value)) {
                std::string result{"["};
                for (const auto &value : std::span{collection->elements}.first(collection->count)) {
                    if (result.size() > 1)
                        result += ", ";
                    result += ScalarText(value, localization);
                }
                result += ']';
                return result;
            }
            return text("ai_inspection_redacted");
        }

        /** @brief Keeps UI filtering and comparison confined to detached rows from the same publication. */
        void DrawBlackboard(const AI::AiAgentInspectionSnapshot &current, const AI::AiAgentInspectionSnapshot *baseline,
                            std::string_view filter, const EditorGuiContext &context) {
            const auto &localization = context.localization;
            for (const auto &row : current.keys) {
                const auto key = std::to_string(row.key.Value());
                const auto &type = BlackboardText.Text(localization, ValueType(row.kind));
                if (!filter.empty() && key.find(filter) == std::string::npos && type.find(filter) == std::string::npos)
                    continue;
                auto value = RowText(row, localization);
                if (baseline) {
                    const auto previous = std::ranges::find(baseline->keys, row.key, &AI::AiInspectionBlackboardRow::key);
                    if (previous != baseline->keys.end() && (previous->status != row.status || previous->value != row.value))
                        value = std::format("{} → {}", RowText(*previous, localization), value);
                }
                const auto &shape =
                    BlackboardText.Text(localization,
                                        row.cardinality == AI::BlackboardValueCardinality::Collection ? "collection" : "single");
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, std::format("{} · {} · {} · {}", key, type, shape, value), Theme::Muted(),
                                context.theme.fonts);
            }
        }

        /** @brief Uses localized labels for the directly owned task lifecycle states. */
        std::string_view TaskState(const AI::AiTaskState state) {
            using enum AI::AiTaskState;
            switch (state) {
                case Idle:
                    return "ai_inspection_task_idle";
                case Running:
                    return "ai_inspection_task_running";
                case Succeeded:
                    return "ai_inspection_task_succeeded";
                case Failed:
                    return "ai_inspection_task_failed";
                case Cancelled:
                    return "ai_inspection_task_cancelled";
            }
            return "ai_inspection_unavailable";
        }

        /** @brief Routes a localized control to the current live generation through the workspace. */
        void DebugButton(const char *key, const EditorPlayDecisionDebugState &state, const EditorPlayDecisionDebugAction action,
                         EditorWorkspaceViewCommandData &command, const EditorGuiContext &context,
                         const std::optional<EditorPlayDecisionBreakpoint> breakpoint = std::nullopt) {
            const auto &label = GamePanel::kTextScope.Text(context.localization, key);
            if (Ui::Button({.label = label.c_str(),
                            .variant = Ui::ButtonVariant::Secondary,
                            .font = context.theme.fonts.sans,
                            .componentSize = Ui::ComponentSize::Small}) &&
                command.command == EditorWorkspaceViewCommand::None) {
                command.command = EditorWorkspaceViewCommand::ControlPlayDecision;
                command.playDecisionDebugCommand = EditorPlayDecisionDebugCommand{state.agent, action, breakpoint};
            }
        }

        /** @brief Draws only the commands available for the approved current view. */
        void DrawDecisionDebug(const EditorPlayDecisionDebugState &state, EditorWorkspaceViewCommandData &command,
                               const EditorGuiContext &context) {
            using enum EditorPlayDecisionDebugAction;
            if (state.paused) {
                DebugButton("ai_debug_resume", state, Resume, command, context);
                if (!state.stepPending)
                    DebugButton("ai_debug_step", state, Step, command, context);
            } else
                DebugButton("ai_debug_pause", state, Pause, command, context);
            if (state.breakpoint)
                DebugButton("ai_debug_clear_breakpoint", state, ClearBreakpoint, command, context);
        }

        /** @brief Formats a normalized consideration or option total from the published trace. */
        std::string UtilityScoreText(const AI::DecisionStepRecord &record, const ILocalizationService &localization) {
            const auto text = [&](const std::string_view key) -> const std::string & {
                return GamePanel::kTextScope.Text(localization, key);
            };
            const auto label = record.key.IsValid() ? std::format("{} {}", text("ai_inspection_utility_consideration"), record.key.Value())
                                                    : text("ai_inspection_utility_total");
            const auto score = record.utilityScore ? std::format("{:.3f}", *record.utilityScore) : text("ai_inspection_unavailable");
            return std::format("{} {} · {} {} · {} {}", text("ai_problem_graph"), record.occurrence.graph.Value(), text("ai_problem_node"),
                               record.node.Value(), label, score);
        }

        /** @brief Draws redacted node, transition and score evidence from one bounded observation. */
        void DrawDecisionTrace(const AI::DecisionStepTrace &trace, const EditorGuiContext &context,
                               const EditorPlayDecisionDebugState *debug = nullptr, EditorWorkspaceViewCommandData *command = nullptr) {
            const auto text = [&](const std::string_view key) -> const std::string & {
                return GamePanel::kTextScope.Text(context.localization, key);
            };
            const auto line = [&](const std::string &value, const ImVec4 color) {
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, value, color, context.theme.fonts);
            };
            line(std::format("{} · {} {}", text("ai_inspection_trace"), text("ai_problem_tick"), trace.Header().tick), Theme::Muted());
            const auto records = trace.Records();
            for (std::size_t index = 0; index < records.size(); ++index) {
                const auto &record = records[index];
                using enum AI::DecisionStepRecordKind;
                if (record.kind == UtilityScore) {
                    line(UtilityScoreText(record, context.localization), Theme::Muted());
                } else if (record.kind == StateMachineTransition) {
                    line(std::format("{} {} · {} {} · {}", text("ai_problem_graph"), record.occurrence.graph.Value(),
                                     text("ai_inspection_transition"), record.node.Value(), record.transitionCount),
                         Theme::Muted());
                } else if (record.kind == StateMachineStep ||
                           (record.kind == BehaviorNode && (debug || record.state == AI::AiTaskState::Running))) {
                    const EditorPlayDecisionBreakpoint target{record.occurrence.graph, record.node, record.occurrence.occurrence};
                    const bool armed = debug && debug->breakpoint == target;
                    line(std::format("{} {} · {} {} · {}", text("ai_problem_graph"), record.occurrence.graph.Value(),
                                     text("ai_problem_node"), record.node.Value(), text(TaskState(record.state))),
                         armed ? Theme::Warn() : Theme::Muted());
                    if (debug && command && !armed && target.node.IsValid()) {
                        ImGui::PushID(static_cast<int>(index));
                        DebugButton("ai_debug_set_breakpoint", *debug, EditorPlayDecisionDebugAction::SetBreakpoint, *command, context,
                                    target);
                        ImGui::PopID();
                    }
                }
            }
            if (trace.DroppedRecords() != 0)
                line(std::format("{} {}", text("ai_inspection_trace_overflow"), trace.DroppedRecords()), Theme::Warn());
        }

        /** @brief Shows the active Scene budgets without writing project or user settings. */
        void DrawAiPolicy(const EditorPlayAiPolicy &policy, const EditorGuiContext &context) {
            const auto text = [&](const std::string_view key) -> const std::string & {
                return GamePanel::kTextScope.Text(context.localization, key);
            };
            const auto title = std::format("{}###PlayAiPolicy", text("ai_policy_title"));
            if (!ImGui::CollapsingHeader(title.c_str()))
                return;
            const auto line = [&](const std::string &value) {
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, value, Theme::Muted(), context.theme.fonts);
            };
            const auto &scheduler = policy.scheduler;
            const auto mode = scheduler.mode == AI::AiSchedulingMode::BestEffort ? "ai_policy_best_effort" : "ai_policy_deterministic";
            line(std::format("{} · {} {} · {} {}", text(mode), text("ai_policy_evaluations"), scheduler.evaluationsPerTick,
                             text("ai_policy_decision_work"), scheduler.workUnitsPerTick));
            if (const auto &profile = scheduler.simulationProfile)
                line(std::format("{} {}/{}/{} · {} {}/{}", text("ai_policy_distance_bands"), profile->fullDistanceMillimeters,
                                 profile->reducedDistanceMillimeters, profile->suspensionDistanceMillimeters, text("ai_policy_cadence"),
                                 profile->reducedIntervalTicks, profile->demotionDelayTicks));
            const auto sceneWork = policy.scenePerceptionWork ? std::to_string(*policy.scenePerceptionWork) : "—";
            line(std::format("{} {}/{} · {} {}", text("ai_policy_perception_work"), sceneWork, policy.perceptionLimit,
                             text("ai_policy_query_work"), policy.queryLimit));
            if (const auto &cache = policy.queryCache)
                line(std::format("{} {}/{}/{}", text("ai_policy_query_cache"), cache->maximumEntries, cache->maximumBytes,
                                 cache->lifetimeTicks));
        }

        /** @brief Displays the actual provider contract and availability copied at the selected tick. */
        void DrawSenseProvider(const AI::AiInspectionSenseProvider &provider, const EditorGuiContext &context) {
            using enum AI::PerceptionCapability;
            constexpr std::array labels{
                std::pair{SpatialCandidateQuery, "ai_inspection_capability_spatial"},
                std::pair{PhysicsLineOfSight, "ai_inspection_capability_sight"},
                std::pair{AcousticOcclusion, "ai_inspection_capability_acoustic"},
                std::pair{PhysicsContact, "ai_inspection_capability_contact"},
                std::pair{TeamRelay, "ai_inspection_capability_relay"},
                std::pair{ScriptExecution, "ai_inspection_capability_script"},
            };
            static_assert(labels.size() == static_cast<std::size_t>(Count));
            const auto text = [&](const std::string_view key) -> const std::string & {
                return GamePanel::kTextScope.Text(context.localization, key);
            };
            auto line = std::format("{} {} · {} {} · {}", text("ai_inspection_perception"), provider.sense.Value(),
                                    text("ai_inspection_provider"), provider.origin.provider.Value(),
                                    text(provider.available ? "ai_inspection_provider_available" : "ai_inspection_provider_unavailable"));
            for (const auto &[capability, label] : labels) {
                if (provider.requiredCapabilities.Contains(AI::PerceptionCapabilitySet::Of(capability)))
                    line.append(" · ").append(text(label));
            }
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, line, provider.available ? Theme::Muted() : Theme::Warn(),
                            context.theme.fonts);
        }

        /** @brief Shows copied lifecycle and memory metadata without reading live runtime state. */
        void DrawObservations(const EditorPlayAgentInspection &entry, const EditorGuiContext &context) {
            const auto &snapshot = *entry.snapshot;
            const auto text = [&](const std::string_view key) -> const std::string & {
                return GamePanel::kTextScope.Text(context.localization, key);
            };
            const auto line = [&](std::string value, const ImVec4 color) {
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, value, color, context.theme.fonts);
            };
            if (const auto &task = snapshot.task) {
                line(std::format("{} {} · {}", text("ai_inspection_task"), task->definition.Value(), text(TaskState(task->state))),
                     Theme::Muted());
                if (task->failure)
                    line(std::format("{} · {}", text("ai_inspection_task_failed"), task->failure->code.Value()), Theme::Warn());
            }
            for (const auto &task : snapshot.decisionTasks)
                line(std::format("{} {} · {} {} · {} {} · {}", text("ai_inspection_task"), task.task.Value(), text("ai_problem_graph"),
                                 task.occurrence.graph.Value(), text("ai_problem_node"), task.node.Value(),
                                 text("ai_inspection_task_running")),
                     Theme::Muted());
            if (snapshot.omittedDecisionTasks != 0)
                line(std::format("{} {}", text("ai_inspection_omitted_tasks"), snapshot.omittedDecisionTasks), Theme::Warn());
            if (snapshot.decisionFailure)
                line(std::format("{} · {}", text("ai_problem"), snapshot.decisionFailure->code.Value()), Theme::Warn());
            if (snapshot.perceptionFailure)
                line(std::format("{} · {}", text("ai_inspection_perception"), snapshot.perceptionFailure->code.Value()), Theme::Warn());
            if (snapshot.perceptionProjectionDrops != 0)
                line(std::format("{} {}", text("ai_inspection_perception_drops"), snapshot.perceptionProjectionDrops), Theme::Warn());
            for (const auto &provider : snapshot.senseProviders)
                DrawSenseProvider(provider, context);
            if (const auto &navigation = entry.navigation)
                line(std::format("{} · {} {} · {} {} · {}", text("ai_inspection_navigation"), text("ai_inspection_profile"),
                                 navigation->profile, text("ai_inspection_navigation_filter"), navigation->filter,
                                 text(navigation->enabled ? "ai_inspection_navigation_enabled" : "ai_inspection_navigation_disabled")),
                     Theme::Muted());
            for (const auto &memory : snapshot.stimuli)
                line(std::format("{} {} · {} {} · {} {:.2f}", text("ai_inspection_perception"), memory.key.sense.Value(),
                                 text("ai_inspection_stimulus"), memory.key.stimulus.Value(), text("ai_inspection_age"), memory.ageSeconds),
                     Theme::Muted());
        }

        /** @brief Chooses an earlier retained snapshot and updates the comparison tick. */
        const EditorPlayAgentInspection *ChooseComparison(const std::span<const EditorPlayAgentInspection> candidates,
                                                          std::uint64_t &comparisonTick, const EditorGuiContext &context) {
            if (candidates.empty()) {
                comparisonTick = 0;
                return nullptr;
            }
            const auto text = [&](std::string_view key) -> const std::string & {
                return GamePanel::kTextScope.Text(context.localization, key);
            };
            const auto baseline = std::ranges::find(candidates, comparisonTick, &EditorPlayAgentInspection::tick);
            int selected = baseline == candidates.end() ? 0 : static_cast<int>(baseline - candidates.begin()) + 1;
            std::vector<std::string> labels;
            labels.reserve(candidates.size() + 1);
            labels.push_back(text("ai_inspection_compare"));
            for (const auto &entry : candidates)
                labels.push_back(std::format("{} {}", text("ai_problem_tick"), entry.tick));
            const auto revisionLabel = [&](const int index) {
                return labels[static_cast<std::size_t>(index)].c_str();
            };
            static_cast<void>(Ui::ComboControl("##AgentBaseline", &selected, static_cast<int>(labels.size()),
                                               Ui::ComboItemSource{.label = revisionLabel}, context.theme.fonts));
            const auto *entry = selected != 0 ? &candidates[static_cast<std::size_t>(selected - 1)] : nullptr;
            comparisonTick = entry ? entry->tick : 0;
            return entry;
        }
    }  // namespace

    /** @brief Displays and compares a bounded history for the selected Play agent. */
    void GamePanel::DrawAgentInspection(const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                                        const EditorGuiContext &context) {
        const auto &history = viewModel.playAgentInspection;
        if (history.empty() || !history.back().snapshot) {
            inspectedAgent_ = {};
            comparisonTick_ = 0;
            inspectionFilter_.clear();
            inspectionOpen_ = false;
            return;
        }
        if (inspectedAgent_ != history.back().agent) {
            inspectedAgent_ = history.back().agent;
            comparisonTick_ = 0;
            inspectionFilter_.clear();
        }
        const auto text = [&](const std::string_view key) -> const std::string & {
            return kTextScope.Text(context.localization, key);
        };
        const auto &fonts = context.theme.fonts;
        if (Ui::Button({.label = text("ai_inspection_title").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .font = fonts.sans,
                        .componentSize = Ui::ComponentSize::Small}))
            inspectionOpen_ = !inspectionOpen_;
        if (!inspectionOpen_)
            return;

        const auto &latest = *history.back().snapshot;
        const auto availableHeight = ImGui::GetContentRegionAvail().y;
        if (availableHeight <= 1.0F)
            return;
        if (ImGui::BeginChild("##AgentInspection", {0, availableHeight * 0.45F}, ImGuiChildFlags_None, ImGuiWindowFlags_NoSavedSettings)) {
            command.visiblePlayDecisionInspector = history.back().agent;
            const auto *debug = viewModel.playDecisionDebug && viewModel.playDecisionDebug->agent == history.back().agent
                                    ? &*viewModel.playDecisionDebug
                                    : nullptr;
            if (debug)
                DrawDecisionDebug(*debug, command, context);
            if (const auto &policy = history.back().policy)
                DrawAiPolicy(*policy, context);
            const auto *comparison = ChooseComparison(std::span{history}.first(history.size() - 1), comparisonTick_, context);
            static_cast<void>(
                Ui::InputTextControl("##AgentKeyFilter", inspectionFilter_, 65, fonts, {.hint = text("ai_inspection_filter").c_str()}));
            Ui::WrappedText(ImGui::GetContentRegionAvail().x,
                            std::format("{} {} · {} {}", text("ai_problem_tick"), latest.decisionTick, text("ai_inspection_revision"),
                                        latest.blackboardRevision),
                            Theme::Muted(), fonts);
            const AI::AiAgentInspectionSnapshot *other = nullptr;
            if (comparison) {
                const auto &entry = comparison->snapshot;
                if (entry && entry->blackboard == latest.blackboard)
                    other = entry.get();
                else
                    Ui::WrappedText(ImGui::GetContentRegionAvail().x, text("ai_inspection_incompatible"), Theme::Warn(), fonts);
            }
            DrawBlackboard(latest, other, inspectionFilter_, context);
            DrawObservations(history.back(), context);
            const auto &latestTrace = debug ? debug->trace : history.back().decisionTrace;
            if (latestTrace)
                DrawDecisionTrace(*latestTrace, context, debug, &command);
            if (comparison && comparison->decisionTrace && comparison->decisionTrace != latestTrace)
                DrawDecisionTrace(*comparison->decisionTrace, context);
        }
        ImGui::EndChild();
    }
}  // namespace Horo::Editor
