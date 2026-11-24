#pragma once

#include "Horo/Foundation/Result.h"
#include "editor/screens/workspace/gameplay/GameplayBehaviorRequest.h"

namespace Horo::Editor {

    [[nodiscard]] Result<void> ValidateCreateGameplayBehaviorRequest(const CreateGameplayBehaviorRequest &request);
}