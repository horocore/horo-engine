#pragma once

#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Editor/EditorStatusBarModel.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/design_system/components/EditorBusyIndicator.h"

#include <algorithm>
#include <cmath>

namespace Horo::Editor {
    inline constexpr const char *BackgroundBuildStatusId = "horo.status.build";
    inline constexpr const char *BackgroundBuildActionId = "horo.build.show";

    /** @brief Closed lifecycle vocabulary shared by the modal and background status provider. */
    inline bool BuildIsRunning(const Application::GameplayBuildState state) {
        using enum Application::GameplayBuildState;
        return state != Succeeded && state != Failed && state != Cancelled && state != TimedOut;
    }

    inline const char *BuildStatusKey(const Application::GameplayBuildState state) {
        constexpr const char *keys[]{"workspace.build_release.queued",    "workspace.build_release.locking",
                                     "workspace.build_release.waiting",   "workspace.build_release.configuring",
                                     "workspace.build_release.building",  "workspace.build_release.validating",
                                     "workspace.build_release.succeeded", "workspace.build_release.failed",
                                     "workspace.build_release.cancelled", "workspace.build_release.timed_out"};
        return keys[static_cast<std::size_t>(state)];
    }

    /** @brief Projects only authoritative active work; closing the modal never owns/cancels that work. */
    inline EditorStatusItemContent BackgroundBuildStatus(const Application::GameplayBuildSnapshot *build,
                                                         const ILocalizationService &localization, const bool modalVisible) {
        EditorStatusItemContent content{.available = false};
        if (!build || modalVisible || !BuildIsRunning(build->state))
            return content;
        content.iconResourceId = Ui::BusyStatusIcon;
        content.label = localization.Get("editor", BuildStatusKey(build->state));
        content.tone = EditorStatusItemTone::Accent;
        content.available = true;
        if (build->progress && std::isfinite(*build->progress))
            content.value = std::to_string(static_cast<int>(std::clamp(*build->progress, 0.0F, 1.0F) * 100)) + "%";
        return content;
    }
}  // namespace Horo::Editor
