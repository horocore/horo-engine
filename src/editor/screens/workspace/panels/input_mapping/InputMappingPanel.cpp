#include "editor/screens/workspace/panels/input_mapping/InputMappingPanel.h"

#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/WorkspacePathText.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>

namespace Horo::Editor {
    namespace {
        const std::vector<Input::InputBinding> &BindingsFor(const Input::ActionDescriptor &action,
                                                            const Input::InputBindingProfile &profile) {
            const auto found = std::ranges::find(profile.overrides, action.id, &Input::BindingOverride::action);
            return found == profile.overrides.end() ? action.defaultBindings : found->bindings;
        }

        const char *ValueTypeLocalizationKey(const Input::ActionValueType type) noexcept {
            using enum Input::ActionValueType;

            switch (type) {
                case Digital:
                    return "workspace.input_mapping.type.digital";
                case Axis1D:
                    return "workspace.input_mapping.type.axis_1d";
                case Axis2D:
                    return "workspace.input_mapping.type.axis_2d";
            }
            return "workspace.input_mapping.type.unknown";
        }

        /** @brief Shares assignment presentation while retaining externally assigned player IDs beyond the common four. */
        bool DrawPlayerAssignment(const char *id, std::optional<Input::PlayerId> &player, const EditorGuiContext &context) {
            const auto &unassigned = InputMappingPanel::kTextScope.Text(context.localization, "unassigned");
            std::array<char, 12> external{};
            std::array<const char *, 6> items{unassigned.c_str(), "1", "2", "3", "4", external.data()};
            int selected = 0;
            std::size_t count = 5;
            if (player) {
                selected = *player < 4 ? static_cast<int>(*player) + 1 : 5;
                if (selected == 5) {
                    const auto converted =
                        std::to_chars(external.data(), external.data() + external.size() - 1, static_cast<unsigned int>(*player) + 1);
                    *converted.ptr = '\0';
                    count = items.size();
                }
            }
            if (!Ui::DrawComboPropRow(InputMappingPanel::kTextScope.Text(context.localization, "player").c_str(), id, selected,
                                      std::span(items).first(count), context.theme.fonts))
                return false;
            if (selected < 5)
                player = selected == 0 ? std::nullopt : std::optional{static_cast<Input::PlayerId>(selected - 1)};
            return true;
        }

    }  // namespace

    void InputMappingPanel::OnAttach(PanelContext &context) {
        router_ = context.inputRouter;
    }

    void InputMappingPanel::OnDetach() {
        bindingCaptureContext_.Reset();
        listeningAction_.reset();
        bindingCapture_.reset();
        deadzoneDraft_.reset();
        router_ = nullptr;
    }

    void InputMappingPanel::OnPresentationHidden(EditorWorkspaceViewCommandData &) {
        bindingCaptureContext_.Reset();
        bindingCapture_.reset();
        listeningAction_.reset();
        deadzoneDraft_.reset();
        statusMessage_.clear();
    }

    void InputMappingPanel::DrawIcon(ImDrawList *drawList, const ImVec2 &position, const ImVec2 &size, const ImU32 color) {
        const ImVec2 center{position.x + size.x * 0.5F, position.y + size.y * 0.5F};
        drawList->AddCircle(center, 6.0F, color, 16, 1.5F);
        drawList->AddLine({center.x - 9.0F, center.y}, {center.x + 9.0F, center.y}, color, 1.5F);
        drawList->AddCircleFilled({center.x - 4.0F, center.y}, 2.0F, color);
        drawList->AddCircleFilled({center.x + 5.0F, center.y}, 2.0F, color);
    }

