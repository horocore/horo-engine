#include "Horo/Runtime/Scene/IncrementalSceneCellCook.h"
#include "Horo/Runtime/Scene/RuntimeSceneCellLayers.h"
#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"
#include "Horo/Runtime/Scene/SceneCellAttachments.h"
#include "Horo/Runtime/Scene/SceneCellPayloadCodec.h"
#include "Horo/Runtime/Scene/WorldBuildValidation.h"

#include <type_traits>
static_assert(!std::is_copy_assignable_v<Horo::Runtime::RuntimeSceneCellPayload>);
static_assert(!std::is_move_assignable_v<Horo::Runtime::RuntimeSceneCellPayload>);

static_assert(!std::is_copy_constructible_v<Horo::Runtime::RuntimeSceneCellLayers>);
static_assert(!std::is_move_assignable_v<Horo::Runtime::RuntimeSceneCellLayers>);
static_assert(std::is_copy_constructible_v<Horo::Runtime::SceneCellAttachmentRequest>);
static_assert(std::is_same_v<decltype(&Horo::Runtime::QueueRuntimeSceneCellAttachment),
                             Horo::Result<void> (*)(Horo::Runtime::RuntimeSceneService &, const Horo::Runtime::RuntimeSceneCellPayload &,
                                                    const Horo::Runtime::SceneCellAttachmentRequest &,
                                                    std::vector<Horo::Runtime::RuntimeGroupAssetLease>,
                                                    std::shared_ptr<const Horo::Runtime::SceneCellPayloadAuthority>)>);

static_assert(std::is_same_v<decltype(&Horo::Runtime::FindRuntimeSceneCellAttachment),
                             std::optional<Horo::Runtime::SceneBaselineAttachmentView> (*)(Horo::Runtime::RuntimeSceneView,
                                                                                           const Horo::Runtime::SceneCellPayloadIdentity &,
                                                                                           Horo::WorldStreaming::PartitionEpoch) noexcept>);
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

static_assert(std::is_move_constructible_v<Horo::Runtime::WorldBuildValidationReport>);

static_assert(std::is_same_v<decltype(&Horo::Runtime::EncodeSceneCellPayload),
                             Horo::Result<std::vector<std::byte>> (*)(const Horo::Runtime::RuntimeSceneCellPayload &,
                                                                      const Horo::Runtime::SceneCellPayloadCodecLimits &,
                                                                      const Horo::CancellationToken &)>);

static_assert(std::is_same_v<decltype(&Horo::Runtime::DecodeSceneCellPayload),
                             Horo::Result<Horo::Runtime::RuntimeSceneCellPayload> (*)(
                                 const Horo::WorldStreaming::WorldPartitionDescriptor &, std::span<const std::byte>,
                                 const Horo::Runtime::SceneCellPayloadIdentity &, std::span<const Horo::Runtime::SceneCellComponentSchema>,
                                 std::span<const Horo::Runtime::SceneCellBehaviorSchema>,
                                 const Horo::Runtime::SceneCellPayloadCodecLimits &, const Horo::CancellationToken &)>);
static_assert(
    std::is_same_v<decltype(&Horo::Runtime::PreparedSceneCellCook::Adopt), void (Horo::Runtime::PreparedSceneCellCook::*)() noexcept>);
static_assert(std::is_invocable_v<decltype(&Horo::Runtime::PreparedSceneCellCook::Reserve), Horo::Runtime::PreparedSceneCellCook &,
                                  const Horo::CancellationToken &>);
static_assert(!std::is_invocable_v<decltype(&Horo::Runtime::PreparedSceneCellCook::Reserve), const Horo::Runtime::PreparedSceneCellCook &,
                                   const Horo::CancellationToken &>);
static_assert(!std::is_invocable_v<decltype(&Horo::Runtime::PreparedSceneCellCook::Adopt), const Horo::Runtime::PreparedSceneCellCook &>);

static_assert(Horo::Runtime::SceneCellPayloadCodecVersion == 2);
static_assert(Horo::Runtime::SceneStructuralParticipantContractVersion == 2);
static_assert(std::is_same_v<decltype(&Horo::Runtime::SceneStructuralParticipant::ValidateBaselines),
                             Horo::Result<void> (Horo::Runtime::SceneStructuralParticipant::*)(
                                 Horo::Runtime::RuntimeSceneView, std::span<const Horo::Runtime::SceneBaselinePreparationView>) const>);
