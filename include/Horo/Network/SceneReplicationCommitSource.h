#pragma once

/** @file SceneReplicationCommitSource.h
 * @brief Explicit Scene-owner commit and NetworkFlush composition for declared transform replication.
 */

#include "Horo/Network/ReplicationStateCapture.h"

#include <thread>

namespace Horo::Network {
    /** @brief Typed canonical Scene property; no property strings, offsets or ECS memory scanning. */
    enum class SceneReplicationProperty : std::uint8_t {
        TranslationX,
        TranslationY,
        TranslationZ
    };

    /** @brief Declared FieldId binding to one Scene-owned canonical transform scalar. */
    struct SceneReplicationFieldBinding final {
        FieldId field;
        SceneReplicationProperty property;
    };

    /** @brief Real RuntimeScene owner adapter and explicit post-commit NetworkFlush composition.
     * The host routes the scene's commands through CommitSimulationTick and keeps all mutation on
     * the scene owner thread. A read barrier rejects reentrant wrapper commits. Even a direct
     * RuntimeScene::Commit bypass invalidates the actual RuntimeSceneView and rejects the whole
     * capture candidate before publication. The source pins the scene; copied snapshots do not.
     * Replace this source/coordinator together on scene/schema/module generation replacement.
     */
    class SceneReplicationCommitSource final : public ICommittedReplicationSource {
        struct ConstructionKey final {
        private:
            friend class SceneReplicationCommitSource;
            ConstructionKey() = default;
        };

    public:
        /** @brief Factory-only constructor admitted after complete declaration validation. @internal
         * @param key Private factory admission; callers cannot fabricate a key.
         * @param scene Pinned canonical owner.
         * @param fields Complete sorted and validated bindings.
         */
        SceneReplicationCommitSource(ConstructionKey key, std::shared_ptr<Runtime::RuntimeScene> scene,
                                     std::vector<SceneReplicationFieldBinding> fields);
        /** @brief Composes declared typed Scene bindings outside simulation.
         * @param scene Canonical scene owner pinned for this adapter lifetime.
         * @param fields Exact unique schema FieldIds to publish as floating-point transform scalars.
         * @return Owner adapter or a typed malformed/capacity error.
         */
        [[nodiscard]] static Result<std::shared_ptr<SceneReplicationCommitSource>> Create(
            std::shared_ptr<Runtime::RuntimeScene> scene, std::span<const SceneReplicationFieldBinding> fields);
        /** @brief Atomically commits Scene commands before opening the replication read safe point.
         * @param commands Complete owner transaction.
         * @param simulationTick Strictly increasing positive host simulation tick.
         * @return Scene commit result, preserving prior complete publication on failure.
         */
        [[nodiscard]] Result<Runtime::StructuralCommitResult> CommitSimulationTick(const Runtime::SceneCommandBuffer &commands,
                                                                                   std::uint64_t simulationTick);
        /** @brief Captures declared source targets at the actual completed Scene tick in NetworkFlush.
         * @param world Matching active replication lifecycle.
         * @param capture Prepared coordinator bound to this source generation.
         * @param phase Actual host phase; only NetworkFlush is accepted.
         * @param cancellation Caller cancellation.
         * @return Capture report or typed phase/commit/lifecycle error; callers supply no fabricated tick.
         */
        [[nodiscard]] Result<ReplicationCaptureReport> CaptureAfterCommit(const ReplicationWorldLifecycle &world,
                                                                          ReplicationStateCapture &capture, Runtime::RuntimePhase phase,
                                                                          const CancellationToken &cancellation = {}) const;
        /** @copydoc ICommittedReplicationSource::BeginRead */
        [[nodiscard]] Result<ReplicationCommittedRead> BeginRead(const NetworkObjectMappingEntry &object,
                                                                 std::uint64_t tick) const override;
        /** @copydoc ICommittedReplicationSource::Capture */
        [[nodiscard]] Result<void> Capture(const ReplicationCommittedRead &read, ReplicationCaptureWriter &writer) const override;
        /** @copydoc ICommittedReplicationSource::IsCurrent */
        [[nodiscard]] bool IsCurrent(const ReplicationCommittedRead &read) const noexcept override;
        /** @copydoc ICommittedReplicationSource::EndRead */
        void EndRead(const ReplicationCommittedRead &read) const noexcept override;

    private:
        std::shared_ptr<Runtime::RuntimeScene> scene_;
        std::vector<SceneReplicationFieldBinding> fields_;
        Runtime::RuntimeSceneView committed_;
        std::uint64_t tick_{};
        std::uint64_t revision_{};
        std::thread::id owner_{std::this_thread::get_id()};
        mutable std::optional<Runtime::EntityRef> reading_;
    };
}  // namespace Horo::Network
