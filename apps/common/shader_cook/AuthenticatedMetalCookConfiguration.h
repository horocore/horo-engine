#pragma once
/** @file AuthenticatedMetalCookConfiguration.h @brief Explicit authenticated offline Metal toolchain host configuration. */
#include "VerifiedShaderHostCatalog.h"

#include <array>
#include <filesystem>

namespace Horo::Application::Internal {
    /** @brief Independent host inputs; tool paths never derive from catalog or project data. */
    struct AuthenticatedMetalCookInputs final {
        std::filesystem::path scratchRoot, catalog, detachedSignature, hostTrust;
        std::array<std::filesystem::path, 4> executables; /**< DXC, SPIRV validator, Cross, Apple xcrun, in exact order. */
    };

    /** @brief Tool configuration and immutable authenticated documents; independent trust roots are deliberately excluded. */
    struct AuthenticatedMetalCookConfiguration final {
        Render::ShaderCompilerToolchainConfiguration configuration;
        std::shared_ptr<const std::vector<std::uint8_t>> catalogBytes, signatureBytes;
    };

    [[nodiscard]] std::string_view ActualMetalCookHost() noexcept;
    /** @brief Reads one explicitly named absolute regular input with a finite bound and exact final length. */
    [[nodiscard]] Result<std::vector<std::uint8_t>> ReadShaderCookInput(const std::filesystem::path &path, std::size_t maximum);
    /** @brief Reuses existing signature verification and independently supplied active publisher roots.
     * @details No discovery, observed-digest approval, process invocation or scratch/output mutation occurs. */
    [[nodiscard]] Result<AuthenticatedMetalCookConfiguration> MakeAuthenticatedMetalCookConfiguration(
        const AuthenticatedMetalCookInputs &inputs);
}  // namespace Horo::Application::Internal