    void InputMappingPanel::DrawPanel(const ImVec2 &position, const ImVec2 &size, const EditorWorkspaceViewModel &viewModel,
                                      EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        if (projectRootText_ != viewModel.projectRoot) {
            bindingCaptureContext_.Reset();
            listeningAction_.reset();
            bindingCapture_.reset();
            deadzoneDraft_.reset();
            statusMessage_.clear();
            projectRoot_ = WorkspaceNativePath(viewModel.projectRoot);
            projectRootText_ = viewModel.projectRoot;
        }
        const std::array labels{
            InputMappingPanel::kTextScope.Text(context.localization, "actions").c_str(),
            InputMappingPanel::kTextScope.Text(context.localization, "devices").c_str(),
            InputMappingPanel::kTextScope.Text(context.localization, "profiles").c_str(),
        };
        auto selected = static_cast<int>(page_);
        // Keep the event that completes capture from activating another panel control.
        const bool capturing = bindingCapture_.has_value();
        ImGui::BeginDisabled(capturing);
        ImGui::SetNextItemWidth(std::max(1.0F, std::min(size.x, ImGui::GetContentRegionAvail().x)));
        if (Ui::ComboControl("##InputMappingPage", &selected, labels.data(), static_cast<int>(labels.size()), context.theme.fonts,
                             {.componentSize = Ui::ComponentSize::Medium}))
            page_ = static_cast<Page>(selected);
        const float usedHeight = ImGui::GetCursorScreenPos().y - position.y;
        const ImVec2 contentSize{std::max(1.0F, std::min(size.x, ImGui::GetContentRegionAvail().x)),
                                 std::max(1.0F, std::min(size.y - usedHeight, ImGui::GetContentRegionAvail().y))};
        ImGui::BeginChild("##InputMappingContent", contentSize, false, ImGuiWindowFlags_NoSavedSettings);
        PollBindingCapture(context);
        if (page_ == Page::Actions)
            DrawActions(context);
        else if (page_ == Page::Devices)
            DrawDevices(context);
        else
            DrawProfiles(context);
        ImGui::EndDisabled();
        if (!statusMessage_.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", statusMessage_.c_str());
        }
        ImGui::EndChild();
    }

    void InputMappingPanel::DrawActions(const EditorGuiContext &context) {
        if (router_ == nullptr)
            return;
        const auto actions = router_->Actions();
        if (actions.empty()) {
            ImGui::TextUnformatted(InputMappingPanel::kTextScope.Text(context.localization, "no_actions").c_str());
            return;
        }
        selectedAction_ = std::min(selectedAction_, actions.size() - 1);
        DrawActionValidation(actions);
        DrawActionTable(actions, context);
        DrawSelectedAction(actions[selectedAction_], context);
    }

    void InputMappingPanel::DrawActionValidation(const std::span<const Input::ActionDescriptor> actions) const {
        const Input::BindingValidationReport validation = Input::ValidateBindingProfile(actions, router_->Profile());
        for (const Input::BindingDiagnostic &diagnostic : validation.diagnostics) {
            ImGui::PushStyleColor(ImGuiCol_Text, diagnostic.blocking ? Theme::Err() : Theme::Warn());
            ImGui::TextWrapped("%s", diagnostic.message.c_str());
            ImGui::PopStyleColor();
        }
    }

