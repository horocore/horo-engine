#include "Horo/AI/AISceneActivation.h"
#include "Horo/Assets/AssetId.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/game/GamePanel.h"

#include <algorithm>
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
            if (snapshot.decisionFailure)
                line(std::format("{} · {}", text("ai_problem"), snapshot.decisionFailure->code.Value()), Theme::Warn());
            if (snapshot.perceptionFailure)
                line(std::format("{} · {}", text("ai_inspection_perception"), snapshot.perceptionFailure->code.Value()), Theme::Warn());
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
    }  // namespace

    /** @brief Displays and compares a bounded history for the selected Play agent. */
    void GamePanel::DrawAgentInspection(const EditorWorkspaceViewModel &viewModel, const EditorGuiContext &context) {
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
        const auto candidates = std::span{history}.first(history.size() - 1);
        const auto baseline = std::ranges::find(candidates, comparisonTick_, &EditorPlayAgentInspection::tick);
        int selected = baseline == candidates.end() ? 0 : static_cast<int>(baseline - candidates.begin()) + 1;
        const auto availableHeight = ImGui::GetContentRegionAvail().y;
        if (availableHeight <= 1.0F)
            return;
        if (ImGui::BeginChild("##AgentInspection", {0, availableHeight * 0.45F}, ImGuiChildFlags_None, ImGuiWindowFlags_NoSavedSettings)) {
            if (!candidates.empty()) {
                std::vector<std::string> labels;
                labels.reserve(candidates.size() + 1);
                labels.push_back(text("ai_inspection_compare"));
                for (const auto &entry : candidates)
                    labels.push_back(std::format("{} {}", text("ai_problem_tick"), entry.tick));
                const auto revisionLabel = [&](const int index) {
                    return labels[static_cast<std::size_t>(index)].c_str();
                };
                static_cast<void>(Ui::ComboControl("##AgentBaseline", &selected, static_cast<int>(candidates.size()) + 1,
                                                   Ui::ComboItemSource{.label = revisionLabel}, fonts));
            }
            comparisonTick_ = selected != 0 ? candidates[static_cast<std::size_t>(selected - 1)].tick : 0;
            static_cast<void>(
                Ui::InputTextControl("##AgentKeyFilter", inspectionFilter_, 65, fonts, {.hint = text("ai_inspection_filter").c_str()}));
            Ui::WrappedText(ImGui::GetContentRegionAvail().x,
                            std::format("{} {} · {} {}", text("ai_problem_tick"), latest.decisionTick, text("ai_inspection_revision"),
                                        latest.blackboardRevision),
                            Theme::Muted(), fonts);
            const AI::AiAgentInspectionSnapshot *other = nullptr;
            if (selected != 0) {
                const auto &entry = candidates[static_cast<std::size_t>(selected - 1)].snapshot;
                if (entry && entry->blackboard == latest.blackboard)
                    other = entry.get();
                else
                    Ui::WrappedText(ImGui::GetContentRegionAvail().x, text("ai_inspection_incompatible"), Theme::Warn(), fonts);
            }
            DrawBlackboard(latest, other, inspectionFilter_, context);
            DrawObservations(history.back(), context);
        }
        ImGui::EndChild();
    }
}  // namespace Horo::Editor
