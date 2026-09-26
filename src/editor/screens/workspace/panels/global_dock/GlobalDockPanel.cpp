#include "editor/screens/workspace/panels/global_dock/GlobalDockPanel.h"

#include "Horo/Editor/EditorIcons.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserPane.h"
#include "editor/screens/workspace/panels/global_dock/panes/audio/GlobalDockAudioPane.h"
#include "editor/screens/workspace/panels/global_dock/panes/build_output/GlobalDockBuildOutputPane.h"
#include "editor/screens/workspace/panels/global_dock/panes/console/GlobalDockConsolePane.h"
#include "editor/screens/workspace/panels/global_dock/panes/mcp/GlobalDockMcpPane.h"
#include "editor/screens/workspace/panels/global_dock/panes/network/GlobalDockNetworkPane.h"
#include "editor/screens/workspace/panels/global_dock/panes/operations/GlobalDockOperationsPane.h"
#include "editor/screens/workspace/panels/global_dock/panes/performance/GlobalDockPerformancePane.h"
#include "editor/screens/workspace/panels/global_dock/panes/physics/GlobalDockPhysicsPane.h"

#include <algorithm>
#include <array>
#include <memory>
#include <ranges>
#include <string_view>
#include <utility>

namespace Horo::Editor {
    namespace {
        constexpr float TabHeight = 36.0F;

        struct BuiltInPaneMetadata {
            std::string_view id;
            std::string_view labelKey;
        };

        [[nodiscard]] constexpr BuiltInPaneMetadata PaneMetadata(const GlobalDockTab tab) noexcept {
            using enum GlobalDockTab;
            switch (tab) {
                case Assets:
                    return {"horo.global_dock.assets", "workspace.global_dock.tab.assets"};
                case Console:
                    return {"horo.global_dock.console", "workspace.global_dock.tab.console"};
                case BuildOutput:
                    return {"horo.global_dock.build_output", "workspace.global_dock.tab.build_output"};
                case Operations:
                    return {"horo.global_dock.operations", "workspace.global_dock.tab.operations"};
                case Mcp:
                    return {"horo.global_dock.mcp", "workspace.global_dock.tab.mcp"};
                case Performance:
                    return {"horo.global_dock.performance", "workspace.global_dock.tab.performance"};
                case Physics:
                    return {"horo.global_dock.physics", "workspace.global_dock.tab.physics"};
                case Audio:
                    return {"horo.global_dock.audio", "workspace.global_dock.tab.audio"};
                case Network:
                    return {"horo.global_dock.network", "workspace.global_dock.tab.network"};
            }
            return {};
        }

        [[nodiscard]] constexpr std::string_view PaneId(const GlobalDockTab tab) noexcept {
            return PaneMetadata(tab).id;
        }

        class BuiltInPaneAdapter : public IGlobalDockPane {
        public:
            explicit BuiltInPaneAdapter(const GlobalDockTab tab) : tab_{tab} {}

            [[nodiscard]] std::string_view Id() const noexcept override {
                return PaneMetadata(tab_).id;
            }

            [[nodiscard]] std::string_view LabelKey() const noexcept override {
                return PaneMetadata(tab_).labelKey;
            }

        private:
            GlobalDockTab tab_;
        };

        template <typename Pane> class PaneAdapter final : public BuiltInPaneAdapter {
        public:
            using BuiltInPaneAdapter::BuiltInPaneAdapter;

            void Draw(const GlobalDockPaneDrawContext &context) override {
                pane_.Draw(context.contentOrigin, context.contentWidth, context.gui);
            }

        private:
            Pane pane_;
        };

        class AssetPaneAdapter final : public BuiltInPaneAdapter {
        public:
            using BuiltInPaneAdapter::BuiltInPaneAdapter;

            void Attach(PanelContext &context) override {
                pane_.Attach(context.guiRenderer);
            }

            void Detach() override {
                pane_.Detach();
            }

            void Draw(const GlobalDockPaneDrawContext &context) override {
                pane_.Draw(context.contentOrigin, context.contentWidth, context.viewModel, context.command, context.gui);
            }

        private:
            AssetBrowserPane pane_;
        };

