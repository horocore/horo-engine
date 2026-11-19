#pragma once

/** @file SceneReplicationCommitSource.h
 * @brief Explicit Scene-owner commit and NetworkFlush composition for declared transform replication.
 */

#include "Horo/Network/ReplicationStateCapture.h"
#include "Horo/Runtime/RuntimeDispatchEvidence.h"

#include <thread>

namespace Horo::Network {
    /** @brief Typed canonical Scene property; no property strings, offsets or ECS memory scanning. */
    enum class SceneReplicationProperty : std::uint8_t {
        TranslationX,
        TranslationY,
        TranslationZ,
        RotationX,
        RotationY,
        RotationZ,
        RotationW
    };

    /** @brief Declared FieldId binding to one Scene-owned transform scalar. Rotation requires all four components. */
    struct SceneReplicationFieldBinding final {
        FieldId field;
        SceneReplicationProperty property;
    };

    /** @brief Real RuntimeScene owner adapter and explicit post-commit NetworkFlush composition.
     * The host routes the scene's commands through CommitSimulationTick and keeps all mutation on
     * the scene owner thread. A read barrier rejects reentrant wrapper commits. Even a direct
     * RuntimeScene::Commit bypass invalidates the actual RuntimeSceneView and rejects the whole
     * capture candidate before publication. The source pins the scene; copied snapshots do not.
     * Application-owned Scenes can bind the read-only observed mode to their committed scheduler dispatch instead.
     * Replace either source/coordinator together on scene/schema/module generation replacement.
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
        /** @internal Factory-only construction for the application's unmodified Scene owner. */
        SceneReplicationCommitSource(ConstructionKey key, Runtime::SceneRuntimeId scene, Runtime::RuntimeDispatchSource dispatch,
                                     ReplicationSchemaId schema, ReplicationSchemaVersion version,
                                     std::vector<SceneReplicationFieldBinding> fields, std::weak_ptr<const void> sceneOwner);
        /** @brief Composes declared typed Scene bindings outside simulation.
         * @param scene Canonical scene owner pinned for this adapter lifetime.
         * @param fields Exact unique schema FieldIds to publish as floating-point transform scalars.
         * @return Owner adapter or a typed malformed/capacity error.
         */
        [[nodiscard]] static Result<std::shared_ptr<SceneReplicationCommitSource>> Create(
            std::shared_ptr<Runtime::RuntimeScene> scene, std::span<const SceneReplicationFieldBinding> fields);
        /** @brief Binds an application-owned Scene and its actual fixed commit issuer without inheriting mutation authority.
         * @param scene Expected unique runtime Scene identity.
         * @param dispatch Exact scheduler issuer retained by the owning application participant.
         * @param schema Exact allowed schema. @param version Its exact installed version.
         * @param fields Complete declared transform fields.
         * @param sceneOwner Exact owner lifetime. A successful read pins this scope only until EndRead.
         * @return Owner-lane observation adapter or typed invalid/capacity error. */
        [[nodiscard]] static Result<std::shared_ptr<SceneReplicationCommitSource>> CreateObserved(
            Runtime::SceneRuntimeId scene, Runtime::RuntimeDispatchSource dispatch, ReplicationSchemaId schema,
            ReplicationSchemaVersion version, std::span<const SceneReplicationFieldBinding> fields, std::weak_ptr<const void> sceneOwner);
        /** @brief Observes the exact current Scene in the canonical post-commit fixed callback.
         * @param view Current immutable view supplied by the already owning Scene service.
         * @param evidence Live callback evidence from the bound scheduler.
         * @return True on observation; wrong worlds, expired/foreign attempts and repeated ticks preserve prior state.
         * @details The source holds only a read-only Scene view. Neither this call nor subsequent capture commits a Scene transaction. */
        [[nodiscard]] bool ObserveCommittedTick(Runtime::RuntimeSceneView view, const Runtime::RuntimeDispatchEvidence &evidence) noexcept;
        /** @brief Atomically commits Scene commands before opening the replication read safe point.
         * @param commands Complete owner transaction.
         * @param simulationTick Strictly increasing positive host simulation tick.
         * @return Scene commit result, preserving prior complete publication on failure.
         */
        [[nodiscard]] Result<Runtime::StructuralCommitResult> CommitSimulationTick(const Runtime::SceneCommandBuffer &commands,
                                                                                   std::uint64_t simulationTick);
        /** @brief Acquires read-only capture evidence for the actual committed Scene and current world mapping.
         * @param world Matching active replication lifecycle.
         * @param phase Actual host phase; only NetworkFlush is accepted.
         * @param cancellation Caller cancellation.
         * @return Exact world read and committed tick or a typed phase, commitment, or lifecycle error.
         * @details Intended for preparing a bounded capture or admitting an explicitly empty initial inventory.
         * Callers still need to read each nonempty target through this source. */
        [[nodiscard]] Result<ReplicationWorldCaptureRead> AcquireCaptureReadAfterCommit(const ReplicationWorldLifecycle &world,
                                                                                        Runtime::RuntimePhase phase,
                                                                                        const CancellationToken &cancellation = {}) const;
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
        /** @brief Keeps the exact canonical Scene owner live before entering a view read. */
        [[nodiscard]] std::shared_ptr<const void> PinSceneOwner() const noexcept;
        std::shared_ptr<Runtime::RuntimeScene> scene_;
        Runtime::SceneRuntimeId expectedScene_;
        Runtime::RuntimeDispatchSource dispatch_;
        ReplicationSchemaId schema_;
        ReplicationSchemaVersion version_;
        std::vector<SceneReplicationFieldBinding> fields_;
        std::weak_ptr<const void> sceneOwner_;
        Runtime::RuntimeSceneView committed_;
        std::uint64_t tick_{};
        std::uint64_t revision_{};
        std::thread::id owner_{std::this_thread::get_id()};
        mutable std::optional<Runtime::EntityRef> reading_;
        mutable std::shared_ptr<const void> readOwner_;
    };
}  // namespace Horo::Network
