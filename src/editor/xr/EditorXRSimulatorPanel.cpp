#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/XRSimulatorPanel.h"
#include "Horo/Foundation/Platform.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <algorithm>
#include <imgui.h>

namespace Horo::Editor {
    void XRSimulatorPanel::OnAttach(PanelContext &context) {
        model_ = context.xrSimulator;
        artifacts_ = context.xrSimulatorArtifacts;
    }

    void XRSimulatorPanel::OnDetach() {
        model_ = nullptr;
        artifacts_.reset();
        status_.clear();
    }

    void XRSimulatorPanel::DrawIcon(ImDrawList *drawList, const ImVec2 &position, const ImVec2 &size, ImU32 color) {
        drawList->AddRect(ImVec2(position.x + 3, position.y + 6), ImVec2(position.x + size.x - 3, position.y + size.y - 6), color, 4);
    }

    bool XRSimulatorPanel::Button(const EditorGuiContext &context, const char *key) const {
        return Ui::Button({.label = kTextScope.Text(context.localization, key).c_str(), .size = {ImGui::GetContentRegionAvail().x, 0}});
    }

    void XRSimulatorPanel::ResultMessage(const Result<void> &result) {
        status_ = result.HasError() ? result.ErrorValue().message : std::string{};
    }

    void XRSimulatorPanel::Apply(const XRSimulatorView &snapshot, const XRSimulatorCommandKind kind) {
        ResultMessage(model_->Apply({snapshot.state.session.session, kind, {}}));
    }

    void XRSimulatorPanel::DrawPlay(const XRSimulatorView &snapshot, const EditorWorkspaceViewModel &viewModel,
                                    EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        if (viewModel.playState == EditorPlayState::Paused) {
            if (Button(context, "run"))
                command.command = EditorWorkspaceViewCommand::ResumePlay;
            if (Button(context, "step")) {
                ResultMessage(model_->Apply({snapshot.state.session.session, XRSimulatorCommandKind::Step, {}}));
                if (status_.empty())
                    command.command = EditorWorkspaceViewCommand::StepPlay;
            }
        } else if (Button(context, "pause"))
            command.command = EditorWorkspaceViewCommand::PausePlay;
        if (Button(context, "reset"))
            Apply(snapshot, XRSimulatorCommandKind::Reset);
    }

    namespace {
        void MoveView(XR::XRSimulatorStep &controls, const XR::XRSimulatorSource &source, const Math::Vec3 before,
                      const Math::Quaternion prior) {
            const auto rotation = source.pose.orientation * prior.Inverse();
            for (std::size_t view = 0; view < controls.viewCount; ++view) {
                controls.views[view].position = source.pose.position + rotation.Rotate(controls.views[view].position - before);
                controls.views[view].orientation = rotation * controls.views[view].orientation;
            }
        }

        void EnableHands(XR::XRSimulatorStep &controls) {
            controls.sourceCount = 5;
            controls.jointCount = XR::XRHandTrackingHardLimits::MaximumJoints;
            for (std::size_t hand = 0; hand < 2; ++hand) {
                const Math::Vec3 palm{hand == 0 ? -0.25F : 0.25F, 1.2F, 2.6F};
                const auto role = hand == 0 ? XR::XRTrackedDeviceRole::LeftHand : XR::XRTrackedDeviceRole::RightHand;
                controls.sources[3 + hand] = {role, XR::XRTrackedPoseKind::Palm, XR::XRTrackedDeviceState::Tracked, {}};
                controls.sources[3 + hand].pose.position = palm;
                for (std::size_t joint = 0; joint < XR::XRHandTrackingHardLimits::JointsPerHand; ++joint) {
                    auto &value = controls.joints[hand * XR::XRHandTrackingHardLimits::JointsPerHand + joint];
                    value = {static_cast<XR::XRHandedness>(hand), static_cast<XR::XRHandJoint>(joint), {}, 0.008F};
                    value.pose.position =
                        palm + Math::Vec3{static_cast<float>(joint / 5) * 0.015F, static_cast<float>(joint % 5) * 0.015F, 0};
                }
            }
        }
    }  // namespace

