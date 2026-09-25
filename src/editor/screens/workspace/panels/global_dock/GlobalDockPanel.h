#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/IGlobalDockPane.h"
#include "Horo/Editor/IWorkspacePanel.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Editor {
    /** @brief Canonical tabs hosted by the embedded global dock surface. */
    enum class GlobalDockTab : std::uint8_t {
        Assets,
        Console,
        BuildOutput,
        Operations,
        Mcp,
        Performance,
        Physics,
        Audio,
        Network,
    };

    inline constexpr std::array kDefaultGlobalDockTabs{
        GlobalDockTab::Assets,      GlobalDockTab::Console, GlobalDockTab::BuildOutput, GlobalDockTab::Operations, GlobalDockTab::Mcp,
        GlobalDockTab::Performance, GlobalDockTab::Physics, GlobalDockTab::Audio,       GlobalDockTab::Network,
    };

    /** @brief Returns the stable default global-dock tab order. */
    [[nodiscard]] constexpr const auto &DefaultGlobalDockTabs() noexcept {
        return kDefaultGlobalDockTabs;
    }

    class GlobalDockPanel final : public IWorkspacePanel {
    public:
        /** @brief Creates the global dock with an optional restored active tab. */
        explicit GlobalDockPanel(GlobalDockTab activeTab = GlobalDockTab::Assets);

        [[nodiscard]] std::string GetId() const override {
            return "horo.global_dock";
        }

        [[nodiscard]] std::string GetDisplayName() const override {
            return "workspace.panel.global_dock";
        }

        [[nodiscard]] WorkspaceDockArea GetDefaultDockArea() const override {
            return WorkspaceDockArea::Bottom;
        }

        [[nodiscard]] std::vector<std::string> GetObservedEventTypes() const override {
            return {};
        }

        void OnAttach(PanelContext &ctx) override;

        void OnDetach() override;

        void DrawIcon(ImDrawList *dl, const ImVec2 &pos, const ImVec2 &size, ImU32 color) override;

        void DrawPanel(const ImVec2 &pos, const ImVec2 &size, const EditorWorkspaceViewModel &vm, EditorWorkspaceViewCommandData &cmd,
                       const EditorGuiContext &ctx) override;

        /** @brief Returns the currently selected embedded global-dock tab. */
        [[nodiscard]] GlobalDockTab ActiveTab() const noexcept {
            return activeTab_;
        }

        /** @brief Returns the stable identity of the selected built-in or module-provided pane. */
        [[nodiscard]] std::string_view ActivePaneId() const noexcept {
            return activePaneId_;
        }

        /**
         * @brief Registers one pane before the panel is attached.
         * @param pane Owned pane implementation with a non-empty, unique identity.
         * @return True when the pane was accepted.
         */
        [[nodiscard]] bool RegisterPane(std::unique_ptr<IGlobalDockPane> pane);

        /** @brief Selects a registered pane by stable identity. */
        [[nodiscard]] bool ActivatePane(std::string_view paneId);

    private:
        struct RegisteredPane {
            std::unique_ptr<IGlobalDockPane> pane;
            GlobalDockTab builtInTab;
            bool builtIn{false};
        };

        template <typename Adapter> void RegisterBuiltInPane(GlobalDockTab tab);
        void RegisterBuiltInPanes();

        GlobalDockTab activeTab_{GlobalDockTab::Assets};
        std::string activePaneId_;
        std::vector<RegisteredPane> panes_;
        bool attached_{false};
    };
}  // namespace Horo::Editor
