#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <memory>
#include <type_traits>

using Participant = Horo::Runtime::SceneActivationParticipant;
using RetirementSignature =
    Horo::Result<std::unique_ptr<Horo::Runtime::SceneActivationCandidate>> (Participant::*)(const Horo::Runtime::RuntimeSceneView &);
static_assert(Horo::Runtime::SceneActivationParticipantContractVersion == 4);
static_assert(std::is_same_v<decltype(&Participant::PrepareRetirement), RetirementSignature>);

using PreparationSignature = Horo::Result<std::unique_ptr<Horo::Runtime::SceneActivationCandidate>> (Participant::*)(
    const Horo::Runtime::RuntimeSceneDefinition &, const Horo::Runtime::RuntimeSceneView &);
static_assert(std::is_same_v<decltype(&Participant::Prepare), PreparationSignature>);
using StructuralParticipant = Horo::Runtime::SceneStructuralParticipant;
using StructuralSignature = Horo::Result<std::unique_ptr<Horo::Runtime::SceneStructuralCandidate>> (StructuralParticipant::*)(
    const Horo::Runtime::RuntimeSceneView &, std::span<const Horo::Runtime::RuntimeEntityView>, std::span<const Horo::Runtime::EntityRef>);
static_assert(std::is_same_v<decltype(&StructuralParticipant::Prepare), StructuralSignature>);
