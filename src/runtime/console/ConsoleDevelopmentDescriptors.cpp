#include "ConsoleProductInternal.h"
#include "Horo/Debug/ConsoleProduct.h"

#if !defined(HORO_PROFILE_EDITOR) && !defined(HORO_PROFILE_DEVELOPMENT)
#error "DevelopmentOnly console translation units require Editor or Development."
#endif

namespace Horo::Debug::Detail {
    /** @copydoc DevelopmentDescriptors */
    std::span<const DebugCommandDescriptor> DevelopmentDescriptors() noexcept {
        return ConsoleSceneDescriptors();
    }

    /** @copydoc AppendDevelopmentVariables */
    void AppendDevelopmentVariables(std::vector<ConsoleVariableDescriptor> &descriptors) {
        const auto append = [&](std::uint64_t id, std::string name) {
            ConsoleVariableDescriptor variable{id, std::move(name), false};
            variable.availability = CommandAvailability::DevelopmentOnly;
            variable.applyPolicy = ConsoleVariableApplyPolicy::NextFrame;
            descriptors.push_back(std::move(variable));
        };
        descriptors.push_back(ConsoleSceneOverlayVariableDescriptor());
        append(0xDB62000E, "debug.draw.physics");
        append(0xDB62000F, "renderer.debug.wireframe");
    }
}  // namespace Horo::Debug::Detail
