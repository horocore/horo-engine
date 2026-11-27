#pragma once

/**
 * @file WorldStreamingRuntimeComposition.h
 * @brief Explicit runtime service composition and scheduler ownership for World Streaming.
 */

#include "Horo/WorldStreaming/OriginRebaseTransaction.h"
#include "Horo/WorldStreaming/StreamingSchedulerAdmission.h"

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Horo::WorldStreaming {
    namespace Detail {
        /** @brief Tag separating runtime service identities from other world-streaming identities. */
        struct StreamingRuntimeServiceIdTag;
        /** @brief Tag separating runtime composition owners from services and scheduler ledgers. */
        struct StreamingRuntimeOwnerIdTag;
        /** @brief Tag separating service revisions from composition revisions. */
        struct StreamingRuntimeServiceRevisionTag;
        /** @brief Tag separating composition revisions from service and source revisions. */
        struct StreamingRuntimeCompositionRevisionTag;
    }  // namespace Detail

    class WorldPartitionRegistry;

    /** @brief Stable host-composed runtime service identity. */
    using StreamingRuntimeServiceId =
        Foundation::Detail::NonZeroId64<Detail::StreamingRuntimeServiceIdTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Unique identity for one host-composed runtime owner lifetime. */
    using StreamingRuntimeOwnerId =
        Foundation::Detail::NonZeroId64<Detail::StreamingRuntimeOwnerIdTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Immutable revision of one runtime service binding. */
    using StreamingRuntimeServiceRevision =
        Foundation::Detail::NonZeroId64<Detail::StreamingRuntimeServiceRevisionTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Immutable revision of the complete runtime composition. */
    using StreamingRuntimeCompositionRevision =
        Foundation::Detail::NonZeroId64<Detail::StreamingRuntimeCompositionRevisionTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Role of one explicitly supplied runtime service; the scheduler is owned directly by the composition. */
    enum class StreamingRuntimeServiceRole : std::uint8_t {
        Planner,
        AssetProvider,
        SceneRuntime,
        FeatureAdapter,
    };

    /** @brief Borrowed service binding whose instance must outlive its active composition revision. */
    struct StreamingRuntimeServiceBinding final {
        StreamingRuntimeServiceId id;             /**< Stable identity used for deterministic lookup and diagnostics. */
        StreamingRuntimeServiceRevision revision; /**< Exact immutable implementation/configuration revision. */
        StreamingRuntimeServiceRole role{};       /**< One declared composition role. */
        const void *instance{};                   /**< Non-owning host-supplied service; never discovered globally. */

        /** @brief Checks the complete binding representation. @return True for valid identity, revision, role and instance. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const StreamingRuntimeServiceBinding &) const noexcept = default;
    };

    /** @brief Exact mounted partition and host composition lifetime, distinct from source-registry owner tokens. */
    struct StreamingRuntimeOwnerToken final {
        WorldPartitionId partition;    /**< Stable mounted partition identity. */
        PartitionEpoch epoch;          /**< Exact mounted partition incarnation. */
        StreamingRuntimeOwnerId owner; /**< Unique host composition lifetime. */

        /** @brief Checks the complete owner representation. @return True when partition, epoch and owner are usable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const StreamingRuntimeOwnerToken &) const noexcept = default;
    };

    /** @brief Validated construction facts for one mounted partition composition. */
    struct WorldStreamingRuntimeCompositionConfig final {
        /** @brief Hard implementation ceiling for feature-adapter bindings. */
        static constexpr std::uint32_t MaximumFeatureAdapters = 256;

        StreamingRuntimeOwnerToken owner;                  /**< Exact partition/epoch composition lifetime. */
        StreamingRuntimeCompositionRevision revision;      /**< Initial immutable composition revision. */
        StreamingSchedulerLedgerId schedulerOwner;         /**< Unique scheduler-ledger owner identity. */
        StreamingSchedulerAdmissionLimits schedulerLimits; /**< Bounded preallocated operation admission. */
        std::uint32_t maximumFeatureAdapters{};            /**< Positive host ceiling within MaximumFeatureAdapters. */

        /** @brief Checks fixed construction facts independent of service bindings. @return True when every field is usable. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Lifecycle gate for replacement, cancellation and scheduler-aware shutdown. */
    enum class WorldStreamingRuntimeCompositionState : std::uint8_t {
        Active,
        Cancelling,
        Draining,
        Closed,
    };

    /**
     * @brief Unique explicit composition of planner, asset, Scene and feature services with one owned scheduler ledger.
     * @details Service instances are borrowed and must outlive their active revision plus every scheduler reservation.
     *          Mutating calls are confined to StreamingAuthorityRole and perform no global discovery or registration.
     *          Origin participants do not re-enter the composition from preparation, publication or rollback callbacks.
     */
    class WorldStreamingRuntimeComposition final {
    public:
        WorldStreamingRuntimeComposition(const WorldStreamingRuntimeComposition &) = delete;
        WorldStreamingRuntimeComposition &operator=(const WorldStreamingRuntimeComposition &) = delete;
        /**
         * @brief Transfer unique composition ownership and close the moved-from scheduler admission gate.
         * @param other Composition whose services and scheduler ownership are transferred.
         */
        WorldStreamingRuntimeComposition(WorldStreamingRuntimeComposition &&other) noexcept;
        WorldStreamingRuntimeComposition &operator=(WorldStreamingRuntimeComposition &&) = delete;

        /**
         * @brief Validate and publish one complete explicit runtime composition.
         * @param config Exact owner, revision and scheduler admission facts.
         * @param services Exactly one planner, asset provider and Scene runtime plus at least one feature adapter.
         * @return Active composition or a typed invalid, conflict or capacity failure with no partial owner.
         */
        [[nodiscard]] static Result<WorldStreamingRuntimeComposition> Create(const WorldStreamingRuntimeCompositionConfig &config,
                                                                             std::span<const StreamingRuntimeServiceBinding> services);

        /**
         * @brief Create a mounted composition that owns its one canonical runtime origin frame.
         * @param config Exact runtime owner, revision and scheduler admission facts.
         * @param services Complete explicit service bindings.
         * @param registry Active registry with a validated publication for this exact mount.
         * @param initial Host-authorized canonical frame with a unique identity for this mounted lifetime.
         * @return Complete mounted composition or a typed failure without publication.
         * @details The host retires the composition before unmounting its registry. The immutable grid anchor does not set the runtime
         * origin.
         */
        [[nodiscard]] static Result<WorldStreamingRuntimeComposition> CreateWithOrigin(
            const WorldStreamingRuntimeCompositionConfig &config, std::span<const StreamingRuntimeServiceBinding> services,
            const WorldPartitionRegistry &registry, OriginFrame initial);

        /**
         * @brief Read the mounted frame owner's observer interface on the authority lane.
         * @return Borrow valid while this composition lives, or null if the mount has no active origin.
         * @details Consumers retain leases to observe a generation. The composition owns every publication.
         */
        [[nodiscard]] const OriginFrameOwner *Origin() const noexcept;

        /**
         * @brief Prepare one canonical rebase against the exact mounted origin and composition revision.
         * @param owner Exact active mounted owner.
         * @param revision Exact active composition revision.
         * @param context Authorized decision, monotonically increasing transaction ID, active frame and participant ceiling.
         * @param required Complete required participant manifest.
         * @param participants Actual host-owned participants; prepared receipts remain owned by the composition.
         * @return Success or typed rejection with no active frame or participant mutation.
         */
        [[nodiscard]] Result<void> PrepareOriginRebase(const StreamingRuntimeOwnerToken &owner,
                                                       StreamingRuntimeCompositionRevision revision,
                                                       const OriginRebaseTransactionContext &context,
                                                       std::span<const OriginRebaseParticipantRequirement> required,
                                                       std::span<IOriginRebaseParticipant *const> participants);

        /**
         * @brief Publish the exact prepared rebase on the host's canonical post-simulation safe point.
         * @param owner Exact active mounted owner.
         * @param revision Exact active composition revision.
         * @param transaction Exact staged transaction identity.
         * @param point Safe point observed by the host.
         * @return Success or typed failure. A wrong safe point leaves the transaction available for retry.
         */
        [[nodiscard]] Result<void> CommitOriginRebase(const StreamingRuntimeOwnerToken &owner, StreamingRuntimeCompositionRevision revision,
                                                      OriginRebaseTransactionId transaction, OriginRebaseCommitPoint point);

        /**
         * @brief Roll back a prepared transaction without changing the active origin.
         * @param owner Exact active mounted owner.
         * @param revision Exact active composition revision.
         * @param transaction Exact staged transaction identity.
         * @return Success or typed stale rejection without affecting another transaction.
         */
        [[nodiscard]] Result<void> CancelOriginRebase(const StreamingRuntimeOwnerToken &owner, StreamingRuntimeCompositionRevision revision,
                                                      OriginRebaseTransactionId transaction);

        /**
         * @brief Transactionally replace all borrowed services while no scheduler work or origin transaction is retained.
         * @param owner Exact active owner lifetime.
         * @param revision Strictly newer complete composition revision.
         * @param services Complete replacement set under the original feature-adapter ceiling.
         * @return Success or typed stale, lifecycle, invalid, conflict or capacity failure without partial mutation.
         */
        [[nodiscard]] Result<void> Replace(const StreamingRuntimeOwnerToken &owner, StreamingRuntimeCompositionRevision revision,
                                           std::span<const StreamingRuntimeServiceBinding> services);

        /**
         * @brief Close new scheduler admission for cooperative cancellation without releasing retained work.
         * @param owner Exact current owner lifetime. @param revision Exact current composition revision.
         * @return Success, including an idempotent repeat, or a typed stale/lifecycle failure.
         */
        [[nodiscard]] Result<void> RequestCancellation(const StreamingRuntimeOwnerToken &owner,
                                                       StreamingRuntimeCompositionRevision revision) noexcept;

        /**
         * @brief Begin terminal shutdown while keeping all borrowed services alive through scheduler drain.
         * @param owner Exact current owner lifetime.
         * @return Success, including an idempotent repeat, or a typed stale-owner failure.
         */
        [[nodiscard]] Result<void> BeginShutdown(const StreamingRuntimeOwnerToken &owner) noexcept;

        /** @brief Resolve one service by stable identity. @param id Exact service identity. @return Owned binding or typed stale error. */
        [[nodiscard]] Result<StreamingRuntimeServiceBinding> Resolve(StreamingRuntimeServiceId id) const;

        /** @brief Return the exact mounted owner lifetime. @return Immutable owner token. */
        [[nodiscard]] const StreamingRuntimeOwnerToken &Owner() const noexcept;
        /** @brief Return the complete composition revision. @return Current non-zero revision. */
        [[nodiscard]] StreamingRuntimeCompositionRevision Revision() const noexcept;
        /** @brief Return canonical role/identity-ordered bindings. @return Borrow valid until replacement or destruction. */
        [[nodiscard]] std::span<const StreamingRuntimeServiceBinding> Services() const noexcept;
        /** @brief Return the lifecycle gate, deriving Closed after the scheduler finishes draining. @return Current lifecycle state. */
        [[nodiscard]] WorldStreamingRuntimeCompositionState State() const noexcept;
        /** @brief Access the uniquely owned scheduler ledger on StreamingAuthorityRole. @return Mutable owned scheduler ledger. */
        [[nodiscard]] StreamingSchedulerAdmissionLedger &Scheduler() noexcept;
        /** @brief Inspect the uniquely owned scheduler ledger. @return Immutable owned scheduler ledger. */
        [[nodiscard]] const StreamingSchedulerAdmissionLedger &Scheduler() const noexcept;

    private:
        WorldStreamingRuntimeComposition(const WorldStreamingRuntimeCompositionConfig &config, StreamingSchedulerAdmissionLedger scheduler,
                                         std::vector<StreamingRuntimeServiceBinding> services) noexcept;

        WorldStreamingRuntimeCompositionConfig config_;
        StreamingSchedulerAdmissionLedger scheduler_;
        std::vector<StreamingRuntimeServiceBinding> services_;
        WorldStreamingRuntimeCompositionState state_{WorldStreamingRuntimeCompositionState::Active};
        std::unique_ptr<OriginFrameOwner> origin_;
        OriginRebaseTransactionId lastOriginTransaction_{};
        std::optional<OriginRebaseTransaction> pendingOrigin_;
    };
}  // namespace Horo::WorldStreaming
