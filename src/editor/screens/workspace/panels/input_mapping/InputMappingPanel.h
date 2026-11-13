#pragma once

#include "Horo/Editor/IWorkspacePanel.h"

#include <array>
#include <optional>
#include <span>

namespace Horo::Editor {
    /** @brief Editable exact millisecond fields for one binding; invalid intermediate text is retained until explicitly applied. */
    struct BindingAccessibilityEditorDraft final {
        bool toggle{}, stickyModifiers{}, repeat{};
        std::array<char, 12> holdMilliseconds{};
        std::array<char, 12> repeatDelayMilliseconds{};
        std::array<char, 12> repeatIntervalMilliseconds{};

        /** @brief Copies policy into exact decimal milliseconds without losing microsecond precision.
         * @param policy Current validated binding policy. @return Independent editable draft.
         */
        [[nodiscard]] static BindingAccessibilityEditorDraft FromPolicy(const Input::BindingAccessibilityPolicy &policy) noexcept;
        /** @brief Parses entire millisecond fields, accepting a dot or comma and at most three fractional digits.
         * @return Validated typed policy, or InvalidPolicy without changing the router or substituting a previous valid value.
         */
        [[nodiscard]] Result<Input::BindingAccessibilityPolicy> Evaluate() const;
    };

    /** @brief Applies a draft through the existing complete-profile validator, without adding a persistence owner.
     * @param router Existing authoritative action/profile owner. @param action Exact typed action identity.
     * @param bindingIndex Selected effective binding. @param expectedRevision Exact router configuration used to create this draft.
     * @param expected Original binding used to create this draft.
     * @param draft Editable desired policy. @return Original validation error or stale-draft failure; rejection leaves the profile
     * unchanged.
     */
    [[nodiscard]] Result<void> ApplyBindingAccessibilityDraft(Input::InputRouter &router, const Input::ActionId &action,
                                                              std::size_t bindingIndex, std::uint64_t expectedRevision,
                                                              const Input::InputBinding &expected,
                                                              const BindingAccessibilityEditorDraft &draft);

    /** @brief Explicit user intent from the inline binding draft; editing alone does not mutate the input owner. */
    enum class BindingAccessibilityDraftCommand : std::uint8_t {
        None,
        Apply,
        Cancel
    };

    /** @brief Draws the shared-controls draft with localized units and complete invalid intermediate text.
     * @param draft Mutable property-edit draft. @param context Current frame's localization, theme and typography.
     * @return Explicit apply/cancel intent; invalid fields disable Apply. No profile mutation or persistence occurs here.
     * @pre Caller supplies the owning action/binding ImGui ID scope when rendering multiple or replacement drafts.
     */
    [[nodiscard]] BindingAccessibilityDraftCommand DrawBindingAccessibilityDraft(BindingAccessibilityEditorDraft &draft,
                                                                                 const EditorGuiContext &context);

    /** @brief Workspace surface for action maps, live devices, profile persistence, and typed rebinding. */
    class InputMappingPanel final : public IWorkspacePanel {
    public:
        /** @brief Catalog scope shared by this surface and its view helpers. */
        static constexpr GuiLocalizationScope kTextScope{"workspace.input_mapping"};

        /** @brief Creates a surface with its required localization scope. */
        InputMappingPanel() : IWorkspacePanel(kTextScope) {}

        [[nodiscard]] std::string GetId() const override {
            return "horo.input_mapping";
        }

        [[nodiscard]] std::string GetDisplayName() const override {
            return "workspace.input_mapping.title";
        }

        [[nodiscard]] WorkspaceDockArea GetDefaultDockArea() const override {
            return WorkspaceDockArea::Right;
        }

        [[nodiscard]] std::vector<std::string> GetObservedEventTypes() const override {
            return {};
        }

        void OnAttach(PanelContext &context) override;
        void OnDetach() override;
        void DrawIcon(ImDrawList *drawList, const ImVec2 &position, const ImVec2 &size, ImU32 color) override;
        void DrawPanel(const ImVec2 &position, const ImVec2 &size, const EditorWorkspaceViewModel &viewModel,
                       EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) override;

    private:
        enum class Page : std::uint8_t {
            Actions,
            Devices,
            Profiles
        };

        void DrawActions(const EditorGuiContext &context, EditorWorkspaceViewCommandData &command);
        void DrawActionValidation(std::span<const Input::ActionDescriptor> actions) const;
        void DrawActionTable(std::span<const Input::ActionDescriptor> actions, const EditorGuiContext &context);
        void DrawSelectedAction(const Input::ActionDescriptor &action, const EditorGuiContext &context,
                                EditorWorkspaceViewCommandData &command);
        /** @brief Draws an explicitly selected binding's draft and commits only through the canonical profile owner. */
        void DrawAccessibilityControls(const Input::ActionDescriptor &action, const EditorGuiContext &context);
        void DrawDevices(const EditorGuiContext &context);
        void DrawProfiles(const EditorGuiContext &context);
        void PollBindingCapture(const EditorGuiContext &context);
        [[nodiscard]] Result<Input::InputBindingProfile> LoadComposedProfile() const;
        [[nodiscard]] std::filesystem::path EditorProfilePath() const;
        [[nodiscard]] std::optional<std::filesystem::path> ProjectUserProfilePath() const;

        Input::InputRouter *router_{nullptr};
        Page page_{Page::Actions};
        std::size_t selectedAction_{0};
        std::optional<Input::ActionId> listeningAction_;
        Input::InputContextToken bindingCaptureContext_;
        std::optional<Input::ActionId> accessibilityAction_;
        std::size_t accessibilityBinding_{};
        std::uint64_t accessibilityConfigurationRevision_{};
        Input::InputBinding accessibilityBaseline_;
        BindingAccessibilityEditorDraft accessibilityDraft_;
        std::string statusMessage_;
        std::string projectRootText_;
        std::filesystem::path projectRoot_;
    };
}  // namespace Horo::Editor
