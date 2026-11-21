#pragma once

/** @file AISaveCapture.h @brief Owner-safe canonical AI participation in Runtime Save capture. */

#include "Horo/AI/AISceneActivation.h"
#include "Horo/Runtime/Save/SaveCaptureSnapshot.h"
#include "Horo/Runtime/Scene/PersistentEntityIdentity.h"

#include <functional>
#include <memory>
#include <optional>

namespace Horo::AI {
    /** @brief An explicit host lookup for the current Scene, invoked only on the capture owner lane. */
    using AiSaveSceneSource = std::function<std::optional<Runtime::RuntimeSceneView>()>;
    /** @brief Supplies the exact complete persistent identity map for the current Scene. */
    using AiSaveIdentitySource = std::function<Result<Runtime::PersistentEntityIdentityMap>(const Runtime::RuntimeSceneView &)>;

    /** @brief Descriptor and bound adapter to register together in one host Save composition. */
    struct AiSaveCaptureParticipant final {
        Runtime::CanonicalStateParticipantDescriptor descriptor;
        std::shared_ptr<const Runtime::ICanonicalStateAdapter> adapter;
    };

    /**
     * @brief Describes the inert Save capture record without binding any owner or invoking a Scene.
     * @param participant Stable semantic owner.
     * @param record Exclusive record identity.
     * @param maximumBytes Host-admitted bound for the record.
     * @param persistentReferences Enables the reference-capable wire schema.
     * @return Descriptor for the selected capture policy.
     */
    [[nodiscard]] Runtime::CanonicalStateParticipantDescriptor AiSaveCaptureDescriptor(Runtime::SaveParticipantId participant,
                                                                                       Runtime::SaveRecordId record,
                                                                                       std::size_t maximumBytes, bool persistentReferences);

    /**
     * @brief Binds base-agent capture to an explicitly selected AI runtime and Scene source.
     * @param participant Stable semantic owner registered by the application.
     * @param record Exclusive canonical record identity.
     * @param maximumBytes Bound for detached AI element storage and for eager copying, at most the default Save copied-record limit.
     * @param runtime Weak borrow of the owner retired by the application.
     * @param scene Current Scene source. It must handle expiry of its host and retain no borrowed Scene.
     * @param identities Optional authoritative persistent identity source. Enables portable entity references when installed.
     * @return Descriptor and capture adapter or a typed unbound or invalid admission failure.
     * @pre Called on the Scene owner lane.
     */
    [[nodiscard]] Result<AiSaveCaptureParticipant> BindAiSaveCapture(Runtime::SaveParticipantId participant, Runtime::SaveRecordId record,
                                                                     std::size_t maximumBytes, std::weak_ptr<const AiSceneRuntime> runtime,
                                                                     AiSaveSceneSource scene, AiSaveIdentitySource identities = {});

    /**
     * @brief Resolves a verified AI record against the exact reconstructed Scene's persistent identities.
     * @param bytes Complete AI participant record.
     * @param maximumBytes Admitted record bound.
     * @param scene Exact current destination candidate view.
     * @param identities Its complete persistent entity map.
     * @return Owned canonical population or a typed wire, entity, or lifecycle failure.
     */
    [[nodiscard]] Result<AiCanonicalState> DecodeAiSavePopulation(std::span<const std::uint8_t> bytes, std::size_t maximumBytes,
                                                                  const Runtime::RuntimeSceneView &scene,
                                                                  const Runtime::PersistentEntityIdentityMap &identities);
}  // namespace Horo::AI
