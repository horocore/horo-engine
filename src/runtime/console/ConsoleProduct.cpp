#include "Horo/Debug/ConsoleProduct.h"

#include "ConsoleProductInternal.h"

namespace Horo::Debug {
    /** @copydoc CompiledConsoleProfile */
    ProductProfile CompiledConsoleProfile() noexcept {
#if defined(HORO_PROFILE_EDITOR)
        return ProductProfile::Editor;
#elif defined(HORO_PROFILE_DEVELOPMENT)
        return ProductProfile::Development;
#elif defined(HORO_PROFILE_PROFILE)
        return ProductProfile::Profile;
#elif defined(HORO_PROFILE_SHIPPING)
        return ProductProfile::Shipping;
#elif defined(HORO_PROFILE_DIAGNOSTICS)
        return ProductProfile::Diagnostics;
#else
        return ProductProfile::Server;
#endif
    }

    /** @copydoc ProductConsoleDescriptors */
    std::vector<DebugCommandDescriptor> ProductConsoleDescriptors() {
        const auto core = CoreConsoleDescriptors();
        std::vector<DebugCommandDescriptor> descriptors(core.begin(), core.end());
#if !defined(HORO_PROFILE_SERVER)
        descriptors.push_back(Detail::ScreenshotDescriptor());
#endif
#if defined(HORO_DEBUG_COMMANDS_ENABLED)
        const auto diagnostic = Detail::DiagnosticDescriptors();
        descriptors.insert(descriptors.end(), diagnostic.begin(), diagnostic.end());
#endif
#if defined(HORO_PROFILE_EDITOR) || defined(HORO_PROFILE_DEVELOPMENT)
        const auto development = Detail::DevelopmentDescriptors();
        descriptors.insert(descriptors.end(), development.begin(), development.end());
#endif
#if defined(HORO_PROFILE_EDITOR) || defined(HORO_PROFILE_DEVELOPMENT) || defined(HORO_PROFILE_DIAGNOSTICS)
        descriptors.push_back(Detail::SupportDescriptor());
#endif
        return descriptors;
    }

    /** @copydoc CreateProductConsoleRegistry */
    Result<CommandRegistry> CreateProductConsoleRegistry(const RegistryPolicy &policy) {
        if (policy.profile != CompiledConsoleProfile())
            return Result<CommandRegistry>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        return CommandRegistry::Create(ProductConsoleDescriptors(), policy);
    }

    /** @copydoc ProductConsoleVariableDescriptors */
    std::vector<ConsoleVariableDescriptor> ProductConsoleVariableDescriptors() {
        std::vector<ConsoleVariableDescriptor> descriptors;
        Detail::AppendPublicVariables(descriptors);
#if defined(HORO_DEBUG_COMMANDS_ENABLED)
        Detail::AppendDiagnosticVariables(descriptors);
#endif
#if defined(HORO_PROFILE_EDITOR) || defined(HORO_PROFILE_DEVELOPMENT)
        Detail::AppendDevelopmentVariables(descriptors);
#endif
        return descriptors;
    }
}  // namespace Horo::Debug
