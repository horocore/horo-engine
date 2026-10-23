#include "editor/screens/workspace/panels/input_mapping/InputMappingPanel.h"

#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/ErrorCode.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Horo::Editor {
    namespace {
        const ErrorCodeDescriptor ProfileExistsFailed{
            .domain = ErrorDomainId{"horo.editor.input"},
            .code = ErrorCode{"input.profile.exists_failed"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Input profile existence check failed.",
            .remediationHint = "Verify profile path permissions and retry.",
            .retryable = true,
            .userActionable = true,
        };

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

        const char *KeyName(const Input::Key key) noexcept {
            using enum Input::Key;
            static constexpr std::array letters{"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                                "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
            if (key >= Input::Key::A && key <= Input::Key::Z)
                return letters[static_cast<std::size_t>(key) - static_cast<std::size_t>(Input::Key::A)];
            switch (key) {
                case Input::Key::Escape:
                    return "Escape";
                case Input::Key::Enter:
                    return "Enter";
                case Input::Key::Space:
                    return "Space";
                case Input::Key::Delete:
                    return "Delete";
                default:
                    return "Key";
            }
        }

        void UpdateBindingDeadzone(Input::InputRouter &router, const Input::ActionDescriptor &selectedAction,
                                   const std::span<const Input::InputBinding> bindings, const std::size_t bindingIndex,
                                   const float deadzone, std::string &statusMessage) {
            Input::InputBindingProfile profile = router.Profile();
            auto existing = std::ranges::find(profile.overrides, selectedAction.id, &Input::BindingOverride::action);
            if (existing == profile.overrides.end()) {
                profile.overrides.emplace_back(selectedAction.id, std::vector<Input::InputBinding>(bindings.begin(), bindings.end()));
                existing = std::prev(profile.overrides.end());
            }
            existing->bindings[bindingIndex].deadzone = deadzone;
            if (const Result<void> applied = router.SetProfile(std::move(profile)); applied.HasError())
                statusMessage = applied.ErrorValue().message;
        }

        template <typename ApplyFn> bool PollGamepadButtons(const Input::GamepadState &gamepad, const ApplyFn &apply) {
            for (std::size_t index = 0; index < gamepad.buttons.size(); ++index)
                if (gamepad.buttons[index].pressed) {
                    apply(Input::InputBinding{.kind = Input::BindingControlKind::GamepadButton,
                                              .gamepadButton = static_cast<Input::GamepadButton>(index)});
                    return true;
                }
            return false;
        }

        template <typename ApplyFn> bool PollRawGamepadButtons(const Input::GamepadState &gamepad, const ApplyFn &apply) {
            for (std::size_t index = 0; index < gamepad.rawButtons.size(); ++index)
                if (gamepad.rawButtons[index].pressed) {
                    apply(Input::InputBinding{.kind = Input::BindingControlKind::RawGamepadButton,
                                              .rawControl = static_cast<std::uint16_t>(index)});
                    return true;
                }
            return false;
        }

        template <typename ApplyFn> bool PollGamepadAxes(const Input::GamepadState &gamepad, const ApplyFn &apply) {
            for (std::size_t index = 0; index < gamepad.axes.size(); ++index)
                if (std::fabs(gamepad.axes[index]) >= 0.75F) {
                    apply(Input::InputBinding{.kind = Input::BindingControlKind::GamepadAxis,
                                              .gamepadAxis = static_cast<Input::GamepadAxis>(index),
                                              .scale = gamepad.axes[index] < 0.0F ? -1.0F : 1.0F,
                                              .deadzoneKind = Input::DeadzoneKind::Axial,
                                              .deadzone = 0.15F});
                    return true;
                }
            return false;
        }

        template <typename ApplyFn> bool PollRawGamepadAxes(const Input::GamepadState &gamepad, const ApplyFn &apply) {
            for (std::size_t index = 0; index < gamepad.rawAxes.size(); ++index)
                if (std::fabs(gamepad.rawAxes[index]) >= 0.75F) {
                    apply(Input::InputBinding{.kind = Input::BindingControlKind::RawGamepadAxis,
                                              .rawControl = static_cast<std::uint16_t>(index),
                                              .scale = gamepad.rawAxes[index] < 0.0F ? -1.0F : 1.0F,
                                              .deadzoneKind = Input::DeadzoneKind::Axial,
                                              .deadzone = 0.15F});
                    return true;
                }
            return false;
        }

        template <typename ApplyFn> bool PollGamepadBinding(const Input::RawInputSnapshot &snapshot, const ApplyFn &apply) {
            return std::ranges::any_of(snapshot.gamepads, [&](const Input::GamepadState &gamepad) {
                return PollGamepadButtons(gamepad, apply) || PollRawGamepadButtons(gamepad, apply) || PollGamepadAxes(gamepad, apply) ||
                       PollRawGamepadAxes(gamepad, apply);
            });
        }
    }  // namespace

    void InputMappingPanel::OnAttach(PanelContext &context) {
        router_ = context.inputRouter;
    }

    void InputMappingPanel::OnDetach() {
        bindingCaptureContext_.Reset();
        listeningAction_.reset();
        router_ = nullptr;
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
        projectRoot_ = viewModel.projectRoot;
        const std::array labels{
            context.localization.Get("editor", "workspace.input_mapping.actions").c_str(),
            context.localization.Get("editor", "workspace.input_mapping.devices").c_str(),
            context.localization.Get("editor", "workspace.input_mapping.profiles").c_str(),
        };
        const auto selected = static_cast<int>(page_);
        const int next = Ui::DrawDockTabs(labels, selected, context.theme.fonts);
        page_ = static_cast<Page>(next);
        ImGui::BeginChild("##InputMappingContent", {size.x, size.y - 30.0F}, false, ImGuiWindowFlags_NoSavedSettings);
        PollBindingCapture(context);
        if (page_ == Page::Actions)
            DrawActions(context);
        else if (page_ == Page::Devices)
            DrawDevices(context);
        else
            DrawProfiles(context);
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
            ImGui::TextUnformatted(context.localization.Get("editor", "workspace.input_mapping.no_actions").c_str());
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
            ImGui::TableSetupColumn(context.localization.Get("editor", "workspace.input_mapping.action").c_str());
            ImGui::TableSetupColumn(context.localization.Get("editor", "workspace.input_mapping.type").c_str());
            ImGui::TableSetupColumn(context.localization.Get("editor", "workspace.input_mapping.bindings").c_str());
            ImGui::TableSetupColumn(context.localization.Get("editor", "workspace.input_mapping.status").c_str());
            ImGui::TableHeadersRow();
            for (std::size_t index = 0; index < actions.size(); ++index) {
                const Input::ActionDescriptor &action = actions[index];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(action.id.Value().c_str(), selectedAction_ == index, ImGuiSelectableFlags_SpanAllColumns))
                    selectedAction_ = index;
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(context.localization.Get("editor", ValueTypeLocalizationKey(action.valueType)).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%zu", BindingsFor(action, router_->Profile()).size());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(action.required ? context.localization.Get("editor", "workspace.input_mapping.required").c_str()
                                                       : context.localization.Get("editor", "workspace.input_mapping.optional").c_str());
            }
            ImGui::EndTable();
        }
    }

    void InputMappingPanel::DrawSelectedAction(const Input::ActionDescriptor &selectedAction, const EditorGuiContext &context) {
        ImGui::Separator();
        ImGui::TextUnformatted(selectedAction.id.Value().c_str());
        const auto &bindings = BindingsFor(selectedAction, router_->Profile());
        for (std::size_t bindingIndex = 0; bindingIndex < bindings.size(); ++bindingIndex) {
            const Input::InputBinding &binding = bindings[bindingIndex];
            if (binding.kind == Input::BindingControlKind::Key)
                ImGui::BulletText("%s%s%s%s%s", binding.requiredModifiers.control ? "Ctrl+" : "",
                                  binding.requiredModifiers.command ? "Cmd+" : "", binding.requiredModifiers.shift ? "Shift+" : "",
                                  binding.requiredModifiers.alt ? "Alt+" : "", KeyName(binding.key));
            else
                ImGui::BulletText("%s %d", context.localization.Get("editor", "workspace.input_mapping.binding").c_str(),
                                  static_cast<int>(binding.kind));
            if (binding.kind == Input::BindingControlKind::GamepadAxis || binding.kind == Input::BindingControlKind::RawGamepadAxis) {
                float deadzone = binding.deadzone;
                ImGui::PushID(static_cast<int>(bindingIndex));
                if (ImGui::SliderFloat(context.localization.Get("editor", "workspace.input_mapping.deadzone").c_str(), &deadzone, 0.0F,
                                       0.95F, "%.2f")) {
                    UpdateBindingDeadzone(*router_, selectedAction, bindings, bindingIndex, deadzone, statusMessage_);
                }
                ImGui::PopID();
            }
        }
        const std::string &rebind = context.localization.Get("editor", "workspace.input_mapping.rebind");
        if (Ui::Button({.label = rebind.c_str(), .variant = Ui::ButtonVariant::Secondary, .font = context.theme.fonts.sans})) {
            listeningAction_ = selectedAction.id;
            bindingCaptureContext_.Reset();
            bindingCaptureContext_ =
                router_->PushContext(Input::InputContextId{"editor.input_binding_capture"}, Input::InputContextKind::FocusedGuiWidget);
            statusMessage_ = context.localization.Get("editor", "workspace.input_mapping.press_key");
        }
    }

    void InputMappingPanel::DrawDevices(const EditorGuiContext &context) {
        if (router_ == nullptr)
            return;
        const auto &gamepads = router_->Snapshot().gamepads;
        if (gamepads.empty()) {
            ImGui::TextUnformatted(context.localization.Get("editor", "workspace.input_mapping.no_devices").c_str());
            return;
        }
        for (const Input::GamepadState &gamepad : gamepads) {
            const std::string cardId = std::format("device-{}", gamepad.id.slot);
            Ui::ScopedCard card(cardId.c_str(), {0.0F, 78.0F});
            ImGui::TextUnformatted(gamepad.name.c_str());
            ImGui::Text("%s %u / %s %llu", context.localization.Get("editor", "workspace.input_mapping.slot").c_str(), gamepad.id.slot,
                        context.localization.Get("editor", "workspace.input_mapping.generation").c_str(),
                        static_cast<unsigned long long>(gamepad.id.sessionGeneration));
            int assignment =
                router_->PlayerForGamepad(gamepad.id).has_value() ? static_cast<int>(*router_->PlayerForGamepad(gamepad.id)) + 1 : 0;
            const std::array<std::string, 5> assignmentLabels{context.localization.Get("editor", "workspace.input_mapping.unassigned"), "1",
                                                              "2", "3", "4"};
            const std::array<const char *, 5> assignmentItems{assignmentLabels[0].c_str(), assignmentLabels[1].c_str(),
                                                              assignmentLabels[2].c_str(), assignmentLabels[3].c_str(),
                                                              assignmentLabels[4].c_str()};
            ImGui::SetNextItemWidth(140.0F);
            if (ImGui::Combo(context.localization.Get("editor", "workspace.input_mapping.player").c_str(), &assignment,
                             assignmentItems.data(), assignmentItems.size())) {
                if (assignment == 0)
                    router_->UnassignGamepad(gamepad.id);
                else
                    static_cast<void>(router_->AssignGamepad(static_cast<Input::PlayerId>(assignment - 1), gamepad.id));
            }
        }
    }

    void InputMappingPanel::DrawProfiles(const EditorGuiContext &context) {
        if (router_ == nullptr)
            return;
        ImGui::Text("%s: %s", context.localization.Get("editor", "workspace.input_mapping.active_profile").c_str(),
                    router_->Profile().profileId.c_str());
        if (const std::string &save = context.localization.Get("editor", "workspace.input_mapping.save_profile");
            Ui::Button({.label = save.c_str(), .font = context.theme.fonts.sans})) {
            Input::InputBindingProfile profile = router_->Profile();
            profile.profileId = "editor-global";
            const Result<void> result = Input::SaveBindingProfileAtomically(EditorProfilePath(), profile);
            statusMessage_ = result.HasError() ? result.ErrorValue().message
                                               : context.localization.Get("editor", "workspace.input_mapping.profile_saved");
        }
        if (const auto projectProfile = ProjectUserProfilePath(); projectProfile.has_value()) {
            ImGui::SameLine();
            const std::string &saveProject = context.localization.Get("editor", "workspace.input_mapping.save_project_profile");
            if (Ui::Button({.label = saveProject.c_str(), .variant = Ui::ButtonVariant::Secondary, .font = context.theme.fonts.sans})) {
                Input::InputBindingProfile profile = router_->Profile();
                profile.profileId = "project-user";
                const Result<void> result = Input::SaveBindingProfileAtomically(*projectProfile, profile);
                statusMessage_ = result.HasError() ? result.ErrorValue().message
                                                   : context.localization.Get("editor", "workspace.input_mapping.project_profile_saved");
            }
        }
        ImGui::SameLine();
        const std::string &reload = context.localization.Get("editor", "workspace.input_mapping.reload_profile");
        if (Ui::Button({.label = reload.c_str(), .variant = Ui::ButtonVariant::Secondary, .font = context.theme.fonts.sans})) {
            const Result<Input::InputBindingProfile> loaded = LoadComposedProfile();
            if (loaded.HasError())
                statusMessage_ = loaded.ErrorValue().message;
            else if (const Result<void> applied = router_->SetProfile(loaded.Value()); applied.HasError())
                statusMessage_ = applied.ErrorValue().message;
            else
                statusMessage_ = context.localization.Get("editor", "workspace.input_mapping.profile_loaded");
        }
    }

    Result<Input::InputBindingProfile> InputMappingPanel::LoadComposedProfile() const {
        Input::InputBindingProfile merged{.profileId = "project-composed"};
        const auto mergeFile = [&](const std::filesystem::path &path) {
            if (std::error_code error; !std::filesystem::exists(path, error))
                return error ? Result<void>::Failure(MakeError(ProfileExistsFailed, error.message())) : Result<void>::Success();
            const Result<Input::InputBindingProfile> loaded = Input::LoadBindingProfile(path);
            if (loaded.HasError())
                return Result<void>::Failure(loaded.ErrorValue());
            Result<Input::InputBindingProfile> layered = Input::MergeBindingProfiles(merged, loaded.Value());
            if (layered.HasError())
                return Result<void>::Failure(layered.ErrorValue());
            merged = std::move(layered).Value();
            return Result<void>::Success();
        };
        if (!projectRoot_.empty()) {
            if (Result<void> project = mergeFile(projectRoot_ / ".horo" / "input.json"); project.HasError())
                return Result<Input::InputBindingProfile>::Failure(project.ErrorValue());
        }
        if (Result<void> editor = mergeFile(EditorProfilePath()); editor.HasError())
            return Result<Input::InputBindingProfile>::Failure(editor.ErrorValue());
        if (const auto projectUser = ProjectUserProfilePath(); projectUser.has_value()) {
            if (Result<void> user = mergeFile(*projectUser); user.HasError())
                return Result<Input::InputBindingProfile>::Failure(user.ErrorValue());
        }
        return Result<Input::InputBindingProfile>::Success(std::move(merged));
    }

    void InputMappingPanel::PollBindingCapture(const EditorGuiContext &context) {
        if (!listeningAction_.has_value() || router_ == nullptr)
            return;
        const Input::RawInputSnapshot &snapshot = router_->Snapshot();
        const auto apply = [this, &context](const Input::InputBinding &binding) {
            Input::InputBindingProfile profile = router_->Profile();
            if (const auto existing = std::ranges::find(profile.overrides, *listeningAction_, &Input::BindingOverride::action);
                existing == profile.overrides.end())
                profile.overrides.push_back(Input::BindingOverride{*listeningAction_, {binding}});
            else
                existing->bindings = {binding};
            const Result<void> applied = router_->SetProfile(std::move(profile));
            statusMessage_ = applied.HasError() ? applied.ErrorValue().message
                                                : context.localization.Get("editor", "workspace.input_mapping.binding_updated");
            listeningAction_.reset();
            bindingCaptureContext_.Reset();
        };
        for (std::size_t index = 1; index < static_cast<std::size_t>(Input::Key::Count); ++index) {
            if (const auto key = static_cast<Input::Key>(index); snapshot.State(key).pressed) {
                if (key == Input::Key::Escape) {
                    listeningAction_.reset();
                    bindingCaptureContext_.Reset();
                    statusMessage_.clear();
                    return;
                }
                apply(Input::InputBinding{.kind = Input::BindingControlKind::Key, .key = key, .requiredModifiers = snapshot.modifiers});
                return;
            }
        }
        for (std::size_t index = 0; index < static_cast<std::size_t>(Input::PointerButton::Count); ++index) {
            const auto button = static_cast<Input::PointerButton>(index);
            if (snapshot.State(button).pressed) {
                apply(Input::InputBinding{.kind = Input::BindingControlKind::PointerButton, .pointerButton = button});
                return;
            }
        }
        PollGamepadBinding(snapshot, apply);
    }

    std::filesystem::path InputMappingPanel::EditorProfilePath() const {
        return ResolveEditorSettingsHomeDirectory() / ".horo" / "input" / "editor.json";
    }

    std::optional<std::filesystem::path> InputMappingPanel::ProjectUserProfilePath() const {
        if (projectRoot_.empty())
            return std::nullopt;
        const Result<Application::ProjectMetadata> metadata = Application::LoadProjectMetadata(projectRoot_);
        if (metadata.HasError())
            return std::nullopt;
        return ResolveEditorSettingsHomeDirectory() / ".horo" / "input" / "projects" / (metadata.Value().projectId + ".json");
    }
}  // namespace Horo::Editor