        class ConsolePaneAdapter final : public BuiltInPaneAdapter {
        public:
            using BuiltInPaneAdapter::BuiltInPaneAdapter;

            void Attach(PanelContext &context) override {
                pane_.Attach(context.logQuery);
            }

            void Detach() override {
                pane_.Detach();
            }

            void Draw(const GlobalDockPaneDrawContext &context) override {
                pane_.Draw(context.contentOrigin, context.contentWidth, context.gui);
            }

        private:
            GlobalDockConsolePane pane_;
        };

        class BuildOutputPaneAdapter final : public BuiltInPaneAdapter {
        public:
            using BuiltInPaneAdapter::BuiltInPaneAdapter;

            void Attach(PanelContext &context) override {
                pane_.Attach(context.buildOutputQuery, context.gameplayBuilds, context.projectRoot);
            }

            void Detach() override {
                pane_.Detach();
            }

            void Draw(const GlobalDockPaneDrawContext &context) override {
                pane_.Draw(context.contentOrigin, context.contentWidth, context.command, context.gui);
            }

        private:
            GlobalDockBuildOutputPane pane_;
        };

        class OperationsPaneAdapter final : public BuiltInPaneAdapter {
        public:
            using BuiltInPaneAdapter::BuiltInPaneAdapter;

            void Attach(PanelContext &context) override {
                pane_.Attach(context.operationQuery, context.operationControl);
            }

            void Detach() override {
                pane_.Detach();
            }

            void Draw(const GlobalDockPaneDrawContext &context) override {
                pane_.Draw(context.contentOrigin, context.contentWidth, context.gui);
            }

        private:
            GlobalDockOperationsPane pane_;
        };
    }  // namespace

    GlobalDockPanel::GlobalDockPanel(const GlobalDockTab activeTab) : activeTab_{activeTab}, activePaneId_{PaneId(activeTab)} {
        RegisterBuiltInPanes();
    }

    bool GlobalDockPanel::RegisterPane(std::unique_ptr<IGlobalDockPane> pane) {
        if (attached_ || pane == nullptr || pane->Id().empty())
            return false;
        if (std::ranges::any_of(panes_, [&pane](const RegisteredPane &entry) {
            return entry.pane->Id() == pane->Id();
        }))
            return false;
        panes_.push_back({.pane = std::move(pane)});
        return true;
    }

    bool GlobalDockPanel::ActivatePane(const std::string_view paneId) {
        const auto selected = std::ranges::find_if(panes_, [paneId](const RegisteredPane &entry) {
            return entry.pane->Id() == paneId;
        });
        if (selected == panes_.end())
            return false;
        activePaneId_ = selected->pane->Id();
        if (selected->builtIn)
            activeTab_ = selected->builtInTab;
        return true;
    }

    template <typename Adapter> void GlobalDockPanel::RegisterBuiltInPane(const GlobalDockTab tab) {
        panes_.push_back({.pane = std::make_unique<Adapter>(tab), .builtInTab = tab, .builtIn = true});
    }

    void GlobalDockPanel::RegisterBuiltInPanes() {
        using enum GlobalDockTab;
        RegisterBuiltInPane<AssetPaneAdapter>(Assets);
        RegisterBuiltInPane<ConsolePaneAdapter>(Console);
        RegisterBuiltInPane<BuildOutputPaneAdapter>(BuildOutput);
        RegisterBuiltInPane<OperationsPaneAdapter>(Operations);
        RegisterBuiltInPane<PaneAdapter<GlobalDockMcpPane>>(Mcp);
        RegisterBuiltInPane<PaneAdapter<GlobalDockPerformancePane>>(Performance);
        RegisterBuiltInPane<PaneAdapter<GlobalDockPhysicsPane>>(Physics);
        RegisterBuiltInPane<PaneAdapter<GlobalDockAudioPane>>(Audio);
        RegisterBuiltInPane<PaneAdapter<GlobalDockNetworkPane>>(Network);
    }

