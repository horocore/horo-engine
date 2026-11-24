#pragma once

#include <string>

namespace Horo::Editor {
    enum class GameplayBehaviorKind {
        Lua,
        Native,
    };

    struct CreateGameplayBehaviorRequest {
        std::string destination;
        std::string baseName;
        GameplayBehaviorKind kind;
    };
}  // namespace Horo::Editor