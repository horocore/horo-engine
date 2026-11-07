#pragma once
#include "EditorPlaySavePhysicsServices.h"

#include <thread>

namespace Horo::Application::Internal {
    /** @brief Canonical participant over an ACTUAL pending Scene target and actual archive bytes.
     * SceneRestoreBundle owns this receipt and its immutable identity map through all synchronous preparation calls.
     * The map borrow is used only on the owner during Instantiate, cleared on retirement, and never read by a worker/destructor.
     * The aggregate remains the sole native Scene publisher; no byte placeholder or no-op backend substitutes native preparation.
     */
    class PlaySavePhysicsReceipt final : public Runtime::IStagedRestoreParticipant, public Runtime::IStagedRestoreTransientRetirement {
    public:
        PlaySavePhysicsReceipt(Physics::Detail::PhysicsSceneStateTarget, PlaySavePhysicsPolicy,
                               std::shared_ptr<const PlaySavePhysicsContent>, std::shared_ptr<const std::vector<std::byte>>,
                               Runtime::StagedRestoreContext, Runtime::RuntimeSceneView, const Runtime::PersistentEntityIdentityMap &,
                               CancellationToken, std::shared_ptr<PlaySavePhysicsRestoreFence>);
        [[nodiscard]] const Runtime::StagedRestoreParticipantRequirement &Requirement() const noexcept override;
        [[nodiscard]] Result<void> Decode(const Runtime::StagedRestoreContext &) override;
        [[nodiscard]] Result<void> Validate(const Runtime::StagedRestoreContext &) override;
        [[nodiscard]] Result<void> Instantiate(const Runtime::StagedRestoreContext &) override;
        [[nodiscard]] const Runtime::ICanonicalRestorePreparedState *PreparedState() const noexcept override;
        [[nodiscard]] Result<void> ApplyState(const Runtime::ICanonicalRestoreDependencyLookup &) override;
        [[nodiscard]] Result<void> FixupReferences(const Runtime::ICanonicalRestoreDependencyLookup &) override;
        void PublishPrepared() noexcept override;
        void RollbackPrepared() noexcept override;
        [[nodiscard]] Result<bool> RetirePublishedTransient() override;

    private:
        struct NativeState final : Runtime::ICanonicalRestorePreparedState {
            std::shared_ptr<Physics::Detail::PreparedPhysicsSceneRestore> prepared;
        } state_;

        [[nodiscard]] Result<void> CheckContext(const Runtime::StagedRestoreContext &) const;
        [[nodiscard]] Result<void> CheckNative() const;
        const std::thread::id owner_{std::this_thread::get_id()};
        Physics::Detail::PhysicsSceneStateTarget target_;
        PlaySavePhysicsPolicy policy_;
        std::shared_ptr<const PlaySavePhysicsContent> content_;
        std::shared_ptr<const std::vector<std::byte>> source_; /**< Exact decoded chunk owner; archive ownership alone is insufficient. */
        Runtime::StagedRestoreContext context_;
        Runtime::RuntimeSceneView scene_;
        const Runtime::PersistentEntityIdentityMap *identities_{};
        CancellationToken cancellation_;
        std::shared_ptr<PlaySavePhysicsRestoreFence> fence_;
        Runtime::StagedRestoreParticipantRequirement requirement_;
        std::unique_ptr<Physics::StateDetail::RestartInput> input_;
        bool validated_{}, published_{}, retired_{};
    };
}  // namespace Horo::Application::Internal