    void InputMappingPanel::DrawActionTable(const std::span<const Input::ActionDescriptor> actions, const EditorGuiContext &context) {
        if (ImGui::BeginTable("##ActionMap", 4,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(InputMappingPanel::kTextScope.Text(context.localization, "action").c_str());
            ImGui::TableSetupColumn(InputMappingPanel::kTextScope.Text(context.localization, "type").c_str());
            ImGui::TableSetupColumn(InputMappingPanel::kTextScope.Text(context.localization, "bindings").c_str());
            ImGui::TableSetupColumn(InputMappingPanel::kTextScope.Text(context.localization, "status").c_str());
            ImGui::TableHeadersRow();
            for (std::size_t index = 0; index < actions.size(); ++index) {
                const Input::ActionDescriptor &action = actions[index];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(action.id.Value().c_str(), selectedAction_ == index, ImGuiSelectableFlags_SpanAllColumns,
                                      {std::max(1.0F, ImGui::GetContentRegionAvail().x), 0.0F})) {
                    if (selectedAction_ != index)
                        deadzoneDraft_.reset();
                    selectedAction_ = index;
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(context.localization.Get("editor", ValueTypeLocalizationKey(action.valueType)).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%zu", BindingsFor(action, router_->Profile()).size());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(action.required ? InputMappingPanel::kTextScope.Text(context.localization, "required").c_str()
                                                       : InputMappingPanel::kTextScope.Text(context.localization, "optional").c_str());
            }
            ImGui::EndTable();
        }
    }

    void InputMappingPanel::DrawSelectedAction(const Input::ActionDescriptor &selectedAction, const EditorGuiContext &context) {
        if (displayedAction_ != selectedAction.id) {
            deadzoneDraft_.reset();
            displayedAction_ = selectedAction.id;
        }
        ImGui::Separator();
        ImGui::TextWrapped("%s", selectedAction.id.Value().c_str());
        const auto &bindings = BindingsFor(selectedAction, router_->Profile());
        for (std::size_t bindingIndex = 0; bindingIndex < bindings.size(); ++bindingIndex) {
            const Input::InputBinding &binding = bindings[bindingIndex];
            const auto label = Ui::ShortcutLabel(binding);
            ImGui::BulletText("%s", label.c_str());
            ImGui::PushID(static_cast<int>(bindingIndex));
            if (selectedAction.valueType != Input::ActionValueType::Digital) {
                int component = binding.component;
                int direction = binding.scale < 0.0F ? 1 : 0;
                const std::array<const char *, 2> components{"X", "Y"};
                const std::array directions{kTextScope.Text(context.localization, "positive").c_str(),
                                            kTextScope.Text(context.localization, "negative").c_str()};
                const bool changedComponent = selectedAction.valueType == Input::ActionValueType::Axis2D &&
                                              Ui::DrawComboPropRow(kTextScope.Text(context.localization, "component").c_str(),
                                                                   "##Component", component, components, context.theme.fonts);
                const bool changedDirection = Ui::DrawComboPropRow(kTextScope.Text(context.localization, "direction").c_str(),
                                                                   "##Direction", direction, directions, context.theme.fonts);
                if (changedComponent || changedDirection) {
                    Input::BindingOverride change{selectedAction.id, {bindings.begin(), bindings.end()}};
                    auto &updated = change.bindings[bindingIndex];
                    updated.component = static_cast<std::uint8_t>(component);
                    updated.scale = direction == 1 ? -std::abs(binding.scale) : std::abs(binding.scale);
                    const auto applied = CommitBindingChange(std::move(change));
                    statusMessage_ =
                        applied.HasError() ? applied.ErrorValue().message : kTextScope.Text(context.localization, "binding_updated");
                    ImGui::PopID();
                    return;
                }
            }
            if (Ui::Button({.label = kTextScope.Text(context.localization, "rebind").c_str(),
                            .variant = Ui::ButtonVariant::Secondary,
                            .font = context.theme.fonts.sans}))
                StartBindingCapture(selectedAction.id, bindingIndex, context);
            if (Ui::Button({.label = kTextScope.Text(context.localization, "remove_binding").c_str(),
                            .variant = Ui::ButtonVariant::Secondary,
                            .font = context.theme.fonts.sans})) {
                Input::BindingOverride change{selectedAction.id, {bindings.begin(), bindings.end()}};
                change.bindings.erase(change.bindings.begin() + static_cast<std::ptrdiff_t>(bindingIndex));
                const auto applied = CommitBindingChange(std::move(change));
                statusMessage_ =
                    applied.HasError() ? applied.ErrorValue().message : kTextScope.Text(context.localization, "binding_updated");
                deadzoneDraft_.reset();
                ImGui::PopID();
                return;
            }
            if (binding.kind == Input::BindingControlKind::GamepadAxis || binding.kind == Input::BindingControlKind::RawGamepadAxis) {
                float deadzone = deadzoneDraft_ && deadzoneDraft_->first == bindingIndex ? deadzoneDraft_->second : binding.deadzone;
                const auto edit = Ui::DrawFloatPropRow(kTextScope.Text(context.localization, "deadzone").c_str(), "##Deadzone", deadzone,
                                                       context.theme.fonts, {.speed = 0.01F, .minimum = 0.0F, .maximum = 0.95F});
                if (edit.changed) {
                    deadzoneDraft_ = std::pair{bindingIndex, deadzone};
                }
                if (edit.committed && deadzoneDraft_ && deadzoneDraft_->first == bindingIndex) {
                    Input::BindingOverride change{selectedAction.id, {bindings.begin(), bindings.end()}};
                    change.bindings[bindingIndex].deadzone = deadzoneDraft_->second;
                    const auto applied = CommitBindingChange(std::move(change));
                    statusMessage_ = applied.HasError() ? applied.ErrorValue().message : std::string{};
                    deadzoneDraft_.reset();
                    ImGui::PopID();
                    return;
                }
            }
            ImGui::PopID();
        }
        const std::string &rebind = InputMappingPanel::kTextScope.Text(context.localization, "add_binding");
        if (Ui::Button({.label = rebind.c_str(), .variant = Ui::ButtonVariant::Secondary, .font = context.theme.fonts.sans})) {
            StartBindingCapture(selectedAction.id, std::nullopt, context);
        }
    }

    void InputMappingPanel::StartBindingCapture(const Input::ActionId &action, const std::optional<std::size_t> bindingIndex,
                                                const EditorGuiContext &context) {
        listeningAction_ = action;
        listeningBinding_ = bindingIndex;
        bindingCaptureContext_.Reset();
        bindingCaptureContext_ =
            router_->PushContext(Input::InputContextId{"editor.input_binding_capture"}, Input::InputContextKind::FocusedGuiWidget);
        auto capture = Input::InputBindingCapture::Create(*router_, bindingCaptureContext_, *listeningAction_);
        if (capture.HasError()) {
            statusMessage_ = capture.ErrorValue().message;
            listeningAction_.reset();
            bindingCaptureContext_.Reset();
        } else {
            bindingCapture_.emplace(std::move(capture).Value());
            statusMessage_ = InputMappingPanel::kTextScope.Text(context.localization, "press_key");
        }
    }

    void InputMappingPanel::DrawDevices(const EditorGuiContext &context) {
        if (router_ == nullptr)
            return;
        {
            Ui::ScopedCard card("keyboard-mouse", {0.0F, 0.0F}, Theme::Layout::CardPad, Theme::Layout::CardPad, Theme::Bg2(), true);
            ImGui::TextWrapped("%s", kTextScope.Text(context.localization, "keyboard_mouse").c_str());
            auto assignment = router_->PlayerForKeyboardMouse();
            if (DrawPlayerAssignment("##KeyboardPlayer", assignment, context))
                router_->AssignKeyboardMouse(assignment);
        }
        const auto &gamepads = router_->Snapshot().gamepads;
        if (gamepads.empty()) {
            ImGui::TextUnformatted(InputMappingPanel::kTextScope.Text(context.localization, "no_devices").c_str());
            return;
        }
        for (const Input::GamepadState &gamepad : gamepads) {
            // Decimal uint32 slot, separator, uint64 incarnation and terminator fit in 32 bytes.
            // A reused slot cannot inherit an old device's open assignment popup.
            std::array<char, 32> cardId{};
            auto *suffix = std::to_chars(cardId.data(), cardId.data() + cardId.size() - 1, gamepad.id.slot).ptr;
            *suffix++ = '-';
            suffix = std::to_chars(suffix, cardId.data() + cardId.size() - 1, gamepad.id.sessionGeneration).ptr;
            *suffix = '\0';
            Ui::ScopedCard card(cardId.data(), {0.0F, 0.0F}, Theme::Layout::CardPad, Theme::Layout::CardPad, Theme::Bg2(), true);
            ImGui::TextWrapped("%s", gamepad.name.c_str());
            ImGui::TextWrapped("%s %u / %s %llu", InputMappingPanel::kTextScope.Text(context.localization, "slot").c_str(), gamepad.id.slot,
                               InputMappingPanel::kTextScope.Text(context.localization, "generation").c_str(),
                               static_cast<unsigned long long>(gamepad.id.sessionGeneration));
            auto assignment = router_->PlayerForGamepad(gamepad.id);
            if (DrawPlayerAssignment("##GamepadPlayer", assignment, context)) {
                if (!assignment)
                    router_->UnassignGamepad(gamepad.id);
                else
                    static_cast<void>(router_->AssignGamepad(*assignment, gamepad.id));
            }
        }
    }

    void InputMappingPanel::DrawProfiles(const EditorGuiContext &context) {
        if (router_ == nullptr)
            return;
        ImGui::TextWrapped("%s: %s", InputMappingPanel::kTextScope.Text(context.localization, "active_profile").c_str(),
                           router_->Profile().profileId.c_str());
        ImGui::TextWrapped("%s", kTextScope.Text(context.localization, "automatic_project_profile").c_str());
        const std::string &reload = InputMappingPanel::kTextScope.Text(context.localization, "reload_profile");
        if (Ui::Button({.label = reload.c_str(), .variant = Ui::ButtonVariant::Secondary, .font = context.theme.fonts.sans})) {
            const auto paths = ProfilePaths();
            if (paths.HasError())
                statusMessage_ = paths.ErrorValue().message;
            else if (const Result<void> applied = router_->ReloadProfile(paths.Value()); applied.HasError())
                statusMessage_ = applied.ErrorValue().message;
            else
                statusMessage_ = InputMappingPanel::kTextScope.Text(context.localization, "profile_loaded");
        }
    }

    Result<Input::BindingProfilePaths> InputMappingPanel::ProfilePaths() const {
        const auto home = ResolveEditorSettingsHomeDirectory();
        if (projectRoot_.empty())
            return Input::ResolveBindingProfilePaths({}, home);
        const auto metadata = Application::LoadProjectMetadata(projectRoot_);
        if (metadata.HasError())
            return Result<Input::BindingProfilePaths>::Failure(metadata.ErrorValue());
        return Input::ResolveBindingProfilePaths(projectRoot_, home, metadata.Value().projectId);
    }

    Result<void> InputMappingPanel::CommitBindingChange(Input::BindingOverride change) const {
        const auto scope = projectRoot_.empty() ? Input::BindingProfileScope::EditorUser : Input::BindingProfileScope::ProjectUser;
        const auto paths = ProfilePaths();
        if (paths.HasError())
            return Result<void>::Failure(paths.ErrorValue());
        return Input::CommitBindingProfileChange(*router_, paths.Value(), scope, std::move(change));
    }

    void InputMappingPanel::PollBindingCapture(const EditorGuiContext &context) {
        if (!listeningAction_ || !bindingCapture_ || router_ == nullptr)
            return;
        // A click in another workspace surface abandons this widget's capture;
        // it must not both activate that surface and author a pointer binding.
        if (std::ranges::any_of(router_->Snapshot().pointer.buttons, &Input::ButtonState::pressed) &&
            !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            listeningAction_.reset();
            bindingCapture_.reset();
            bindingCaptureContext_.Reset();
            statusMessage_.clear();
            return;
        }
        const auto result = bindingCapture_->Poll(*router_, bindingCaptureContext_);
        if (result.status == Input::BindingCaptureStatus::Pending)
            return;
        if (result.status == Input::BindingCaptureStatus::Captured) {
            const auto actions = router_->Actions();
            const auto action = std::ranges::find(actions, *listeningAction_, &Input::ActionDescriptor::id);
            if (action == actions.end()) {
                listeningAction_.reset();
                bindingCapture_.reset();
                bindingCaptureContext_.Reset();
                return;
            }
            const auto &bindings = BindingsFor(*action, router_->Profile());
            Input::BindingOverride change{*listeningAction_, {bindings.begin(), bindings.end()}};
            auto captured = *result.binding;
            if (listeningBinding_ && *listeningBinding_ < change.bindings.size()) {
                // Replacing a physical control preserves its role in a composite action.
                const auto &previous = change.bindings[*listeningBinding_];
                captured.component = previous.component;
                captured.scale *= previous.scale;
                change.bindings[*listeningBinding_] = captured;
            } else {
                change.bindings.push_back(captured);
            }
            const auto applied = CommitBindingChange(std::move(change));
            statusMessage_ = applied.HasError() ? applied.ErrorValue().message : kTextScope.Text(context.localization, "binding_updated");
        } else
            statusMessage_.clear();
        listeningAction_.reset();
        bindingCapture_.reset();
        bindingCaptureContext_.Reset();
    }

}  // namespace Horo::Editor
