#include "InputGyroscopeEditor.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Runtime/InputAccessibility.h"
#include "InputMappingPanel.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iterator>
#include <limits>

namespace Horo::Editor {
    namespace {
        /** @brief Editing never retargets another profile/action generation. */
        const ErrorCodeDescriptor InvalidGyroscopeDraft{ErrorDomainId{"horo.editor.input"},
                                                        ErrorCode{"input.gyroscope.draft_invalid"},
                                                        ErrorSeverity::Error,
                                                        "The gyroscope draft is invalid, stale or exceeds the binding inventory.",
                                                        "Review the values and reopen the draft if the action/profile changed.",
                                                        false,
                                                        true};

        /** @brief Formats a float with enough digits to preserve the existing canonical binding exactly. */
        std::array<char, 32> FormatNumber(const float value) noexcept {
            std::array<char, 32> field{};
            const auto formatted = std::to_chars(field.data(), field.data() + field.size() - 1, value, std::chars_format::general,
                                                 std::numeric_limits<float>::max_digits10);
            return formatted.ec == std::errc{} ? field : std::array<char, 32>{};
        }

        /** @brief Accepts a complete finite number with dot/comma decimal separator, without changing the editable text. */
        std::optional<float> ParseNumber(std::array<char, 32> field) noexcept {
            const auto end = std::ranges::find(field, '\0');
            if (end == field.begin() || end == field.end())
                return std::nullopt;
            if (end[-1] < '0' || end[-1] > '9')
                return std::nullopt;
            std::ranges::replace(field, ',', '.');
            float value{};
            const auto parsed = std::from_chars(field.data(), field.data() + (end - field.begin()), value);
            return parsed.ec == std::errc{} && parsed.ptr == field.data() + (end - field.begin()) && std::isfinite(value)
                       ? std::optional{value}
                       : std::nullopt;
        }

        /** @brief Shared bounded value projection; frame checks do not construct owned diagnostic strings. */
        std::optional<Input::InputBinding> ParseDraft(const GyroscopeBindingEditorDraft &draft,
                                                      const Input::InputBinding &baseline) noexcept {
            const auto rate = ParseNumber(draft.calibration);
            const auto scale = ParseNumber(draft.sensitivity);
            if (!rate || !scale || draft.axis < 0 || draft.axis >= static_cast<int>(Input::SensorAxis::Count) || draft.component < 0 ||
                draft.component > 1 || baseline.kind != Input::BindingControlKind::GyroscopeAxis)
                return std::nullopt;
            auto binding = baseline;
            binding.gyroscope = {static_cast<Input::SensorAxis>(draft.axis), *rate};
            binding.component = static_cast<std::uint8_t>(draft.component);
            binding.scale = *scale;
            return binding.gyroscope.IsValid() ? std::optional{binding} : std::nullopt;
        }

        /** @brief Resolves an owned request against the current authority before borrowing any binding inventory. */
        Result<Input::InputBinding> ResolveBaseline(const Input::InputRouter &router, const GyroscopeBindingEditRequest &request) {
            using Output = Result<Input::InputBinding>;
            if (!request.configurationRevision || router.RoutingState({}).configurationRevision != request.configurationRevision)
                return Output::Failure(MakeError(InvalidGyroscopeDraft));
            const auto actions = router.Actions();
            const auto action = std::ranges::find(actions, request.action, &Input::ActionDescriptor::id);
            if (action == actions.end() || action->valueType == Input::ActionValueType::Digital)
                return Output::Failure(MakeError(InvalidGyroscopeDraft));
            const auto overridden = std::ranges::find(router.Profile().overrides, request.action, &Input::BindingOverride::action);
            const auto &bindings = overridden == router.Profile().overrides.end() ? action->defaultBindings : overridden->bindings;
            if (request.replacement) {
                const auto &replacement = *request.replacement;
                if (replacement.index >= bindings.size() || bindings[replacement.index] != replacement.expected ||
                    replacement.expected.kind != Input::BindingControlKind::GyroscopeAxis)
                    return Output::Failure(MakeError(InvalidGyroscopeDraft));
                return Output::Success(replacement.expected);
            }
            if (bindings.size() >= Input::MaximumAccessibilityBindingsPerAction)
                return Output::Failure(MakeError(InvalidGyroscopeDraft));
            return Output::Success(
                Input::InputBinding{.kind = Input::BindingControlKind::GyroscopeAxis, .gyroscope = {Input::SensorAxis::Y, 2.0F}});
        }

