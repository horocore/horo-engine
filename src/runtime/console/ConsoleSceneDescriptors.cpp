#include "Horo/Debug/ConsoleBuiltins.h"

namespace Horo::Debug {
    /** @copydoc ConsoleSceneDescriptors */
    std::span<const DebugCommandDescriptor> ConsoleSceneDescriptors() noexcept {
        static constexpr DebugArgumentDescriptor arguments[]{
            {"entity", "Generation-checked runtime entity identity", DebugArgumentType::EntityId}};
        static constexpr DebugCommandDescriptor descriptors[]{{SceneTreeCommandId,
                                                               "game.scene_tree",
                                                               "Inspect the runtime scene hierarchy",
                                                               "game.scene_tree",
                                                               {},
                                                               CommandPermission::Developer,
                                                               CommandThreadPolicy::OwnerThreadNextFrame,
                                                               CommandAvailability::DevelopmentOnly},
                                                              {InspectCommandId, "game.inspect", "Inspect safe public entity state",
                                                               "game.inspect <scene:index:generation>", arguments,
                                                               CommandPermission::Developer, CommandThreadPolicy::OwnerThreadNextFrame,
                                                               CommandAvailability::DevelopmentOnly}};
        return descriptors;
    }
}  // namespace Horo::Debug
