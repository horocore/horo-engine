#pragma once
/** @file NativeNavigationBakeComposition.h
 * @brief Non-installed executable composition shared by editor, CLI and MCP.
 */
#include "Horo/Application/NavigationBakeApplication.h"

#include <atomic>

namespace Horo::Application::Internal {
    /** @brief Captures authorized canonical project source with explicit provider/package admission.
     * @param files Retained native root. @param compatibility Actual project admission. @param closed Owned revocation view.
     * @param invocation Original cancellation and deadline. @return Canonical source or original typed failure.
     */
    [[nodiscard]] Result<std::shared_ptr<const NavigationProjectBakeSource>> CaptureNativeNavigationBakeSource(
        std::shared_ptr<const Platform::IProjectReadFiles> files, const ProjectCompatibilitySnapshot &compatibility,
        std::shared_ptr<std::atomic<bool>> closed, const NavigationBakeInvocation &invocation);
    /** @brief Composes the shared native target/cache/provider without starting work; tooling/background I/O only.
     * @param root Exact authorized root. @param definition Explicit definition. @param compatibility Project admission.
     * @param closed Owned revocation view. @param deadline Maximum invocation deadline.
     * @return Validated host config; application construction still runs on its owning lane.
     */
    [[nodiscard]] Result<NavigationBakeApplicationConfig> ComposeNativeNavigationBake(
        const std::filesystem::path &root, Assets::AssetId definition, ProjectCompatibilitySnapshot compatibility,
        std::shared_ptr<std::atomic<bool>> closed,
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max());
}  // namespace Horo::Application::Internal
