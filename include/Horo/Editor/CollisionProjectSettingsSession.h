#pragma once

/** @file CollisionProjectSettingsSession.h
 * @brief Owner-thread collision draft/history, complete reference review and revocable editor presentation.
 */

#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Editor/ProjectCollisionSettingsStore.h"
#include "Horo/Editor/SceneDocumentTransaction.h"
#include "Horo/Scene/SceneRuntimeConversion.h"

#include <memory>
#include <span>
#include <thread>

namespace Horo::Editor {
    /** @brief Closed actionable collision editor rejection vocabulary. */
    enum class CollisionSettingsFailure : std::uint8_t {
        Invalid,
        Closed,
        WrongThread,
        Stale,
        InUse,
        Unsupported,
        Active,
        Capacity
    };
    /** @brief Returns the stable owning diagnostic without allocating. @param reason Closed rejection. @return Descriptor. */
    [[nodiscard]] const ErrorCodeDescriptor &CollisionSettingsError(CollisionSettingsFailure reason) noexcept;

    /** @brief Runtime ownership evidence is explicit; absence of an injected owner is Unknown. */
    enum class CollisionActivationState : std::uint8_t {
        Unknown,
        NoPhysics,
        Inactive,
        Active
    };

    /** @brief Explicit host evidence; editor policy never discovers a world or selects a backend. */
    struct CollisionActivationEvidence final {
        std::uint64_t revision{};
        CollisionActivationState state{CollisionActivationState::Unknown};
        bool playRunning{};
        std::string projectId;
        std::uint64_t documentRevision{};
        std::uint64_t generation{};
        std::uint64_t fingerprint{};
        bool operator==(const CollisionActivationEvidence &) const = default;
    };

    /** @brief Exact host-published inventory admission; a revision alone never proves complete ownership. */
    struct CollisionInventoryEvidence final {
        Assets::AssetRegistryRevision revision;
        Assets::AssetRegistryBuildStatus status{Assets::AssetRegistryBuildStatus::Failed};
        bool operator==(const CollisionInventoryEvidence &) const = default;
    };

    /** @brief Explicit borrowed owners, valid until the composition closes this session. */
    struct CollisionProjectSettingsOwners final {
        const Assets::AssetRegistry &registry;
        DurableFileSystem &files;
        ProjectMutationCoordinator &mutations;
        CollisionInventoryEvidence inventory;
        const Assets::IAssetRegistrySource &source; /**< Root-bound host source, alive until this session closes. */
        SceneDocumentTransactions *activeScene{};
        std::filesystem::path activeScenePath;
        std::optional<SceneSource::TriggerVolumeConversionContext> triggerBinding;
    };

    /** @brief Closed impact meaning; presentation translates it without parsing diagnostic strings. */
    enum class CollisionImpactReason : std::uint8_t {
        ProfileReference,
        OpaqueOwner,
        InventoryIncomplete,
        TriggerCaptureStale,
        ActiveGeneration
    };

    /** @brief Existing authored owner referencing a stable profile, or an owner whose typed inventory is unsupported. */
    struct CollisionReferenceImpact final {
        std::string path;
        SceneSource::SceneObjectId object;
        std::optional<Physics::CollisionProfileId> profile;
        bool unsupported{};
        bool blocksPublication{};
        CollisionImpactReason reason{CollisionImpactReason::ProfileReference};
    };

    /** @brief Immutable source review owned by one exact opening/draft and host generation. */
    class CollisionSettingsReview final {
    public:
        /** @brief Borrows every known impact, including explicit unsupported owners. @return Bounded owned evidence. */
        [[nodiscard]] std::span<const CollisionReferenceImpact> Impacts() const noexcept;
        /** @brief Returns whether publication is allowed after all owners and activation policy were checked. */
        [[nodiscard]] bool CanCommit() const noexcept;

    private:
        friend class CollisionProjectSettingsSession;
        struct State;
        explicit CollisionSettingsReview(std::shared_ptr<const State> state);
        std::shared_ptr<const State> state_;
    };

