#pragma once
/** @file XRSimulatorPanel.h @brief Localized explicit synthetic control surface for the existing canonical Play owner. */
#include "Horo/Editor/IWorkspacePanel.h"
#include "Horo/Editor/XRSimulatorModel.h"

namespace Horo::Editor {
    class XRSimulatorPanel final : public IWorkspacePanel {
    public:
        static constexpr GuiLocalizationScope kTextScope{"workspace.xr_simulator"};

        XRSimulatorPanel() : IWorkspacePanel(kTextScope) {}

        [[nodiscard]] std::string GetId() const override {
            return "horo.xr_simulator";
        }

        [[nodiscard]] std::string GetDisplayName() const override {
            return "workspace.xr_simulator.title";
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
        void ResultMessage(const Result<void> &result);
        void DrawPlay(const XRSimulatorView &snapshot, const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                      const EditorGuiContext &context);
        [[nodiscard]] bool DrawSource(XR::XRSimulatorStep &controls, std::size_t index, const EditorGuiContext &context);
        void DrawHands(const XRSimulatorView &snapshot, const EditorGuiContext &context);
        void DrawControls(const XRSimulatorView &snapshot, const EditorGuiContext &context);
        void DrawEvents(const XRSimulatorView &snapshot, const EditorGuiContext &context);
        void DrawArtifacts(const XRSimulatorView &snapshot, const EditorGuiContext &context);
        void DrawCapture(const XRSimulatorView &snapshot, const EditorGuiContext &context);
        void Apply(const XRSimulatorView &snapshot, XRSimulatorCommandKind kind);
        [[nodiscard]] bool Button(const EditorGuiContext &context, const char *key) const;
        XRSimulatorModel *model_{};
        std::weak_ptr<IXRSimulatorArtifacts> artifacts_;
        std::array<char, 256> importPath_{};
        std::array<char, 96> captureName_{'c', 'a', 'p', 't', 'u', 'r', 'e', '.', 'j', 's', 'o', 'n'};
        std::string status_;
        int selectedJoint_{};
    };
}  // namespace Horo::Editor
