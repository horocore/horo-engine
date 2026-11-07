#pragma once
/** @file AuthenticatedShaderCookConfiguration.h @brief Backend-neutral authenticated offline tool identity and publication documents. */
#include "VerifiedShaderHostCatalog.h"

#include <array>
#include <filesystem>

namespace Horo::Application::Internal {
    struct AuthenticatedShaderCookConfiguration final {
        Render::ShaderCompilerToolchainConfiguration configuration;
        std::shared_ptr<const std::vector<std::uint8_t>> catalogBytes, signatureBytes;
    };

    /** @brief Actual host inputs, independent trust, and exactly DXC/validator/Cross paths; no Apple selection occurs. */
    struct AuthenticatedSpirvCookInputs final {
        std::filesystem::path scratchRoot, catalog, detachedSignature, hostTrust;
        std::array<std::filesystem::path, 3> executables;
    };
    enum class ShaderCookToolProfile {
        Spirv,
        Metal
    };

    struct ShaderCookAuthenticationFiles final {
        std::filesystem::path scratchRoot, catalog, detachedSignature, hostTrust;
    };

    [[nodiscard]] std::string_view ActualShaderCookHost() noexcept;
    [[nodiscard]] Result<std::vector<std::uint8_t>> ReadShaderCookInput(const std::filesystem::path &, std::size_t maximum);
    /** @brief Verifies the immutable catalog signature using independent active trust before installing a closed tool profile. */
    [[nodiscard]] Result<AuthenticatedShaderCookConfiguration> MakeAuthenticatedShaderCookConfiguration(
        const ShaderCookAuthenticationFiles &, std::span<const std::filesystem::path>, ShaderCookToolProfile);
    [[nodiscard]] Result<AuthenticatedShaderCookConfiguration> MakeAuthenticatedSpirvCookConfiguration(
        const AuthenticatedSpirvCookInputs &);
}  // namespace Horo::Application::Internal