        /** @brief Exclusive host workflow with owned draft state and process-service borrows only. */
        class GyroscopeBindingModal final : public EditorModal {
        public:
            GyroscopeBindingModal(const EditorGuiContext &context, Input::InputRouter &router, GyroscopeBindingEditRequest request)
                : EditorModal(InputMappingPanel::kTextScope), context_(context), router_(router), request_(std::move(request)) {}

            ModalId Id() const override {
                return ModalId{0x4759524f42494e44ULL};
            }

            ModalPresentation Presentation() const override {
                return {.size = ModalSizePolicy::Medium};
            }

            ModalClosePolicy ClosePolicy() const override {
                return {};
            }

            CloseDecision CanClose(ModalCloseReason) override {
                return CloseDecision::Allow;
            }

            Result<void> OnOpen(EditorModalContext &) override {
                const auto baseline = ResolveBaseline(router_, request_);
                if (baseline.HasError())
                    return Result<void>::Failure(baseline.ErrorValue());
                baseline_ = baseline.Value();
                draft_ = GyroscopeBindingEditorDraft::FromBinding(baseline_);
                const auto actions = router_.Actions();
                const auto action = std::ranges::find(actions, request_.action, &Input::ActionDescriptor::id);
                if (action == actions.end())
                    return Result<void>::Failure(MakeError(InvalidGyroscopeDraft));
                twoDimensional_ = action->valueType == Input::ActionValueType::Axis2D;
                return Result<void>::Success();
            }

            ModalFrameResult Draw() override {
                const auto &fonts = context_.theme.fonts;
                const float footerHeight = 2 * DesignSystem::MetricsFor(Theme::GetActiveTokens(), Ui::ComponentSize::Medium).minimumHeight +
                                           ImGui::GetStyle().ItemSpacing.y + 2 * Theme::Layout::CardPad;
                Ui::ScopedModalShell shell({.id = "InputGyroscopeBinding",
                                            .title = Text("gyro.title").c_str(),
                                            .requestedSize = {520, 600},
                                            .minimumWidth = 240,
                                            .minimumHeight = 320,
                                            .footerHeight = footerHeight},
                                           fonts);
                if (!shell.IsOpen())
                    return ModalFrameResult::None();
                ImGui::BeginChild("##GyroscopeBody", {0, shell.BodyHeight()});
                ImGui::TextWrapped("%s", request_.action.Value().c_str());
                Ui::Hint(Text("gyro.hint").c_str(), fonts);
                const std::array axis{Text("gyro.axis_x").c_str(), Text("gyro.axis_y").c_str(), Text("gyro.axis_z").c_str()};
                ImGui::TextWrapped("%s", Text("gyro.axis").c_str());
                static_cast<void>(Ui::ComboControl("##GyroscopeAxis", &draft_.axis, axis.data(), static_cast<int>(axis.size()), fonts));
                if (twoDimensional_) {
                    const std::array components{Text("gyro.component_x").c_str(), Text("gyro.component_y").c_str()};
                    ImGui::TextWrapped("%s", Text("gyro.component").c_str());
                    static_cast<void>(Ui::ComboControl("##GyroscopeComponent", &draft_.component, components.data(),
                                                       static_cast<int>(components.size()), fonts));
                }
                ImGui::TextWrapped("%s", Text("gyro.calibration").c_str());
                static_cast<void>(
                    Ui::InputTextControl("##GyroscopeCalibration", draft_.calibration.data(), draft_.calibration.size(), fonts));
                ImGui::TextWrapped("%s", Text("gyro.sensitivity").c_str());
                static_cast<void>(
                    Ui::InputTextControl("##GyroscopeSensitivity", draft_.sensitivity.data(), draft_.sensitivity.size(), fonts));
                const bool valid = draft_.IsValid(baseline_);
                if (!valid)
                    Ui::ErrorText(Text("gyro.invalid").c_str(), fonts);
                if (failure_)
                    Ui::ErrorText(Text("gyro.apply_failed").c_str(), fonts);
                ImGui::EndChild();
                shell.BeginFooter({Theme::Layout::CardPad, Theme::Layout::CardPad});
                const bool cancel = Ui::Button({.label = Text("accessibility.cancel").c_str(),
                                                .variant = Ui::ButtonVariant::Secondary,
                                                .font = fonts.sans,
                                                .style = {.width = Ui::StyleWidth::FillAvailable}});
                const bool apply = Ui::Button({.label = Text("accessibility.apply").c_str(),
                                               .enabled = valid,
                                               .font = fonts.sans,
                                               .style = {.width = Ui::StyleWidth::FillAvailable}});
                shell.EndFooter();
                if (cancel || shell.CloseRequested())
                    return ModalFrameResult::RequestClose(ModalCloseReason::Cancelled);
                if (apply) {
                    const auto applied = ApplyGyroscopeBindingDraft(router_, request_, draft_);
                    if (applied.HasValue())
                        return ModalFrameResult::RequestClose(ModalCloseReason::Completed);
                    failure_ = applied.ErrorValue();
                }
                return ModalFrameResult::None();
            }

