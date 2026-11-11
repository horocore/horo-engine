#pragma once

#include "Horo/Debug/CommandRegistry.h"
#include "Horo/Debug/ConsoleVariable.h"

namespace Horo::Debug::Detail {
    /** @brief Public screenshot metadata, compiled independently of diagnostic translation units. */
    [[nodiscard]] DebugCommandDescriptor ScreenshotDescriptor() noexcept;
    /** @brief Public local-terminal variable metadata, independent of diagnostic translation units. */
    void AppendPublicVariables(std::vector<ConsoleVariableDescriptor> &descriptors);
#if defined(HORO_DEBUG_COMMANDS_ENABLED)
    /** @brief NonShipping and exact-profile metadata, never linked into Shipping artifacts. */
    [[nodiscard]] std::span<const DebugCommandDescriptor> DiagnosticDescriptors() noexcept;
    /** @brief Adds only NonShipping variable schemas for the selected non-shipping artifact. */
    void AppendDiagnosticVariables(std::vector<ConsoleVariableDescriptor> &descriptors);
#endif
#if defined(HORO_PROFILE_EDITOR) || defined(HORO_PROFILE_DEVELOPMENT)
    /** @brief DevelopmentOnly metadata from separately composed translation units. */
    [[nodiscard]] std::span<const DebugCommandDescriptor> DevelopmentDescriptors() noexcept;
    /** @brief Adds DevelopmentOnly variable schemas from the development-only translation unit. */
    void AppendDevelopmentVariables(std::vector<ConsoleVariableDescriptor> &descriptors);
#endif
#if defined(HORO_PROFILE_EDITOR) || defined(HORO_PROFILE_DEVELOPMENT) || defined(HORO_PROFILE_DIAGNOSTICS)
    /** @brief DiagnosticsOnly support metadata; never part of Profile, Server or Shipping. */
    [[nodiscard]] DebugCommandDescriptor SupportDescriptor() noexcept;
#endif
}  // namespace Horo::Debug::Detail
