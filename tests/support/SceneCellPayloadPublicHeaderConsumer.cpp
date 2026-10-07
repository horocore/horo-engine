#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"
#include "Horo/Runtime/Scene/SceneCellAttachments.h"

#include <type_traits>
static_assert(!std::is_copy_assignable_v<Horo::Runtime::RuntimeSceneCellPayload>);
static_assert(!std::is_move_assignable_v<Horo::Runtime::RuntimeSceneCellPayload>);

static_assert(std::is_base_of_v<Horo::Runtime::SceneActivationParticipant, Horo::Runtime::SceneCellAttachmentParticipant>);
