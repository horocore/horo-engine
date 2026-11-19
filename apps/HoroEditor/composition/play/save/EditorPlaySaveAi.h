#pragma once

/** @file EditorPlaySaveAi.h @brief Explicit Play capture binding and candidate-only AI restore handoff. */

#include "Horo/AI/AISaveCapture.h"
#include "Horo/Runtime/Save/SaveArchiveReader.h"

namespace Horo::Application::Internal {
    /** @brief Binds one Play Scene owner to AI across Save capture and detached Scene preparation. */
    class PlaySaveAiServices final {
    public:
        /** @brief Selects owners which outlive the active Play Save session. */
        PlaySaveAiServices(Runtime::RuntimeSceneService &scenes, std::shared_ptr<AI::AiSceneRuntime> runtime) noexcept;
        /** @brief Declares the current inert AI record schema used by the Play sandbox. */
        [[nodiscard]] Runtime::CanonicalStateParticipantDescriptor Descriptor() const;
        /** @brief Provides the capture-only binding; Scene activation owns restore publication. */
        [[nodiscard]] Result<AI::AiSaveCaptureParticipant> BindCapture(AI::AiSaveIdentitySource identities) const;
        /** @brief Stages decoded values once for the actual private Scene assembled by Save. */
        [[nodiscard]] Result<void> Stage(Runtime::RuntimeSceneView scene, std::span<const std::byte> record,
                                         const Runtime::PersistentEntityIdentityMap &identities, CancellationToken cancellation);
        /** @brief Takes saved state for the exact staged Scene, or selects ordinary activation when none is staged. */
        [[nodiscard]] Result<AI::AiSceneRestoredPopulation> Take(const Runtime::RuntimeSceneView &scene);
        /** @brief Rejects publication if the staged population was never taken by the exact Scene participant. */
        [[nodiscard]] Result<void> ValidateConsumption(const Runtime::RuntimeSceneView &scene) const;
        /** @brief Drops a staged source on rollback before the AI participant took it. */
        void Cancel(const Runtime::RuntimeSceneView &scene) noexcept;

    private:
        Runtime::RuntimeSceneService &scenes_;
        std::shared_ptr<AI::AiSceneRuntime> runtime_;
        Runtime::RuntimeSceneView scene_;
        AI::AiSceneRestoredPopulation pending_;
    };

    /** @brief Reads the exact optional Play AI record and checks its immutable manifest ownership. */
    [[nodiscard]] Result<std::optional<std::vector<std::byte>>> ReadPlaySaveAi(
        const Runtime::ValidatedSaveArchive &archive, const std::optional<Runtime::CanonicalStateParticipantDescriptor> &descriptor);
}  // namespace Horo::Application::Internal
