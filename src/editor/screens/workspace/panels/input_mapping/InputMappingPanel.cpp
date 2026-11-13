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

        const char *GyroscopeAvailabilityKey(const Input::SensorAvailability availability) noexcept {
            using enum Input::SensorAvailability;
            switch (availability) {
                case Unsupported:
                    return "gyro.availability_unsupported";
                case Disabled:
                    return "gyro.availability_disabled";
                case PermissionRequired:
                    return "gyro.availability_permission_required";
                case Suspended:
                    return "gyro.availability_suspended";
                case AwaitingSample:
                    return "gyro.availability_awaiting_sample";
                case NativeFailure:
                    return "gyro.availability_native_failure";
                case Available:
                    return "gyro.availability_available";
            }
            return "gyro.availability_unknown";
        }

        std::array<char, 48> DeviceCardId(const Input::GamepadDeviceId device) noexcept {
            std::array<char, 48> id{};
            const auto slot = std::to_chars(id.data(), id.data() + id.size(), device.slot);
            *slot.ptr = ':';
            const auto generation = std::to_chars(slot.ptr + 1, id.data() + id.size() - 1, device.sessionGeneration);
            *generation.ptr = '\0';
            return id;
        }

        void DrawDeviceAssignment(const Input::GamepadDeviceId device, Input::InputRouter &router, const EditorGuiContext &context) {
            const auto player = router.PlayerForGamepad(device);
            const bool custom = player && *player >= 4;
            std::array<char, 16> customNumber{};
            if (custom)
                (void)std::to_chars(customNumber.data(), customNumber.data() + customNumber.size() - 1,
                                    static_cast<std::uint64_t>(*player) + 1);
            const std::array<const char *, 6> items{InputMappingPanel::kTextScope.Text(context.localization, "unassigned").c_str(),
                                                    "1",
                                                    "2",
                                                    "3",
                                                    "4",
                                                    customNumber.data()};
            int assignment = player ? (custom ? 5 : static_cast<int>(*player) + 1) : 0;
            ImGui::TextWrapped("%s", InputMappingPanel::kTextScope.Text(context.localization, "player").c_str());
            ImGui::SetNextItemWidth(std::max(1.0F, ImGui::GetContentRegionAvail().x));
            if (Ui::ComboControl("Player", &assignment, items.data(), custom ? 6 : 5, context.theme.fonts)) {
                if (assignment == 0)
                    router.UnassignGamepad(device);
                else if (assignment <= 4)
                    (void)router.AssignGamepad(static_cast<Input::PlayerId>(assignment - 1), device);
            }
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
        accessibilityAction_.reset();
        router_ = nullptr;
    }

    void InputMappingPanel::OnPresentationHidden(EditorWorkspaceViewCommandData &) {
        bindingCaptureContext_.Reset();
        bindingCapture_.reset();
        listeningAction_.reset();
        deadzoneDraft_.reset();
        accessibilityAction_.reset();
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
        accessibilityAction_.reset();
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
            DrawActions(context, command);
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

    void InputMappingPanel::DrawActions(const EditorGuiContext &context, EditorWorkspaceViewCommandData &command) {
        if (router_ == nullptr)
            return;
        const auto actions = router_->Actions();
        if (actions.empty()) {
            ImGui::TextUnformatted(InputMappingPanel::kTextScope.Text(context.localization, "no_actions").c_str());
            return;
        }
        selectedAction_ = std::min(selectedAction_, actions.size() - 1);
        DrawActionTable(actions, context);
        DrawSelectedAction(actions[selectedAction_], context, command);
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
        accessibilityAction_.reset();
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

    void InputMappingPanel::DrawSelectedAction(const Input::ActionDescriptor &action, const EditorGuiContext &context, EditorWorkspaceViewCommandData &command) {
        if (displayedAction_ != action.id) {
            deadzoneDraft_.reset();
        accessibilityAction_.reset();
            displayedAction_ = action.id;
        }
        ImGui::Separator();
        ImGui::TextWrapped("%s", action.id.Value().c_str());
        const auto &bindings = BindingsFor(action, router_->Profile());
        for (std::size_t index = 0; index < bindings.size(); ++index) {
            const auto label = Ui::ShortcutLabel(bindings[index]);
            ImGui::BulletText("%s", label.c_str());
            ImGui::PushID(static_cast<int>(index));
            const bool changed = DrawBindingDirection(action, bindings, index, context) ||
                                 DrawBindingActions(action, bindings, index, context) ||
                                 DrawBindingDeadzone(action, bindings, index, context) ||
                                 DrawExtendedBindingAction(action, bindings, index, context, command);
            ImGui::PopID();
            // Publishing a profile may retire the borrowed bindings storage.
            if (changed)
                return;
        }
        const auto &add = kTextScope.Text(context.localization, "add_binding");
        if (Ui::Button({.label = add.c_str(), .variant = Ui::ButtonVariant::Secondary, .font = context.theme.fonts.sans}))
            StartBindingCapture(action.id, std::nullopt, context);
        DrawAccessibilityControls(action, context);
        if (action.valueType != Input::ActionValueType::Digital &&
            Ui::Button({.label = LocalizedText(context.localization, "gyro.add").c_str(),
                        .variant = Ui::ButtonVariant::Secondary, .font = context.theme.fonts.sans})) {
            listeningAction_.reset();
            bindingCaptureContext_.Reset();
            command.command = EditorWorkspaceViewCommand::OpenGyroscopeBinding;
            command.gyroscopeBinding = GyroscopeBindingEditRequest{action.id, router_->RoutingState({}).configurationRevision, {}};
        }
    }

    bool InputMappingPanel::DrawExtendedBindingAction(const Input::ActionDescriptor &action,
                                                       const std::span<const Input::InputBinding> bindings,
                                                       const std::size_t index, const EditorGuiContext &context,
                                                       EditorWorkspaceViewCommandData &command) {
        const auto &binding = bindings[index];
        if (binding.kind == Input::BindingControlKind::GyroscopeAxis &&
            Ui::Button({.label = LocalizedText(context.localization, "gyro.edit").c_str(),
                        .variant = Ui::ButtonVariant::Secondary, .font = context.theme.fonts.sans})) {
            listeningAction_.reset();
            bindingCaptureContext_.Reset();
            command.command = EditorWorkspaceViewCommand::OpenGyroscopeBinding;
            command.gyroscopeBinding = GyroscopeBindingEditRequest{
                action.id, router_->RoutingState({}).configurationRevision, GyroscopeBindingReplacement{index, binding}};
            return true;
        }
        if (action.valueType != Input::ActionValueType::Digital ||
            !Ui::Button({.label = LocalizedText(context.localization, "accessibility.edit").c_str(),
                         .variant = Ui::ButtonVariant::Secondary, .font = context.theme.fonts.sans}))
            return false;
        accessibilityAction_ = action.id;
        accessibilityBinding_ = index;
        accessibilityConfigurationRevision_ = router_->RoutingState({}).configurationRevision;
        accessibilityBaseline_ = binding;
        accessibilityDraft_ = BindingAccessibilityEditorDraft::FromPolicy(binding.accessibility);
        return true;
    }

    bool InputMappingPanel::DrawBindingDirection(const Input::ActionDescriptor &action, const std::span<const Input::InputBinding> bindings,
                                                 const std::size_t index, const EditorGuiContext &context) {
        if (action.valueType == Input::ActionValueType::Digital)
            return false;
        const auto &binding = bindings[index];
        int component = binding.component;
        int direction = binding.scale < 0.0F ? 1 : 0;
        const std::array<const char *, 2> components{"X", "Y"};
        const std::array directions{kTextScope.Text(context.localization, "positive").c_str(),
                                    kTextScope.Text(context.localization, "negative").c_str()};
        const bool changedComponent = action.valueType == Input::ActionValueType::Axis2D &&
                                      Ui::DrawComboPropRow(kTextScope.Text(context.localization, "component").c_str(), "##Component",
                                                           component, components, context.theme.fonts);
        const bool changedDirection = Ui::DrawComboPropRow(kTextScope.Text(context.localization, "direction").c_str(), "##Direction",
                                                           direction, directions, context.theme.fonts);
        if (!changedComponent && !changedDirection)
            return false;
        Input::BindingOverride change{action.id, {bindings.begin(), bindings.end()}};
        auto &updated = change.bindings[index];
        updated.component = static_cast<std::uint8_t>(component);
        updated.scale = direction == 1 ? -std::abs(binding.scale) : std::abs(binding.scale);
        const auto applied = CommitBindingChange(std::move(change));
        statusMessage_ = applied.HasError() ? applied.ErrorValue().message : kTextScope.Text(context.localization, "binding_updated");
        return true;
    }

    bool InputMappingPanel::DrawBindingActions(const Input::ActionDescriptor &action, const std::span<const Input::InputBinding> bindings,
                                               const std::size_t index, const EditorGuiContext &context) {
        if (Ui::Button({.label = kTextScope.Text(context.localization, "rebind").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .font = context.theme.fonts.sans}))
            StartBindingCapture(action.id, index, context);
        if (!Ui::Button({.label = kTextScope.Text(context.localization, "remove_binding").c_str(),
                         .variant = Ui::ButtonVariant::Secondary,
                         .font = context.theme.fonts.sans}))
            return false;
        Input::BindingOverride change{action.id, {bindings.begin(), bindings.end()}};
        change.bindings.erase(change.bindings.begin() + static_cast<std::ptrdiff_t>(index));
        const auto applied = CommitBindingChange(std::move(change));
        statusMessage_ = applied.HasError() ? applied.ErrorValue().message : kTextScope.Text(context.localization, "binding_updated");
        deadzoneDraft_.reset();
        accessibilityAction_.reset();
        return true;
    }

    bool InputMappingPanel::DrawBindingDeadzone(const Input::ActionDescriptor &action, const std::span<const Input::InputBinding> bindings,
                                                const std::size_t index, const EditorGuiContext &context) {
        const auto &binding = bindings[index];
        if (binding.kind != Input::BindingControlKind::GamepadAxis && binding.kind != Input::BindingControlKind::RawGamepadAxis)
            return false;
        float deadzone = deadzoneDraft_ && deadzoneDraft_->first == index ? deadzoneDraft_->second : binding.deadzone;
        const auto edit = Ui::DrawFloatPropRow(kTextScope.Text(context.localization, "deadzone").c_str(), "##Deadzone", deadzone,
                                               context.theme.fonts, {.speed = 0.01F, .minimum = 0.0F, .maximum = 0.95F});
        if (edit.changed)
            deadzoneDraft_ = std::pair{index, deadzone};
        if (!edit.committed || !deadzoneDraft_ || deadzoneDraft_->first != index)
            return false;
        Input::BindingOverride change{action.id, {bindings.begin(), bindings.end()}};
        change.bindings[index].deadzone = deadzoneDraft_->second;
        const auto applied = CommitBindingChange(std::move(change));
        statusMessage_ = applied.HasError() ? applied.ErrorValue().message : std::string{};
        deadzoneDraft_.reset();
        accessibilityAction_.reset();
        return true;
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

    void DrawInputDeviceCard(const Input::GamepadState &gamepad, Input::InputRouter &router, const EditorGuiContext &context) {
        const auto id = DeviceCardId(gamepad.id);
        Ui::ScopedCard card(id.data(), {0.0F, 0.0F}, Theme::Layout::CardPad, Theme::Layout::CardPad, Theme::Bg2(), true);
        ImGui::TextWrapped("%s", gamepad.name.c_str());
        ImGui::TextWrapped("%s %u / %s %llu", InputMappingPanel::kTextScope.Text(context.localization, "slot").c_str(), gamepad.id.slot,
                           InputMappingPanel::kTextScope.Text(context.localization, "generation").c_str(),
                           static_cast<unsigned long long>(gamepad.id.sessionGeneration));
        ImGui::TextWrapped("%s: %s", InputMappingPanel::kTextScope.Text(context.localization, "gyro.status").c_str(),
                           InputMappingPanel::kTextScope
                               .Text(context.localization, GyroscopeAvailabilityKey(gamepad.gyroscope.availability))
                               .c_str());
        DrawDeviceAssignment(gamepad.id, router, context);
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
        for (const Input::GamepadState &gamepad : gamepads)
            DrawInputDeviceCard(gamepad, *router_, context);
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
        // A gesture in another workspace surface abandons this widget's capture;
        // it must not both interact with that surface and author a pointer binding.
        const auto &pointer = router_->Snapshot().pointer;
        const bool pointerGesture =
            std::ranges::any_of(pointer.buttons, &Input::ButtonState::pressed) || pointer.wheelX != 0.0F || pointer.wheelY != 0.0F;
        if (pointerGesture && !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
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
