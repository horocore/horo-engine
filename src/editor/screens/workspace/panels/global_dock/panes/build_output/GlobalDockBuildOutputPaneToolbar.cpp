#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneChrome.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneLayout.h"
#include "editor/screens/workspace/panels/global_dock/panes/build_output/GlobalDockBuildOutputPane.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] const char *ActiveBuildStateKey(const Application::GameplayBuildState state) noexcept {
            using enum Application::GameplayBuildState;
            switch (state) {
                case Queued:
                    return "workspace.global_dock.build_output.active_state.queued";
                case AcquiringLock:
                    return "workspace.global_dock.build_output.active_state.acquiring_lock";
                case WaitingForExternalBuild:
                    return "workspace.global_dock.build_output.active_state.waiting";
                case Configuring:
                    return "workspace.global_dock.build_output.active_state.configuring";
                case Building:
                    return "workspace.global_dock.build_output.active_state.building";
                case Validating:
                    return "workspace.global_dock.build_output.active_state.validating";
                case Succeeded:
                case Failed:
                case Cancelled:
                case TimedOut:
                    return "workspace.global_dock.build_output.active_state.queued";
            }
            return "workspace.global_dock.build_output.active_state.queued";
        }

    }  // namespace

    void GlobalDockBuildOutputPane::DrawActiveBuild(const Application::GameplayBuildSnapshot &snapshot,
                                                    const GlobalDockPaneRegions &regions, const GlobalDockPaneMetrics &metrics,
                                                    const EditorGuiContext &context, const float height) const {
        const GlobalDockToolbarChipProps cancel{
            .id = "BuildCancelActive",
            .label = context.localization.Get("editor", snapshot.cancellationRequested ? "workspace.global_dock.build_output.cancelling"
                                                                                       : "workspace.global_dock.build_output.cancel"),
            .tone = GlobalDockTone::Warning,
            .disabled = snapshot.cancellationRequested,
        };
        const float y = regions.contentOrigin.y;
        const float buttonWidth = std::min(MeasureGlobalDockToolbarChip(cancel, context.theme.fonts),
                                           std::max(1.0F, regions.contentWidth - 2.0F * metrics.contentPadding));
        const float buttonX = regions.contentOrigin.x + regions.contentWidth - metrics.contentPadding - buttonWidth;
        const float controlY = y + (height - metrics.controlHeight) * 0.5F;
        DrawGlobalDockToolbarSurface({regions.contentOrigin.x, y}, regions.contentWidth, height);

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - snapshot.startedAt);
        const std::string progress =
            snapshot.progress.has_value() ? std::format(" · {}%", static_cast<int>(*snapshot.progress * 100.0F)) : std::string{};
        const std::string label =
            std::format("{} · {}{} · {}:{:02d}", context.localization.Get("editor", "workspace.global_dock.build_output.active"),
                        context.localization.Get("editor", ActiveBuildStateKey(snapshot.state)), progress, elapsed.count() / 60,
                        elapsed.count() % 60);
        const float textY = y + (height - Theme::TextPx::Label()) * 0.5F;
        DrawGlobalDockClippedText(*ImGui::GetWindowDrawList(), context.theme.fonts.sansCompact, Theme::TextPx::Label(),
                                  {regions.contentOrigin.x + metrics.contentPadding, textY}, {buttonX - metrics.toolbarGap, y + height},
                                  Theme::Text(), label);

        if (DrawGlobalDockToolbarChip({buttonX, controlY}, buttonWidth, cancel, context.theme.fonts))
            static_cast<void>(m_gameplayBuilds->RequestCancel(snapshot.id));
    }

}  // namespace Horo::Editor