    /** @copydoc GlobalDockPanel::DrawIcon */
    void GlobalDockPanel::DrawIcon(ImDrawList *drawList, const ImVec2 &position, const ImVec2 &size, const ImU32 color) {
        const float glyphSize = std::min(size.x, size.y) * 0.72F;
        const float x = position.x + (size.x - glyphSize) * 0.5F;
        const float y = position.y + (size.y - glyphSize) * 0.5F;
        const float stroke = std::max(1.0F, glyphSize * 0.085F);
        drawList->AddRect({x, y}, {x + glyphSize, y + glyphSize}, color, glyphSize * 0.08F, 0, stroke);
        drawList->AddLine({x + glyphSize * 0.2F, y + glyphSize * 0.34F}, {x + glyphSize * 0.38F, y + glyphSize * 0.5F}, color, stroke);
        drawList->AddLine({x + glyphSize * 0.38F, y + glyphSize * 0.5F}, {x + glyphSize * 0.2F, y + glyphSize * 0.66F}, color, stroke);
        drawList->AddLine({x + glyphSize * 0.5F, y + glyphSize * 0.66F}, {x + glyphSize * 0.78F, y + glyphSize * 0.66F}, color, stroke);
    }

    /** @copydoc GlobalDockPanel::DrawPanel */
    void GlobalDockPanel::DrawPanel(const ImVec2 &position, const ImVec2 &size, const EditorWorkspaceViewModel &viewModel,
                                    EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        static_cast<void>(position);
        std::vector<const char *> tabNames;
        tabNames.reserve(panes_.size());
        for (const RegisteredPane &entry : panes_)
            tabNames.push_back(context.localization.Get("editor", entry.pane->LabelKey()).c_str());
        const auto active = std::ranges::find_if(panes_, [this](const RegisteredPane &entry) {
            return entry.pane->Id() == activePaneId_;
        });
        int activeIndex = active == panes_.end() ? 0 : static_cast<int>(std::distance(panes_.begin(), active));
        const ImVec2 tabBarPosition = ImGui::GetCursorScreenPos();
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(tabBarPosition, {tabBarPosition.x + size.x, tabBarPosition.y + TabHeight},
                                Theme::U32(Theme::Mix(Theme::Bg0(), Theme::Bg1(), 0.18F)));
        drawList->AddLine({tabBarPosition.x, tabBarPosition.y + TabHeight - 1.0F},
                          {tabBarPosition.x + size.x, tabBarPosition.y + TabHeight - 1.0F}, Theme::U32(Theme::BorderStrong()));
        ImGui::SetCursorScreenPos({tabBarPosition.x + 14.0F, tabBarPosition.y});
        if (const int selectedIndex = Ui::DrawDockTabs(tabNames, activeIndex, context.theme.fonts, TabHeight, Ui::DockTabStyle::GlobalDock);
            selectedIndex >= 0 && selectedIndex < static_cast<int>(panes_.size())) {
            static_cast<void>(ActivatePane(panes_[static_cast<std::size_t>(selectedIndex)].pane->Id()));
            activeIndex = selectedIndex;
        }
        ImGui::SetCursorScreenPos({tabBarPosition.x, tabBarPosition.y + TabHeight});

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
        ImGui::BeginChild("##Content", {size.x, std::max(1.0F, size.y - TabHeight)}, false, ImGuiWindowFlags_NoSavedSettings);
        const ImVec2 contentOrigin = ImGui::GetCursorScreenPos();
        const float contentWidth = std::max(1.0F, ImGui::GetContentRegionAvail().x);

        if (!panes_.empty()) {
            const auto selectedIndex = static_cast<std::size_t>(std::clamp(activeIndex, 0, static_cast<int>(panes_.size() - 1U)));
            panes_[selectedIndex].pane->Draw(
                {.contentOrigin = contentOrigin, .contentWidth = contentWidth, .viewModel = viewModel, .command = command, .gui = context});
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    /** @copydoc GlobalDockPanel::OnAttach */
    void GlobalDockPanel::OnAttach(PanelContext &context) {
        for (RegisteredPane &entry : panes_)
            entry.pane->Attach(context);
        attached_ = true;
    }

    /** @copydoc GlobalDockPanel::OnDetach */
    void GlobalDockPanel::OnDetach() {
        for (RegisteredPane &entry : panes_)
            entry.pane->Detach();
        attached_ = false;
    }
}  // namespace Horo::Editor
