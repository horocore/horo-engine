#include "Horo/Runtime/Scene/IncrementalSceneCellCook.h"
#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"
#include "Horo/Runtime/Scene/SceneCellAttachments.h"

#include <type_traits>
static_assert(!std::is_copy_assignable_v<Horo::Runtime::RuntimeSceneCellPayload>);
static_assert(!std::is_move_assignable_v<Horo::Runtime::RuntimeSceneCellPayload>);

static_assert(!std::is_copy_constructible_v<Horo::Runtime::IncrementalSceneCellCook>);
static_assert(std::is_base_of_v<Horo::Runtime::SceneActivationParticipant, Horo::Runtime::SceneCellAttachmentParticipant>);

namespace {
    template <typename Participant>
    concept CanBypassAttachmentAdmission = requires { Participant({}, nullptr); };
    static_assert(!CanBypassAttachmentAdmission<Horo::Runtime::SceneCellAttachmentParticipant>);
    static_assert(std::is_same_v<decltype(&Horo::Runtime::SceneCellAttachmentParticipant::RequestCancellation),
                                 void (Horo::Runtime::SceneCellAttachmentParticipant::*)() const noexcept>);
    static_assert(std::is_same_v<decltype(&Horo::Runtime::SceneCellAttachmentParticipant::Shutdown),
                                 void (Horo::Runtime::SceneCellAttachmentParticipant::*)() const noexcept>);
}  // namespace
