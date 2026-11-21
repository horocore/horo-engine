#pragma once
/** @file OpenXRPackageProbe.h @brief Terminal command for supervised installed Editor OpenXR probing. */
#include <iosfwd>
#include <span>

namespace Horo::Application::Internal {
    /** @brief Runs the explicit packaged Desktop probe and prints a single redacted receipt on success.
     * @param arguments Bounded invocation after the command name.
     * @param output Terminal destination for exact normalized evidence.
     * @param errors Terminal destination for actionable redacted failures.
     * @return Standard process exit status. Success requires the verified child to retire cleanly.
     */
    int RunOpenXRPackageProbe(std::span<char *> arguments, std::ostream &output, std::ostream &errors);
}  // namespace Horo::Application::Internal
