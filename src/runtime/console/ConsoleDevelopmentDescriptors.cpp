#include "ConsoleProductInternal.h"
#include "Horo/Debug/ConsoleProduct.h"

#if !defined(HORO_PROFILE_EDITOR) && !defined(HORO_PROFILE_DEVELOPMENT)
#error "DevelopmentOnly console translation units require Editor or Development."
#endif

namespace Horo::Debug::Detail {
    namespace {
        constexpr DebugArgumentDescriptor InspectArguments[]{
            {"entity", "Generation-checked runtime entity identity", DebugArgumentType::EntityId}};
        constexpr DebugCommandDescriptor Descriptors[]{{SceneTreeCommandId,
                                                        "game.scene_tree",
                                                        "Inspect the runtime scene hierarchy",
                                                        "game.scene_tree",
                                                        {},
                                                        CommandPermission::Developer,
                                                        CommandThreadPolicy::OwnerThreadNextFrame,
                                                        CommandAvailability::DevelopmentOnly},
                                                       {InspectCommandId, "game.inspect", "Inspect safe public entity state",
                                                        "game.inspect <scene:index:generation>", InspectArguments,
                                                        CommandPermission::Developer, CommandThreadPolicy::OwnerThreadNextFrame,
                                                        CommandAvailability::DevelopmentOnly}};
    }  // namespace

    /** @copydoc DevelopmentDescriptors */
    std::span<const DebugCommandDescriptor> DevelopmentDescriptors() noexcept {
        return Descriptors;
    }

    /** @copydoc AppendDevelopmentVariables */
    void AppendDevelopmentVariables(std::vector<ConsoleVariableDescriptor> &descriptors) {
        const auto append = [&](std::uint64_t id, std::string name) {
            ConsoleVariableDescriptor variable{id, std::move(name), false};
            variable.availability = CommandAvailability::DevelopmentOnly;
            variable.applyPolicy = ConsoleVariableApplyPolicy::NextFrame;
            descriptors.push_back(std::move(variable));
        };
        append(0xDB62000D, "overlay.scene.visible");
        append(0xDB62000E, "debug.draw.physics");
        append(0xDB62000F, "renderer.debug.wireframe");
    }
}  // namespace Horo::Debug::Detail
