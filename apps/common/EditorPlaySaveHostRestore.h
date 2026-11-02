#pragma once

/** @file EditorPlaySaveHostRestore.h
 * @brief Actual detached slot read and same-producer Scene restore handoff.
 */
#include "EditorPlaySaveHostStorage.h"
#include "Horo/Gameplay/BehaviorRuntimeRestore.h"
#include "Horo/Runtime/Save/RuntimeSaveSession.h"
#include "Horo/Runtime/Scene/SceneRestoreBundle.h"

#include <mutex>

namespace Horo::Application::Internal {
    /** @brief One exact load's worker-owned slot/codec/module inputs; publishes no mutable world state. */
    class PlaySaveRestoreSource final : public Runtime::IRuntimeSaveRestoreSource {
    public:
        /** @brief Captures owned immutable source inputs; storage selection retains exact catalog/generation consent. */
        PlaySaveRestoreSource(std::shared_ptr<PlaySaveStorage> storage, std::shared_ptr<const PlaySaveStorageSchema> schema,
                              Runtime::SaveManagerCommand selection,
                              std::shared_ptr<const std::vector<Editor::PlaySaveGameplayBinding>> liveBindings,
                              std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> behaviorOwner,
                              std::shared_ptr<Gameplay::BehaviorLifecyclePersistenceAdapter> lifecycle);
        /** @copydoc Runtime::IRuntimeSaveRestoreSource::Stage */
        [[nodiscard]] Result<Runtime::RuntimeSaveRestoreStaging> Stage(const Runtime::StagedRestoreContext &context,
                                                                       const Runtime::SaveParticipantRegistrySnapshot &participants,
                                                                       const CancellationToken &cancellation) override;
        /** @brief Stages actual candidate-bound native receipts on the owner lane
         * after the real Scene/prehook preparation. */
        [[nodiscard]] Result<Runtime::RuntimeSaveRestoreStaging> StageOwners(
            const Runtime::SaveParticipantRegistrySnapshot &participants,
            std::shared_ptr<Gameplay::BehaviorRuntimeRestoreCandidate> candidate,
            const Runtime::PersistentEntityIdentityMap &identities) const;
        /** @brief Borrows only the canonical Scene-owned runtime binding for this
         * exact accepted operation. */
        [[nodiscard]] std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> BehaviorOwner() const noexcept;
        /** @brief Returns only actual fully decoded immutable Scene state after worker settlement. */
        [[nodiscard]] std::shared_ptr<const PlaySaveSceneState> PreparedScene() const;

    private:
        std::shared_ptr<PlaySaveStorage> storage_;
        std::shared_ptr<const PlaySaveStorageSchema> schema_;
        Runtime::SaveManagerCommand selection_;
        std::shared_ptr<const std::vector<Editor::PlaySaveGameplayBinding>> liveBindings_;
        std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> behaviorOwner_;
        std::shared_ptr<Gameplay::BehaviorLifecyclePersistenceAdapter> lifecycle_;
        std::shared_ptr<const Runtime::ValidatedSaveArchive> archive_;
        mutable std::mutex publication_; /**< Worker publishes one complete immutable result; owner only reads after job settlement. */
        std::shared_ptr<const PlaySaveSceneState> decoded_;
        bool attempted_{};
    };

    /** @brief Scene-owned publication evidence survives controller/world observation without retaining a live UI pointer. */
    struct PlaySaveSceneHandoffState final {
        Runtime::SaveOperationHandle operation;
        bool transferred{};
        bool published{};
        bool retired{};
    };

    /** @brief Transfers the original session packet into the existing SceneRestoreBundle and its real atomic Scene publication.
     * @details Owner calls only. Retain the containing Play Scene host through IsSettled; destructor never calls a Scene service.
     * The generation authority is an actual host-owned object, not a copied source-context echo. Unsupported external resolvers
     * fail before taking the producer. Cancel requests the original operation; Scene pumping performs actual rollback/retirement.
     */
    class PlaySaveSceneHandoff final : public Runtime::IRuntimeSaveRestoreHandoff {
    public:
        /** @brief Binds the actual Scene owner, exact worker source and current generation authority without queuing any work. */
        PlaySaveSceneHandoff(Runtime::RuntimeSceneService &scenes, std::shared_ptr<PlaySaveRestoreSource> source,
                             std::shared_ptr<const Runtime::ISceneRestoreGenerationAuthority> authority,
                             std::shared_ptr<PlaySaveSceneHandoffState> state, Runtime::SaveWorldId world);
        /** @copydoc Runtime::IRuntimeSaveRestoreHandoff::Accept */
        [[nodiscard]] Result<void> Accept(Runtime::RuntimeSavePreparedRestore &prepared) override;
        /** @copydoc Runtime::IRuntimeSaveRestoreHandoff::Cancel */
        void Cancel() noexcept override;
        /** @copydoc Runtime::IRuntimeSaveRestoreHandoff::IsSettled */
        [[nodiscard]] bool IsSettled() const noexcept override;

    private:
        Runtime::RuntimeSceneService &scenes_;
        std::shared_ptr<PlaySaveRestoreSource> source_;
        std::shared_ptr<const Runtime::ISceneRestoreGenerationAuthority> authority_;
        std::shared_ptr<PlaySaveSceneHandoffState> state_;
        Runtime::SaveWorldId world_;
    };
}  // namespace Horo::Application::Internal
