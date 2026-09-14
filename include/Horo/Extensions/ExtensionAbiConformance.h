#pragma once

/**
 * @file ExtensionAbiConformance.h
 * @brief Author-facing native extension ABI conformance execution contract.
 */

#include "Horo/Extensions/ExtensionAbi.h"

#include <cstdint>
#include <string_view>

namespace Horo::Extensions {
    /** @brief Stable terminal outcome produced by one isolated ABI conformance run. */
    enum class ExtensionAbiConformanceCode : std::uint8_t {
        Passed,
        LibraryLoadFailed,
        MissingLoadEntryPoint,
        NegotiationRejected,
        RegistrationRejected,
        LoadRejected,
        ModuleTableRejected,
        ModuleIdentityRejected,
        CleanupRejected,
    };

    /** @brief Deterministic evidence from loading and shutting down one native extension module. */
    struct ExtensionAbiConformanceReport {
        ExtensionAbiConformanceCode code{ExtensionAbiConformanceCode::LibraryLoadFailed}; /**< Terminal conformance outcome. */
        HoroExtensionStatus status{HORO_EXTENSION_SUCCESS};                               /**< Module or host ABI status, when present. */
        std::uint32_t registrationCount{};                                                /**< Accepted host callback registrations. */
        bool queryPresent{};                                                              /**< Whether the module exports ABI query. */
        bool unloadPresent{};                                                             /**< Whether the module exports unload. */
        bool unloadInvoked{};                                                             /**< Whether shutdown invoked unload. */

        /** @brief Returns whether every exercised ABI and cleanup contract passed. */
        [[nodiscard]] bool Passed() const noexcept;
    };

    /**
     * @brief Load, negotiate, exercise, and shut down one native extension module in the current process.
     * @param modulePath Native module path compatible with the current host platform and architecture.
     * @return Stable conformance evidence; failures never publish registrations outside the invocation.
     * @warning Native code can terminate or corrupt its hosting process; run this author tool only on trusted build output.
     */
    [[nodiscard]] ExtensionAbiConformanceReport RunExtensionAbiConformance(std::string_view modulePath);

    /**
     * @brief Return the stable diagnostic code for a conformance outcome.
     * @param code Outcome to translate.
     * @return Process-lifetime diagnostic identifier.
     */
    [[nodiscard]] std::string_view ExtensionAbiConformanceCodeName(ExtensionAbiConformanceCode code) noexcept;
}  // namespace Horo::Extensions