    bool XRSimulatorPanel::DrawSource(XR::XRSimulatorStep &controls, const std::size_t index, const EditorGuiContext &context) {
        auto &source = controls.sources[index];
        const auto before = source.pose.position;
        const auto prior = source.pose.orientation;
        auto euler = prior.ToEulerRadians();
        ImGui::PushID(static_cast<int>(index));
        constexpr std::array roles{"head", "left_controller", "right_controller", "source", "left_hand", "right_hand"};
        if (const auto role = static_cast<std::size_t>(source.role); role < roles.size())
            ImGui::TextWrapped("%s", kTextScope.Text(context.localization, roles[role]).c_str());
        bool changed =
            ImGui::DragFloat3(kTextScope.Text(context.localization, "position").c_str(), &source.pose.position.x, 0.01F, -100, 100);
        changed |= ImGui::DragFloat3(kTextScope.Text(context.localization, "rotation").c_str(), &euler.x, 0.01F, -3.14F, 3.14F);
        ImGui::PopID();
        if (!changed)
            return false;
        source.pose.orientation = Math::Quaternion::FromEulerRadians(euler);
        source.pose.positionValid = true;
        source.pose.orientationValid = true;
        source.state = XR::XRTrackedDeviceState::Tracked;
        if (source.role == XR::XRTrackedDeviceRole::Head)
            MoveView(controls, source, before, prior);
        return true;
    }

    void XRSimulatorPanel::DrawControls(const XRSimulatorView &snapshot, const EditorGuiContext &context) {
        auto controls = snapshot.controls;
        bool changed{};
        for (std::size_t index = 0; index < controls.sourceCount; ++index)
            changed |= DrawSource(controls, index, context);
        if (controls.actionCount)
            changed |= ImGui::SliderFloat2(kTextScope.Text(context.localization, "move").c_str(), &controls.actions[0].x, -1, 1);
        if (changed)
            ResultMessage(model_->Apply({snapshot.state.session.session, XRSimulatorCommandKind::Controls, controls}));
    }

    void XRSimulatorPanel::DrawHands(const XRSimulatorView &snapshot, const EditorGuiContext &context) {
        if (Button(context, "enable_hands")) {
            auto controls = snapshot.controls;
            EnableHands(controls);
            ResultMessage(model_->Apply({snapshot.state.session.session, XRSimulatorCommandKind::Controls, controls}));
            return;
        }
        auto controls = snapshot.controls;
        if (controls.jointCount == 0)
            return;
        selectedJoint_ = std::min(selectedJoint_, static_cast<int>(controls.jointCount) - 1);
        ImGui::SliderInt(kTextScope.Text(context.localization, "joint").c_str(), &selectedJoint_, 0,
                         static_cast<int>(controls.jointCount) - 1);
        auto &joint = controls.joints[static_cast<std::size_t>(selectedJoint_)];
        if (ImGui::DragFloat3(kTextScope.Text(context.localization, "joint_position").c_str(), &joint.pose.position.x, 0.005F, -100, 100))
            ResultMessage(model_->Apply({snapshot.state.session.session, XRSimulatorCommandKind::Controls, controls}));
    }

    void XRSimulatorPanel::DrawEvents(const XRSimulatorView &snapshot, const EditorGuiContext &context) {
        constexpr std::array events{XR::XRSessionEvent::FocusLost, XR::XRSessionEvent::BecameFocused, XR::XRSessionEvent::SessionLost};
        constexpr std::array keys{"focus_lost", "focused", "session_lost"};
        for (std::size_t index = 0; index < events.size(); ++index) {
            if (!Button(context, keys[index]))
                continue;
            XR::XRSimulatorStep step;
            step.event = events[index];
            step.executeFrame = false;
            step.shouldRender = false;
            step.deltaNanoseconds = 0;
            ResultMessage(model_->Apply({snapshot.state.session.session, XRSimulatorCommandKind::Controls, step}));
        }
        if (Button(context, "tracking_lost")) {
            auto controls = snapshot.controls;
            for (std::size_t index = 0; index < controls.sourceCount; ++index) {
                controls.sources[index].state = XR::XRTrackedDeviceState::TrackingLost;
                controls.sources[index].pose.positionValid = false;
                controls.sources[index].pose.orientationValid = false;
            }
            controls.jointCount = 0;
            ResultMessage(model_->Apply({snapshot.state.session.session, XRSimulatorCommandKind::Controls, controls}));
        }
    }

