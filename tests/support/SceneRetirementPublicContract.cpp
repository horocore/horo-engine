#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <memory>
#include <type_traits>

using Participant = Horo::Runtime::SceneActivationParticipant;
using RetirementSignature =
    Horo::Result<std::unique_ptr<Horo::Runtime::SceneActivationCandidate>> (Participant::*)(const Horo::Runtime::RuntimeSceneView &);
static_assert(Horo::Runtime::SceneActivationParticipantContractVersion == 4);
static_assert(std::is_same_v<decltype(&Participant::PrepareRetirement), RetirementSignature>);
