#include "XrSetupModal.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/XR/XRFeatureNegotiation.h"
#include "Horo/XR/XRSessionLifecycle.h"
#include "Horo/XR/XRTrackingSnapshot.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace Horo::Editor {
    namespace {
        constexpr GuiLocalizationScope TextScope{"workspace.xr_setup"};

        /** @brief Labels the actual published test outcome. */
        const char *TestKey(const EditorXrSetupTestState test) noexcept {
            using enum EditorXrSetupTestState;
            switch (test) {
                case NotRun:
                    return "test_not_run";
                case Queued:
                    return "test_queued";
                case Passed:
                    return "test_passed";
                case Failed:
                    return "test_failed";
                case Cancelled:
                    return "test_cancelled";
            }
            return "test_failed";
        }

        /** @brief Pairs a redacted host result with recovery guidance. */
        const char *IssueKey(const EditorXrSetupIssue issue) noexcept {
            using enum EditorXrSetupIssue;
            switch (issue) {
                case None:
                    return "permissions";
                case Busy:
                    return "busy";
                case Unavailable:
                    return "unavailable";
                case Renderer:
                    return "renderer_incompatible";
                case Unsupported:
                    return "unsupported";
                case Permission:
                    return "permission_denied";
                case Stale:
                    return "stale";
                case Capacity:
                    return "capacity";
                case Invalid:
                    return "invalid";
                case Cleanup:
                    return "cleanup";
            }
            return "unavailable";
        }

        /** @brief Displays requested project policy without inferring effective capability admission. */
        const char *ProfileKey(XR::XRFeatureProfile profile) noexcept {
            switch (profile) {
                using enum XR::XRFeatureProfile;
                case Projection1_0:
                    return "profile_projection";
                case TrackedInteraction1_0:
                    return "profile_tracked";
                case Count:
                    return "profile_unavailable";
            }
            return "profile_unavailable";
        }

        constexpr std::array StatusKeys{"inspect_unavailable", "inspect_idle",    "inspect_waiting", "inspect_unfocused",
                                        "inspect_events",      "inspect_current", "inspect_stale"};
        constexpr std::array SessionKeys{"inspect_session_inactive", "inspect_session_ready",    "inspect_session_running",
                                         "inspect_session_visible",  "inspect_session_focused",  "inspect_session_stopping",
                                         "inspect_session_lost",     "inspect_session_destroyed"};
        constexpr std::array TrackingKeys{"inspect_active", "inspect_tracking_lost", "inspect_tracking_permission", "inspect_session_lost"};
        constexpr std::array CapabilityStateKeys{"inspect_unsupported",         "inspect_not_available", "inspect_available",
                                                 "inspect_permission_required", "inspect_denied",        "inspect_dependency_missing",
                                                 "inspect_incompatible",        "inspect_temporary",     "inspect_session_lost",
                                                 "inspect_policy_disabled"};
        constexpr std::array DecisionKeys{"inspect_not_requested", "inspect_required", "inspect_optional_enabled",
                                          "inspect_optional_disabled", "inspect_projection_fallback"};
        constexpr std::array SpaceKeys{"inspect_view", "inspect_local", "inspect_stage", "inspect_world", "inspect_action_space"};
        constexpr std::array ActionKeys{"inspect_boolean", "inspect_scalar", "inspect_vector"};
        constexpr std::array HandKeys{"inspect_left_hand", "inspect_right_hand"};
        constexpr std::array DeviceKeys{"inspect_tracked", "inspect_tracking_lost", "inspect_disconnected"};

        static_assert(TrackingKeys.size() == static_cast<std::size_t>(XR::XRTrackingState::Count));
        static_assert(CapabilityStateKeys.size() == static_cast<std::size_t>(XR::XRCapabilityState::Count));
        static_assert(SpaceKeys.size() == static_cast<std::size_t>(XR::XRSpaceKind::Count));
        static_assert(DeviceKeys.size() == static_cast<std::size_t>(XR::XRTrackedDeviceState::Count));
        static_assert(ActionKeys.size() == static_cast<std::size_t>(XR::XRTrackedActionKind::Count));

        /** @brief Maps finite typed observations to keys with an explicit unavailable fallback. */
        template <typename Value, std::size_t Size>
        const char *InspectionKey(Value value, const std::array<const char *, Size> &keys) noexcept {
            const auto index = static_cast<std::size_t>(value);
            return index < keys.size() ? keys[index] : "inspect_unavailable";
        }

        /** @brief Names only the capabilities in the inspector's disclosure scope. */
        const char *CapabilityKey(XR::XRCapability capability) noexcept {
            using enum XR::XRCapability;
            switch (capability) {
                case Projection:
                    return "inspect_projection";
                case ViewSpace:
                    return "inspect_view";
                case LocalSpace:
                    return "inspect_local";
                case StageSpace:
                    return "inspect_stage";
                case CanonicalInputProjection:
                    return "inspect_action_projection";
                case HandTracking:
                    return "inspect_hands";
                case EyeGaze:
                    return "inspect_eye_gaze";
                default:
                    return "inspect_unavailable";
            }
        }

        /** @brief Wraps both columns and formats only bounded numbers into stack storage. */
        class InspectionRows final {
        public:
            explicit InspectionRows(const EditorGuiContext &context) : context_(context) {}

            void Begin(std::string_view label) const {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                Cell(label);
                ImGui::TableSetColumnIndex(1);
            }

            void Cell(std::string_view value) const {
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, value, Theme::Text(), context_.theme.fonts);
            }

            void Text(std::string_view label, std::string_view value) const {
                Begin(label);
                Cell(value);
            }

            template <typename... Values> void Numbers(std::format_string<Values...> format, Values &&...values) const {
                std::array<char, 96> buffer{};
                const auto result = std::format_to_n(buffer.data(), buffer.size(), format, std::forward<Values>(values)...);
                Cell({buffer.data(), static_cast<std::size_t>(result.out - buffer.data())});
            }

        private:
            const EditorGuiContext &context_;
        };
    }  // namespace

    /** @copydoc XrSetupModal::XrSetupModal */
    XrSetupModal::XrSetupModal(const EditorGuiContext &context, IEditorXrSetup *setup)
        : EditorModal(TextScope), context_(context), setup_(setup) {}

    ModalId XrSetupModal::Id() const {
        return ModalId{0x58525345545550ULL};
    }

    ModalPresentation XrSetupModal::Presentation() const {
        return {.size = ModalSizePolicy::Compact};
    }

    ModalClosePolicy XrSetupModal::ClosePolicy() const {
        return {};
    }

    Result<void> XrSetupModal::OnOpen(EditorModalContext &) {
        gazeRejected_ = false;
        return Result<void>::Success();
    }

    CloseDecision XrSetupModal::CanClose(ModalCloseReason) {
        return CloseDecision::Allow;
    }

    void XrSetupModal::OnClose(ModalCloseReason) {
        if (setup_) {
            setup_->CancelSetupTest(request_);
            if (auto *preview = setup_->HapticPreview())
                preview->CancelHapticPreview(previewRequest_);
        }
    }

    const std::string &XrSetupModal::Text(const char *key) const {
        return LocalizedText(context_.localization, key);
    }

    /** @copydoc XrSetupModal::DrawInspection */
    void XrSetupModal::DrawInspection(const EditorXrInspectionSnapshot &report) const {
        const auto &fonts = context_.theme.fonts;
        const float width = ImGui::GetContentRegionAvail().x;
        Ui::WrappedText(width, Text("inspect_title"), Theme::Text(), fonts);
        Ui::WrappedText(width, Text(InspectionKey(report.status, StatusKeys)), Theme::Muted(), fonts);
        if (report.privacy == EditorXrInspectionPrivacy::Redacted)
            Ui::WrappedText(width, Text("inspect_redacted"), Theme::Muted(), fonts);
        if (!report.session || !ImGui::BeginTable("##XrInspection", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_RowBg))
            return;
        const InspectionRows rows{context_};
        rows.Text(Text("inspect_session"), Text(InspectionKey(*report.session, SessionKeys)));
        if (report.status != EditorXrInspectionStatus::Stale && report.capabilityRevision != 0) {
            rows.Begin(Text("inspect_generations"));
            rows.Numbers("{} / {} / {}", report.runtimeGeneration, report.systemGeneration, report.sessionGeneration);
            const auto focus = report.status == EditorXrInspectionStatus::EventsPending ? "inspect_not_available"
                                                                                        : (report.focused ? "inspect_yes" : "inspect_no");
            rows.Text(Text("inspect_focus"), Text(focus));
            rows.Begin(Text("inspect_capability_revision"));
            rows.Numbers("{}", report.capabilityRevision);
            for (const auto &entry : report.capabilities) {
                rows.Begin(Text(CapabilityKey(entry.kind)));
                rows.Cell(Text(InspectionKey(entry.observed, CapabilityStateKeys)));
                rows.Cell(Text(InspectionKey(entry.decision, DecisionKeys)));
            }
        }
        if (report.tracking) {
            rows.Text(Text("inspect_tracking"), Text(InspectionKey(*report.tracking, TrackingKeys)));
            rows.Begin(Text("inspect_tracking_revisions"));
            rows.Numbers("{} / {} / {}", report.trackingRevision, report.originRevision, report.bindingRevision);
            rows.Begin(Text("inspect_capture_ms"));
            rows.Numbers("{}", report.sampleNanoseconds / 1'000'000);
            rows.Begin(Text("inspect_devices"));
            rows.Numbers("{}", report.deviceCount);
            const auto validity = [&](const EditorXrInspectionValidity &evidence) {
                rows.Cell(Text("inspect_position_validity"));
                rows.Numbers("{} / {} / {}", evidence.position[2], evidence.position[1], evidence.position[0]);
                rows.Cell(Text("inspect_orientation_validity"));
                rows.Numbers("{} / {} / {}", evidence.orientation[2], evidence.orientation[1], evidence.orientation[0]);
                rows.Cell(Text("inspect_confidence"));
                rows.Numbers("{} / {} / {}", evidence.confidence[2], evidence.confidence[1], evidence.confidence[0]);
            };
            rows.Begin(Text("inspect_poses"));
            validity(report.poses);
            for (std::size_t index = 0; index < report.hands.size(); ++index) {
                const auto &hand = report.hands[index];
                rows.Begin(Text(HandKeys[index]));
                if (!hand.state) {
                    rows.Cell(Text("inspect_not_available"));
                    continue;
                }
                rows.Cell(Text(InspectionKey(*hand.state, DeviceKeys)));
                rows.Cell(Text("inspect_hand_generation"));
                rows.Numbers("{}", hand.generation);
                validity(hand.validity);
            }
            for (std::size_t index = 0; index < report.spaces.size(); ++index) {
                if (report.spaces[index] == 0)
                    continue;
                rows.Begin(Text(SpaceKeys[index]));
                rows.Cell(Text("inspect_space_references"));
                rows.Numbers("{}", report.spaces[index]);
            }
            rows.Begin(Text("inspect_action_count"));
            rows.Numbers("{} / {}", report.activeActionCount, report.actionCount);
            const auto visibleActions =
                report.omittedActionCount <= report.actionCount
                    ? std::min(report.actions.size(), static_cast<std::size_t>(report.actionCount - report.omittedActionCount))
                    : 0U;
            for (std::size_t index = 0; index < visibleActions; ++index) {
                const auto &action = report.actions[index];
                rows.Begin(Text(InspectionKey(action.kind, ActionKeys)));
                rows.Cell(Text("inspect_binding_source"));
                rows.Numbers("{} / {}", action.bindingIndex, action.deviceGeneration);
                rows.Cell(Text(action.active ? "inspect_active" : "inspect_session_inactive"));
            }
            if (report.omittedActionCount) {
                rows.Begin(Text("inspect_omitted_actions"));
                rows.Numbers("{}", report.omittedActionCount);
            }
        }
        ImGui::EndTable();
    }

    /** @copydoc XrSetupModal::DrawHapticPreview */
    void XrSetupModal::DrawHapticPreview(IEditorXrHapticPreview &preview) {
        using enum EditorXrHapticHand;
        using enum EditorXrHapticPreviewState;
        const auto &fonts = context_.theme.fonts;
        constexpr float PreviewAmplitude = 0.3F;
        constexpr auto PreviewDuration = Duration::FromMilliseconds(150);
        for (const auto hand : {Left, Right}) {
            if (!Ui::Button({.label = Text(hand == Left ? "haptic_left" : "haptic_right").c_str(),
                             .variant = Ui::ButtonVariant::Secondary,
                             .enabled = preview.CanPreviewHaptic(hand),
                             .font = fonts.sans,
                             .style = {.width = Ui::StyleWidth::FillAvailable}}))
                continue;
            const auto result = preview.QueueHapticPreview({hand, PreviewAmplitude, PreviewDuration});
            previewRejected_ = result.HasError();
            if (result.HasValue())
                previewRequest_ = result.Value();
        }

        constexpr std::array keys{"", "haptic_queued", "haptic_submitted", "haptic_unsupported", "haptic_rejected", "haptic_cancelled"};
        const auto state = previewRejected_ ? Rejected : preview.ReadHapticPreview(previewRequest_);
        const auto index = static_cast<std::size_t>(state);
        if (state != Unknown && index < keys.size())
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(keys[index]), Theme::Muted(), fonts);
    }

    /** @copydoc XrSetupModal::DrawEyeGazePreview */
    void XrSetupModal::DrawEyeGazePreview(IEditorXrEyeGazePreview &preview) {
        using enum EditorXrEyeGazeState;
        const auto state = preview.ReadEyeGazeState();
        if (state == Unavailable && !gazeRejected_)
            return;
        const auto &fonts = context_.theme.fonts;
        constexpr std::array keys{"gaze_rejected", "gaze_scope", "gaze_armed", "gaze_waiting", "gaze_tracking"};
        const auto index = static_cast<std::size_t>(state);
        if (index >= keys.size())
            return;
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(gazeRejected_ ? "gaze_rejected" : keys[index]), Theme::Muted(), fonts);
        const bool canArm = state == Ready;
        if (state != Unavailable && Ui::Button({.label = Text(canArm ? "gaze_allow" : "gaze_revoke").c_str(),
                                                .variant = Ui::ButtonVariant::Secondary,
                                                .font = fonts.sans,
                                                .style = {.width = Ui::StyleWidth::FillAvailable}})) {
            if (canArm)
                gazeRejected_ = preview.ArmEyeGaze().HasError();
            else {
                preview.RevokeEyeGaze();
                gazeRejected_ = false;
            }
        }
    }

    /** @copydoc XrSetupModal::DrawPlaneCapturePreview */
    void XrSetupModal::DrawPlaneCapturePreview(IEditorXrPlaneCapturePreview &preview) {
        using enum EditorXrPlaneCaptureState;
        const auto state = preview.ReadPlaneCaptureState();
        if (state == Unavailable && !planesRejected_)
            return;
        const auto &fonts = context_.theme.fonts;
        constexpr std::array keys{"planes_rejected", "planes_scope", "planes_armed", "planes_waiting", "planes_tracking"};
        const auto index = static_cast<std::size_t>(state);
        if (index >= keys.size())
            return;
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(planesRejected_ ? "planes_rejected" : keys[index]), Theme::Muted(), fonts);
        const bool canArm = state == Ready;
        if (state != Unavailable && Ui::Button({.label = Text(canArm ? "planes_allow" : "planes_revoke").c_str(),
                                                .variant = Ui::ButtonVariant::Secondary,
                                                .font = fonts.sans,
                                                .style = {.width = Ui::StyleWidth::FillAvailable}})) {
            if (canArm)
                planesRejected_ = preview.ArmPlaneCapture().HasError();
            else {
                preview.RevokePlaneCapture();
                planesRejected_ = false;
            }
        }
    }

    ModalFrameResult XrSetupModal::Draw() {
        const auto &fonts = context_.theme.fonts;
        Ui::ScopedModalShell shell({.id = "XrSetup",
                                    .title = Text("title").c_str(),
                                    .requestedSize = {600, 480},
                                    .minimumWidth = 260,
                                    .minimumHeight = 260,
                                    .footerHeight = 108},
                                   fonts);
        const auto report = setup_ ? setup_->ReadSetup() : EditorXrSetupReport{};
        ImGui::BeginChild("##XrSetupBody", {0, shell.BodyHeight()}, false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        const float width = ImGui::GetContentRegionAvail().x;
        Ui::WrappedText(width, Text(report.loaderMajor ? "runtime_discovered" : "runtime_missing"), Theme::Text(), fonts);
        if (report.loaderMajor) {
            std::array<char, 17> version{};
            const auto display =
                std::format_to_n(version.data(), version.size(), "{}.{}.{}", report.loaderMajor, report.loaderMinor, report.loaderPatch);
            Ui::WrappedText(width, {version.data(), static_cast<std::size_t>(display.out - version.data())}, Theme::Text(), fonts);
        }
        Ui::WrappedText(width, Text(report.rendererReady ? "renderer_ready" : "renderer_missing"), Theme::Text(), fonts);
        if (report.requestedProfile)
            Ui::WrappedText(width, Text(ProfileKey(*report.requestedProfile)), Theme::Text(), fonts);
        Ui::WrappedText(width, Text(TestKey(report.test)), Theme::Text(), fonts);
        const auto issue = rejected_ ? EditorXrSetupIssue::Unavailable : report.issue;
        Ui::WrappedText(width, Text(IssueKey(issue)), Theme::Muted(), fonts);
        DrawInspection(setup_ ? setup_->ReadInspection() : EditorXrInspectionSnapshot{});
        if (auto *preview = setup_ ? setup_->HapticPreview() : nullptr)
            DrawHapticPreview(*preview);
        if (auto *preview = setup_ ? setup_->PlaneCapturePreview() : nullptr)
            DrawPlaneCapturePreview(*preview);
        if (auto *preview = setup_ ? setup_->EyeGazePreview() : nullptr)
            DrawEyeGazePreview(*preview);
        ImGui::EndChild();

        shell.BeginFooter({16, 12});
        if (Ui::Button({.label = Text("run_test").c_str(),
                        .variant = Ui::ButtonVariant::Primary,
                        .enabled = report.mayRequestTest,
                        .font = fonts.sans,
                        .style = {.width = Ui::StyleWidth::FillAvailable}})) {
            const auto queued = setup_->RequestSetupTest();
            rejected_ = queued.HasError();
            if (queued.HasValue())
                request_ = queued.Value();
        }
        const bool close = Ui::Button({.label = Text("close").c_str(),
                                       .variant = Ui::ButtonVariant::Secondary,
                                       .font = fonts.sans,
                                       .style = {.width = Ui::StyleWidth::FillAvailable}});
        shell.EndFooter();
        return close || shell.CloseRequested() ? ModalFrameResult::RequestClose(ModalCloseReason::Completed) : ModalFrameResult::None();
    }
}  // namespace Horo::Editor
