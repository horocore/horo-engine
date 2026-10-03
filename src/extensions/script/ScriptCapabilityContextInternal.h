#pragma once
#include "Horo/Extensions/ScriptCapabilityContext.h"

namespace Horo::Extensions {
    struct ScriptCapabilityContextState final {
        std::shared_ptr<ScriptInvocationRegistry> registry;
        ScriptInvocationContextRegistration registration;
        ScriptCapabilityContextDescriptor descriptor;
        CancellationToken parentCancellation;
        std::atomic<bool> active{true};
        ScriptCapabilityContextState(std::shared_ptr<ScriptInvocationRegistry> registryIn,
                                     ScriptInvocationContextRegistration registrationIn, ScriptCapabilityContextDescriptor descriptorIn,
                                     CancellationToken parent);
        [[nodiscard]] bool IsUsable() const noexcept;
    };

    struct ScriptCapabilityBindingState final {
        std::shared_ptr<ScriptCapabilityContextState> context;
        std::size_t importIndex{};
        [[nodiscard]] bool MatchesProvider(const std::shared_ptr<ScriptInvocationProviderState> &provider) const noexcept;
        [[nodiscard]] bool IsUsable() const noexcept;
    };
}  // namespace Horo::Extensions
