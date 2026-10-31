#pragma once

#include "Horo/Editor/IWorkspacePanel.h"

#include <optional>
#include <span>

namespace Horo::Editor {
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

        void DrawActions(const EditorGuiContext &context);
        void DrawActionValidation(std::span<const Input::ActionDescriptor> actions) const;
        void DrawActionTable(std::span<const Input::ActionDescriptor> actions, const EditorGuiContext &context);
        void DrawSelectedAction(const Input::ActionDescriptor &action, const EditorGuiContext &context);
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
        std::string statusMessage_;
        std::filesystem::path projectRoot_;
    };
}  // namespace Horo::Editor
