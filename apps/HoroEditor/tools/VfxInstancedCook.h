#pragma once
/** @file VfxInstancedCook.h @brief Authenticated offline cooking of the indexed forward VFX shader family. */
#include "AuthenticatedShaderCookConfiguration.h"

#include <filesystem>
#include <span>

namespace Horo::Editor::Offline {
    /** @brief Closed native output routes; neither value selects a runtime backend. */
    enum class VfxInstancedCookBackend {
        Metal, /**< Authenticated DXC/validator/Cross/Xcrun Metal library route. */
        Vulkan /**< Authenticated DXC/validator SPIR-V route with Cross reflection. */
    };

    /** @brief Compiles ordinary and masked variants together and validates written bytes before exclusive atomic publication.
     * @param backend Explicit closed output route.
     * @param repositoryRoot Absolute repository root containing the canonical VFX instance shader.
     * @param destination Absolute absent package directory; existing destinations remain unchanged.
     * @param authentication Explicit catalog, signature, independent trust and existing scratch directory.
     * @param executables Exactly DXC, validator, Cross, and additionally Xcrun for Metal, all absolute authenticated paths.
     * @return Success only after complete compilation and validated publication, otherwise the preserved typed failure.
     */
    [[nodiscard]] Result<void> CookVfxInstancedFamily(VfxInstancedCookBackend backend, const std::filesystem::path &repositoryRoot,
                                                      const std::filesystem::path &destination,
                                                      const Application::Internal::ShaderCookAuthenticationFiles &authentication,
                                                      std::span<const std::filesystem::path> executables);
}  // namespace Horo::Editor::Offline
