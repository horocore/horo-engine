#include "AuthenticatedMetalCookConfiguration.h"

namespace Horo::Application::Internal {
    std::string_view ActualMetalCookHost() noexcept {
        return ActualShaderCookHost();
    }

    Result<AuthenticatedMetalCookConfiguration> MakeAuthenticatedMetalCookConfiguration(const AuthenticatedMetalCookInputs &inputs) {
        return MakeAuthenticatedShaderCookConfiguration({inputs.scratchRoot, inputs.catalog, inputs.detachedSignature, inputs.hostTrust},
                                                        inputs.executables, ShaderCookToolProfile::Metal);
    }
}  // namespace Horo::Application::Internal
