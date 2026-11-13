#pragma once

/** @file ConsoleProduct.h @brief Compile-selected default console metadata; one product profile per artifact. */
#include "Horo/Debug/ConsoleBuiltins.h"
#include "Horo/Debug/ConsoleConfiguration.h"
#include "Horo/Debug/ConsoleVariable.h"

#if (defined(HORO_PROFILE_EDITOR) + defined(HORO_PROFILE_DEVELOPMENT) + defined(HORO_PROFILE_PROFILE) + defined(HORO_PROFILE_SHIPPING) +   \
     defined(HORO_PROFILE_DIAGNOSTICS) + defined(HORO_PROFILE_SERVER)) != 1
#error "Console product requires exactly one HORO_PROFILE_* definition from its owning CMake target."
#endif

namespace Horo::Debug {
    /** @brief Stable minimum-set command identities for explicit owning-capability bindings. */
    inline constexpr DebugCommandId ScreenshotCommandId = 0xDB600005;
    inline constexpr DebugCommandId LogLevelCommandId = 0xDB600006;
    inline constexpr DebugCommandId ProfileCaptureCommandId = 0xDB600008;
    inline constexpr DebugCommandId SupportBundleCommandId = 0xDB60000B;
    /** @brief Returns this separate build artifact's fixed identity. @return Selected product profile. */
    [[nodiscard]] ProductProfile CompiledConsoleProfile() noexcept;
    /** @brief Returns only the selected artifact's compiled default metadata, without activating any capability.
     * @return Owned finite descriptor list. Shipping still requires the project's exact Public allowlist at registry composition.
     * Hosts must bind real owning capabilities for every admitted descriptor; no placeholder handlers are provided.
     */
    [[nodiscard]] std::vector<DebugCommandDescriptor> ProductConsoleDescriptors();
    /** @brief Composes the catalog only under this artifact's exact compiled profile.
     * @param policy Host allowlists and budgets; a mismatched profile fails rather than changing the artifact identity.
     * @return Owned registry with the compiled catalog and project allowlist gates, or typed composition failure.
     */
    [[nodiscard]] Result<CommandRegistry> CreateProductConsoleRegistry(const RegistryPolicy &policy);
    /** @brief Returns profile-compiled inert variable schemas, not active feature bindings.
     * @return Finite metadata for explicit host composition; host consumers must use the resulting descriptor-owned state.
     * Category-specific log variables remain contributions of the actual logging policy owner.
     */
    [[nodiscard]] std::vector<ConsoleVariableDescriptor> ProductConsoleVariableDescriptors();
}  // namespace Horo::Debug
