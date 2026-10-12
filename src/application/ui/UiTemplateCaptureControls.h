#pragma once

#include "Horo/Packages/PackageRestore.h"

namespace Horo::Application::Detail {
    /** @brief Exact bounded control bytes and graph publication observed under the shared project lease. */
    struct UiTemplateCaptureControls final {
        std::optional<std::string> intent;
        std::optional<std::string> lock;
        std::shared_ptr<const Packages::PackageRestoreGraph> graph;
    };

    /** @brief Reads regular controls without following symlinks; absent files remain explicit. */
    [[nodiscard]] Result<UiTemplateCaptureControls> ReadTemplateControls(const std::filesystem::path &root,
                                                                         std::shared_ptr<const Packages::PackageRestoreGraph> graph);
    /** @brief Uses existing Packages intent/lock/archive verification, retaining the exact published graph. */
    [[nodiscard]] Result<std::string> ValidateTemplatePackages(const UiTemplateCaptureControls &controls, bool requirePublisher);
    /** @brief Preserves typed read failures; only readable control or graph publication drift reports stale. */
    [[nodiscard]] Result<void> VerifyTemplateControls(const std::filesystem::path &root, const UiTemplateCaptureControls &captured,
                                                      std::shared_ptr<const Packages::PackageRestoreGraph> currentGraph);
}  // namespace Horo::Application::Detail
