#pragma once
/** @file CliHost.h @brief Private production terminal host composition. */
#include "Horo/Application/HostObservability.h"

#include <optional>
#include <ostream>
#include <span>
#include <string_view>

namespace Horo::Application::Internal {
    /** @brief Executes real CLI commands with exclusive protocol stdout ownership.
     * @param arguments Arguments after the executable name.
     * @param identity Host-owned safe build identity.
     * @param output Result stream. @param diagnostics Human/log diagnostic stream.
     * @param terminal Whether the human presentation stream is a terminal.
     * @param admissionFailure Original typed native argument-conversion failure, before parser admission.
     * @return Stable CLI exit category.
     */
    [[nodiscard]] int RunCli(std::span<const std::string_view> arguments, HostObservabilityIdentity identity, std::ostream &output,
                             std::ostream &diagnostics, bool terminal, std::optional<Error> admissionFailure = {});
}  // namespace Horo::Application::Internal