        private:
            const std::string &Text(const std::string_view key) const {
                return LocalizedText(context_.localization, key);
            }

            EditorGuiContext context_;
            Input::InputRouter &router_;
            GyroscopeBindingEditRequest request_;
            Input::InputBinding baseline_;
            GyroscopeBindingEditorDraft draft_;
            bool twoDimensional_{};
            std::optional<Error> failure_;
        };
    }  // namespace

    /** @copydoc GyroscopeBindingEditorDraft::FromBinding */
    GyroscopeBindingEditorDraft GyroscopeBindingEditorDraft::FromBinding(const Input::InputBinding &binding) noexcept {
        return {static_cast<int>(binding.gyroscope.axis), binding.component, FormatNumber(binding.gyroscope.radiansPerSecondAtFullScale),
                FormatNumber(binding.scale)};
    }

    /** @copydoc GyroscopeBindingEditorDraft::Evaluate */
    Result<Input::InputBinding> GyroscopeBindingEditorDraft::Evaluate(const Input::InputBinding &baseline) const {
        using Output = Result<Input::InputBinding>;
        const auto binding = ParseDraft(*this, baseline);
        return binding ? Output::Success(*binding) : Output::Failure(MakeError(InvalidGyroscopeDraft));
    }

    /** @copydoc GyroscopeBindingEditorDraft::IsValid */
    bool GyroscopeBindingEditorDraft::IsValid(const Input::InputBinding &baseline) const noexcept {
        return ParseDraft(*this, baseline).has_value();
    }

    /** @copydoc ApplyGyroscopeBindingDraft */
    Result<void> ApplyGyroscopeBindingDraft(Input::InputRouter &router, const GyroscopeBindingEditRequest &request,
                                            const GyroscopeBindingEditorDraft &draft) {
        const auto baseline = ResolveBaseline(router, request);
        if (baseline.HasError())
            return Result<void>::Failure(baseline.ErrorValue());
        auto desired = draft.Evaluate(baseline.Value());
        if (desired.HasError())
            return Result<void>::Failure(desired.ErrorValue());
        auto candidate = router.Profile();
        auto overridden = std::ranges::find(candidate.overrides, request.action, &Input::BindingOverride::action);
        if (overridden == candidate.overrides.end()) {
            const auto actions = router.Actions();
            const auto action = std::ranges::find(actions, request.action, &Input::ActionDescriptor::id);
            candidate.overrides.push_back({request.action, action->defaultBindings});
            overridden = std::prev(candidate.overrides.end());
        }
        if (request.replacement)
            overridden->bindings[request.replacement->index] = std::move(desired).Value();
        else
            overridden->bindings.push_back(std::move(desired).Value());
        return router.SetProfile(std::move(candidate));
    }

    /** @copydoc MakeGyroscopeBindingModal */
    std::unique_ptr<EditorModal> MakeGyroscopeBindingModal(const EditorGuiContext &context, Input::InputRouter &router,
                                                           GyroscopeBindingEditRequest request) {
        return std::make_unique<GyroscopeBindingModal>(context, router, std::move(request));
    }
}  // namespace Horo::Editor
