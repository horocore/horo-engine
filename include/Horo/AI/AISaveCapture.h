#pragma once

/** @file AISaveCapture.h @brief Owner-safe canonical AI participation in Runtime Save capture. */

#include "Horo/AI/AISceneActivation.h"
#include "Horo/Runtime/Save/SaveCaptureSnapshot.h"

#include <functional>
#include <memory>
#include <optional>

namespace Horo::AI {
    /** @brief An explicit host lookup for the current Scene, invoked only on the capture owner lane. */
    using AiSaveSceneSource = std::function<std::optional<Runtime::RuntimeSceneView>()>;

    /** @brief Descriptor and bound adapter to register together in one host Save composition. */
    struct AiSaveCaptureParticipant final {
        Runtime::CanonicalStateParticipantDescriptor descriptor;
        std::shared_ptr<const Runtime::ICanonicalStateAdapter> adapter;
    };

    /**
     * @brief Binds base-agent capture to an explicitly selected AI runtime and Scene source.
     * @param participant Stable semantic owner registered by the application.
     * @param record Exclusive canonical record identity.
     * @param maximumBytes Bound for detached AI element storage and for eager copying, at most the default Save copied-record limit.
     * @param runtime Weak borrow of the owner retired by the application.
     * @param scene Current Scene source. It must handle expiry of its host and retain no borrowed Scene.
     * @return Descriptor and capture adapter or a typed unbound or invalid admission failure.
     * @pre Called on the Scene owner lane.
     */
    [[nodiscard]] Result<AiSaveCaptureParticipant> BindAiSaveCapture(Runtime::SaveParticipantId participant, Runtime::SaveRecordId record,
                                                                     std::size_t maximumBytes, std::weak_ptr<const AiSceneRuntime> runtime,
                                                                     AiSaveSceneSource scene);
}  // namespace Horo::AI