    void XRSimulatorPanel::DrawCapture(const XRSimulatorView &snapshot, const EditorGuiContext &context) {
        using enum Horo::Editor::XRSimulatorCommandKind;
        if (Button(context, "record"))
            Apply(snapshot, ArmCapture);
        if (Button(context, "finish_record"))
            Apply(snapshot, StopCapture);
        if (Button(context, "cancel_record"))
            Apply(snapshot, CancelCapture);
        if (Button(context, "replay"))
            Apply(snapshot, Replay);
        constexpr std::array states{"capture_idle",    "capture_armed",  "capture_recording", "capture_complete",
                                    "capture_partial", "capture_failed", "capture_cancelled"};
        if (const auto index = static_cast<std::size_t>(snapshot.state.capture.state); index < states.size())
            ImGui::TextWrapped("%s", kTextScope.Text(context.localization, states[index]).c_str());
        ImGui::Text("%s: %zu; %s: %zu", kTextScope.Text(context.localization, "captured_steps").c_str(), snapshot.state.capture.steps,
                    kTextScope.Text(context.localization, "dropped_steps").c_str(), snapshot.state.capture.dropped);
    }

    void XRSimulatorPanel::DrawArtifacts(const XRSimulatorView &snapshot, const EditorGuiContext &context) {
        const auto port = artifacts_.lock();
        if (!port)
            return;
        ImGui::TextWrapped("%s", kTextScope.Text(context.localization, "import_path").c_str());
        static_cast<void>(Ui::InputTextControl("##simulator.import", importPath_.data(), importPath_.size(), context.theme.fonts));
        if (Button(context, "import")) {
            const auto bytes = port->ReadScenario(importPath_.data());
            if (bytes.HasError())
                status_ = bytes.ErrorValue().message;
            else
                ResultMessage(model_->Import(snapshot.state.session.session, bytes.Value()));
        }
        ImGui::TextWrapped("%s", kTextScope.Text(context.localization, "capture_name").c_str());
        static_cast<void>(Ui::InputTextControl("##simulator.capture", captureName_.data(), captureName_.size(), context.theme.fonts));
        if (Button(context, "export")) {
            const auto bytes = model_->Export(snapshot.state.session.session);
            if (bytes.HasError()) {
                status_ = bytes.ErrorValue().message;
                return;
            }
            AtomicFileReplacementReceipt receipt;
            const auto written = port->WriteCapture(captureName_.data(), bytes.Value(), receipt);
            ResultMessage(written);
            if (receipt.WasCommitted()) {
                status_ = std::string{port->PublicationNamespace()} + "/" + captureName_.data() +
                          (written.HasError() ? ": " + written.ErrorValue().message : std::string{});
            }
        }
    }

    void XRSimulatorPanel::DrawPanel(const ImVec2 &, const ImVec2 &, const EditorWorkspaceViewModel &viewModel,
                                     EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        ImGui::TextWrapped("%s", kTextScope.Text(context.localization, "evidence").c_str());
        const auto snapshot = model_ ? model_->Snapshot() : XRSimulatorView{};
        if (!snapshot.available) {
            ImGui::TextWrapped("%s", kTextScope.Text(context.localization, "inactive").c_str());
            if (model_ && Button(context, "start"))
                command.command = EditorWorkspaceViewCommand::StartPlay;
            return;
        }
        DrawPlay(snapshot, viewModel, command, context);
        DrawControls(snapshot, context);
        DrawHands(snapshot, context);
        DrawEvents(snapshot, context);
        DrawCapture(snapshot, context);
        DrawArtifacts(snapshot, context);
        if (!status_.empty())
            ImGui::TextWrapped("%s", status_.c_str());
    }
}  // namespace Horo::Editor
