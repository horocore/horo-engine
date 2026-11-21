#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "Horo/Runtime/Scene/SceneInstances.h"

#include <memory>
#include <type_traits>

using Participant = Horo::Runtime::SceneActivationParticipant;
using RetirementSignature =
    Horo::Result<std::unique_ptr<Horo::Runtime::SceneActivationCandidate>> (Participant::*)(const Horo::Runtime::RuntimeSceneView &);
static_assert(Horo::Runtime::SceneActivationParticipantContractVersion == 5);
static_assert(std::is_same_v<decltype(&Participant::PrepareRetirement), RetirementSignature>);

using PreparationSignature = Horo::Result<std::unique_ptr<Horo::Runtime::SceneActivationCandidate>> (Participant::*)(
    const Horo::Runtime::RuntimeSceneDefinition &, const Horo::Runtime::RuntimeSceneView &);
static_assert(std::is_same_v<decltype(&Participant::Prepare), PreparationSignature>);
using StructuralParticipant = Horo::Runtime::SceneStructuralParticipant;
using StructuralSignature = Horo::Result<std::unique_ptr<Horo::Runtime::SceneStructuralCandidate>> (StructuralParticipant::*)(
    const Horo::Runtime::RuntimeSceneView &, std::span<const Horo::Runtime::RuntimeEntityView>, std::span<const Horo::Runtime::EntityRef>);
static_assert(std::is_same_v<decltype(&StructuralParticipant::Prepare), StructuralSignature>);

using SceneService = Horo::Runtime::RuntimeSceneService;
using ReplicaTransformCommit = Horo::Result<void> (SceneService::*)(const Horo::Runtime::RuntimeSceneView &,
                                                                    const Horo::Runtime::SceneCommandBuffer &,
                                                                    const Horo::Runtime::RuntimeDispatchEvidence &);
static_assert(std::is_same_v<decltype(&SceneService::CommitReplicaTransformBatch), ReplicaTransformCommit>);

using InstanceOperation = Horo::Runtime::SceneInstanceOperation;
static_assert(!std::is_default_constructible_v<InstanceOperation>);
static_assert(std::is_copy_constructible_v<InstanceOperation>);

using ActivatedOperation = Horo::Runtime::ActivatedSceneInstanceOperation;
static_assert(!std::is_default_constructible_v<ActivatedOperation>);
static_assert(std::is_copy_constructible_v<ActivatedOperation>);
static_assert(std::is_same_v<decltype(&ActivatedOperation::Snapshot),
                             Horo::Result<Horo::Runtime::ScenePublicationSnapshot> (ActivatedOperation::*)() const>);