    /** @brief One nonreopenable owner-thread session; modals retain weak_ptr only and resolve once per synchronous draw. */
    class CollisionProjectSettingsSession final {
    public:
        /** @brief Opens the exact project collision document and binds explicit existing owner services.
         * @param root Canonical authorized project root. @param projectId Loaded project metadata identity.
         * @param owners Explicit host services, completeness evidence and optional canonical authored Scene owner.
         * @param activation Explicit current host evidence; Unknown preserves the draft but blocks semantic replacement.
         * @return New nonreopenable session or typed missing/source/lifecycle error; no schema defaults are installed.
         * @pre Commands and injected host/filesystem services are synchronous and nonreentrant on the owner thread.
         * All borrowed host services and the optional canonical Scene owner outlive this opening. The composition
         * root calls Close before retiring/replacing any owner; weak presentation handles may outlive a closed session.
         */
        [[nodiscard]] static Result<std::shared_ptr<CollisionProjectSettingsSession>> Open(const std::filesystem::path &root,
                                                                                           std::string projectId,
                                                                                           const CollisionProjectSettingsOwners &owners,
                                                                                           CollisionActivationEvidence activation);
        ~CollisionProjectSettingsSession();
        CollisionProjectSettingsSession(const CollisionProjectSettingsSession &) = delete;
        CollisionProjectSettingsSession &operator=(const CollisionProjectSettingsSession &) = delete;
        /** @brief Returns a shared immutable cached draft without I/O or copying its vectors.
         * @return Current document or a closed/foreign-thread error. Any returned snapshot owns its complete value.
         */
        [[nodiscard]] Result<std::shared_ptr<const Application::ProjectCollisionSettings>> Snapshot() const;
        /** @brief Creates an inert stable layer identity for an explicit authoring action. @return New unused ID or typed error. */
        [[nodiscard]] Result<Physics::CollisionLayerId> NewLayerId() const;
        /** @brief Creates an inert stable profile identity for an explicit authoring action. @return New unused ID or typed error. */
        [[nodiscard]] Result<Physics::CollisionProfileId> NewProfileId() const;
        /** @brief Creates an inert stable query intent identity for an explicit authoring action. @return New unused ID or typed error. */
        [[nodiscard]] Result<Physics::PhysicsQueryChannelId> NewChannelId() const;
        /** @brief Captures source identities, complete owner impacts and activation admission before a draft can commit.
         * @param candidate Complete proposed document; display renames retain typed IDs.
         * @return Owned review containing blocking impact evidence or typed malformed/inventory failure.
         * @details Profile deletion remains blocked while any exact existing owner refers to it. Internal layers/channels
         * must be removed from the proposed complete schema explicitly. Active semantic replacement requires Stop/restart.
         */
        [[nodiscard]] Result<CollisionSettingsReview> Preview(Application::ProjectCollisionSettingsInput candidate) const;
        /** @brief Rechecks exact review/source/owner/activation fences and publishes one preallocated history root.
         * @param review Review returned by this opening. @return Success or zero-mutation stale/impact/activation rejection.
         */
        [[nodiscard]] Result<void> Commit(const CollisionSettingsReview &review);
        /** @brief Replays one local collision draft history entry under the same source and owner impact admission.
         * @param redo False for undo, true for redo. @return Success or unchanged state when stale/blocked.
         */
        [[nodiscard]] Result<void> Replay(bool redo);
        /** @brief Durably saves this exact draft under source CAS without mutating any active world.
         * @return Published receipt or a typed failure; uncertain durability leaves the draft dirty for explicit retry.
         */
        [[nodiscard]] Result<ProjectCollisionPublication> Save();
        /** @brief Updates explicit host lifecycle evidence on the owner thread, invalidating older reviews.
         * @param evidence New monotonic host state; exhaustion may revoke to Unknown at UINT64_MAX without discarding drafts. @return
         * Success or typed stale/invalid lifecycle evidence.
         */
        [[nodiscard]] Result<void> ObserveActivation(CollisionActivationEvidence evidence);
        /** @brief Updates exact host inventory admission after an explicit registry publication.
         * @param evidence Published revision and actual completeness outcome. @return Success or stale/invalid rejection.
         */
        [[nodiscard]] Result<void> ObserveInventory(CollisionInventoryEvidence evidence);
        /** @brief Borrows the exact last host evidence; no runtime discovery, provider invocation or current-world claim.
         * @return Borrowed evidence until the next explicit session command, or a closed/foreign-thread rejection.
         */
        [[nodiscard]] Result<const CollisionActivationEvidence *> Activation() const;
        /** @brief Checks local draft state only. @return True when unsaved edits or uncertain durability remain. */
        [[nodiscard]] bool IsDirty() const noexcept;
        /** @brief Revokes this opening before clearing cached state; old weak references never resolve a new session. */
        void Close() noexcept;

    private:
        struct State;
        explicit CollisionProjectSettingsSession(std::unique_ptr<State> state);
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Editor
