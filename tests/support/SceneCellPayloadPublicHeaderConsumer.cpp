#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"

#include <type_traits>
static_assert(!std::is_copy_assignable_v<Horo::Runtime::RuntimeSceneCellPayload>);
static_assert(!std::is_move_assignable_v<Horo::Runtime::RuntimeSceneCellPayload>);

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
