#pragma once

/**
 * @file NavigationWorldLifecycle.h
 * @brief Transactional per-Scene navigation-world activation, leasing, pause, unload, and shutdown.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Navigation/NavigationBackend.h"
#include "Horo/Navigation/NavigationIdentity.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Horo::Navigation {
    namespace Detail {
        struct NavigationWorldRecord;
    }

    /** @brief Exact Scene/world/topology identity tuple prepared before safe-point publication. */
    struct NavigationWorldActivationDescriptor final {
        NavigationSceneRuntimeId scene;            /**< Exact runtime-Scene incarnation supplied by the host. */
        NavigationSceneGeneration sceneGeneration; /**< Exact Scene activation generation. */
        NavigationWorldId world;                   /**< Never-reused navigation-world incarnation. */
        NavigationGeneration topology;             /**< Immutable provider topology generation. */

        /** @brief Checks complete non-zero identity representation. @return True when every identity is usable. */
        [[nodiscard]] bool IsValid() const noexcept;
        constexpr auto operator<=>(const NavigationWorldActivationDescriptor &) const noexcept = default;
    };

    /** @brief Owner-thread lifecycle state for the published navigation world. */
    enum class NavigationWorldLifecycleState : std::uint8_t {
        Empty,
        Active,
        Paused,
        ShuttingDown,
        Closed,
    };

    /** @brief First logical reason a pinned provider stopped admitting results. */
    enum class NavigationWorldRevocation : std::uint8_t {
        None,
        TopologyReplaced,
        WorldRetired,
        TileEvicted
    };

    /** @brief Selects why the current topology will stop accepting results at structural publication. */
    enum class NavigationTopologyChange : std::uint8_t {
        Replacement,
        ForcedCellEviction
    };

    /**
     * @brief Immutable read lease that pins one provider record while its world may be logically revoked.
     * @details Copies are thread-safe lifetime pins. Backend calls must use Cancellation(), and IsRevoked()
     *          must be rechecked before publishing a result. The returned backend borrow never outlives this lease.
     */
    class NavigationWorldReadLease final {
    public:
        /** @brief Construct an inert lease suitable for later move assignment. */
        NavigationWorldReadLease() noexcept = default;
        /** @brief Copy a provider lifetime pin. @param other Valid or moved-from source lease. */
        NavigationWorldReadLease(const NavigationWorldReadLease &other) noexcept;
        /** @brief Replace this lifetime pin with a copy. @param other Valid or moved-from source lease. @return This lease. */
        NavigationWorldReadLease &operator=(const NavigationWorldReadLease &other) noexcept;
        /** @brief Transfer a provider lifetime pin. @param other Source left invalid. */
        NavigationWorldReadLease(NavigationWorldReadLease &&other) noexcept;
        /** @brief Replace this lifetime pin by transfer. @param other Source left invalid. @return This lease. */
        NavigationWorldReadLease &operator=(NavigationWorldReadLease &&other) noexcept;
        /** @brief Release this provider lifetime pin without blocking. */
        ~NavigationWorldReadLease();

        /** @brief Report whether this lease pins a provider record. @return False for a moved-from lease. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Return the exact immutable activation identities. @return Borrow valid for this lease lifetime.
         * @pre IsValid() is true.
         */
        [[nodiscard]] const NavigationWorldActivationDescriptor &Descriptor() const noexcept;
        /** @brief Return the pinned provider execution seam. @return Borrow valid for this lease lifetime.
         * @pre IsValid() is true.
         */
        [[nodiscard]] const INavigationQueryBackend &Backend() const noexcept;
        /** @brief Return cooperative world-revocation cancellation. @return Token valid beyond this lease. */
        [[nodiscard]] CancellationToken Cancellation() const noexcept;
        /** @brief Report logical revocation independent of retained memory.
         * @return True after replacement, unload, or shutdown; pause alone does not invalidate admitted work.
         */
        [[nodiscard]] bool IsRevoked() const noexcept;
        /** @brief Reads the first logical retirement reason without releasing the provider pin.
         * @return Exact reason, or None for a current or inert lease.
         */
        [[nodiscard]] NavigationWorldRevocation Revocation() const noexcept;

    private:
        friend class NavigationWorldLifecycle;
        explicit NavigationWorldReadLease(std::shared_ptr<const Detail::NavigationWorldRecord> record) noexcept;
        std::shared_ptr<const Detail::NavigationWorldRecord> record_;
    };

    /**
     * @brief Unique owner-thread authority for one active and one staged per-Scene navigation world.
     * @details Stage performs all fallible retention before CommitAtSafePoint. Commit is a bounded ownership swap;
     *          failure preserves the old active world. Mutations and retirement collection run on the host's
     *          lifecycle owner thread, including Acquire. Read leases may then cross workers and keep revoked
     *          provider memory safe. Destruction requests cancellation but never blocks for worker completion.
     */
    class NavigationWorldLifecycle final {
    public:
        static constexpr std::uint32_t MaximumRetiredWorlds = 64;

        NavigationWorldLifecycle(const NavigationWorldLifecycle &) = delete;
        NavigationWorldLifecycle &operator=(const NavigationWorldLifecycle &) = delete;
        /** @brief Transfer owner-thread lifecycle authority. @param other Source becomes closed. */
        NavigationWorldLifecycle(NavigationWorldLifecycle &&other) noexcept;
        NavigationWorldLifecycle &operator=(NavigationWorldLifecycle &&) = delete;
        /** @brief Revoke owned records and release the owner's pins without blocking for worker leases. */
        ~NavigationWorldLifecycle();

        /**
         * @brief Create an empty owner with preallocated retirement bookkeeping.
         * @param maximumRetiredWorlds Positive bound within MaximumRetiredWorlds.
         * @return Empty lifecycle or typed invalid/capacity failure.
         */
        [[nodiscard]] static Result<NavigationWorldLifecycle> Create(std::uint32_t maximumRetiredWorlds);

        /**
         * @brief Validate and retain one detached provider candidate without changing the active world.
         * @param descriptor Exact Scene/world/topology identities for later publication. A candidate for the active
         *                   world must keep its Scene incarnation and generation and advance past every staged topology generation.
         * @param backend Unique fully initialized provider; consumed on success or failure.
         * @return Success or typed invalid/lifecycle/capacity failure with the active world unchanged.
         */
        [[nodiscard]] Result<void> Stage(const NavigationWorldActivationDescriptor &descriptor,
                                         std::unique_ptr<INavigationQueryBackend> backend);

        /** @brief Stages a streamed topology from an exact active predecessor.
         * @param expectedActive Descriptor captured before the detached provider was built.
         * @param nextTopology Strictly newer, previously unused generation of the same world.
         * @param backend Unique fully initialized provider. Consumed on success or failure.
         * @return Success or typed refusal. A stale build leaves current and staged providers untouched.
         */
        [[nodiscard]] Result<void> StageTopology(const NavigationWorldActivationDescriptor &expectedActive,
                                                 NavigationGeneration nextTopology, std::unique_ptr<INavigationQueryBackend> backend);

        /** @brief Stages explicit loss of tile coverage while retaining the logical world identity.
         * @param expectedActive Exact currently published descriptor. @param nextTopology New unique generation.
         * @return Success or typed refusal. After commit, Acquire reports NoNavigationData until coverage returns.
         */
        [[nodiscard]] Result<void> StageEmptyTopology(const NavigationWorldActivationDescriptor &expectedActive,
                                                      NavigationGeneration nextTopology);

        /** @brief Stages the first generation of a streamed world before any cell grants coverage. */
        [[nodiscard]] Result<void> StageInitialEmptyTopology(const NavigationWorldActivationDescriptor &descriptor);

        /** @brief Discards only the named unpublished same-world replacement during aggregate rollback.
         * @param candidate Exact candidate identity. @return Success or a typed stale failure.
         */
        [[nodiscard]] Result<void> DiscardStagedTopology(const NavigationWorldActivationDescriptor &candidate);

        /** @brief Publishes only the named candidate during a streamed cell's lifecycle commit.
         * @param candidate Exact staged topology identity. @return Success or typed refusal, preserving the active topology.
         */
        [[nodiscard]] Result<void> CommitStagedTopology(const NavigationWorldActivationDescriptor &candidate);

        /** @brief Checks exact staged identity and bounded retirement before an aggregate commit.
         * @param candidate Exact staged topology. @return Success or typed stale/capacity rejection.
         */
        [[nodiscard]] Result<void> ValidateStagedTopology(const NavigationWorldActivationDescriptor &candidate) const;
        /**
         * @brief Publish the staged candidate at CommitDeferredLifecycleChanges.
         * @param expectedScene Exact active Scene incarnation at the safe point.
         * @param expectedGeneration Exact active Scene generation at the safe point.
         * @return Success or typed stale/lifecycle/retirement-capacity failure with the old world preserved.
         * @post A same-world topology replacement preserves paused admission and revokes the previous provider's read leases.
         */
        [[nodiscard]] Result<void> CommitAtSafePoint(NavigationSceneRuntimeId expectedScene, NavigationSceneGeneration expectedGeneration);

        /** @brief Close new leases without revoking the active world. @param world Exact active world. @return Typed result. */
        [[nodiscard]] Result<void> Pause(NavigationWorldId world) noexcept;
        /** @brief Reopen leases for a paused unchanged world. @param world Exact active world. @return Typed result. */
        [[nodiscard]] Result<void> Resume(NavigationWorldId world) noexcept;
        /** @brief Revoke and retire the exact active world. @param world Exact active world. @return Typed result. */
        [[nodiscard]] Result<void> Unload(NavigationWorldId world) noexcept;

        /** @brief Close all admission, cancel staged/active/retired work, and begin ordered retirement. */
        void BeginShutdown() noexcept;
        /** @brief Reclaim drained retired providers and finish shutdown when possible. @return Current lifecycle state. */
        [[nodiscard]] NavigationWorldLifecycleState CollectRetired() noexcept;

        /** @brief Acquire the exact active world for bounded work on the owner thread.
         * @param world Expected world.
         * @return Lease or typed failure.
         */
        [[nodiscard]] Result<NavigationWorldReadLease> Acquire(NavigationWorldId world) const;
        /** @brief Inspect the active descriptor by value. @return Descriptor or NoNavigationData. */
        [[nodiscard]] Result<NavigationWorldActivationDescriptor> ActiveDescriptor() const;
        /** @brief Report current owner-thread lifecycle state. @return Current state. */
        [[nodiscard]] NavigationWorldLifecycleState State() const noexcept;
        /** @brief Report retained revoked records awaiting lease drain. @return Bounded retained count. */
        [[nodiscard]] std::size_t RetiredCount() const noexcept;
        /** @brief Report whether a detached candidate is staged. @return True when CommitAtSafePoint may publish. */
        [[nodiscard]] bool HasStagedCandidate() const noexcept;

    private:
        friend class NavigationStreamedTopologyPublication;
        friend class NavigationTraversalCatalog;
        /** @brief Claims the current topology for one catalog on the lifecycle owner lane.
         * @param expected Exact activation observed by the catalog factory.
         * @param owner Lifetime witness for the catalog's detached implementation.
         * @return Monotonically issued catalog generation, or a typed stale, duplicate, or exhausted result.
         */
        [[nodiscard]] Result<std::uint64_t> ClaimTraversalCatalog(const NavigationWorldActivationDescriptor &expected,
                                                                  std::weak_ptr<const void> owner);
        /** @brief Publishes the validated aggregate topology without waiting for cancellation observers. */
        void PublishValidatedTopology(const NavigationWorldActivationDescriptor &candidate, NavigationTopologyChange change) noexcept;
        /** @brief Wakes cancellation observers on the exact predecessor after aggregate publication. */
        static void NotifyRetired(const NavigationWorldReadLease &predecessor) noexcept;
        /** @brief Discards only the named unpublished topology. @return True if it was discarded. */
        bool TryDiscardStagedTopology(const NavigationWorldActivationDescriptor &candidate) noexcept;
        /** @brief Construct storage after bounded retirement allocation succeeds. */
        NavigationWorldLifecycle(std::uint32_t maximumRetiredWorlds,
                                 std::vector<std::shared_ptr<Detail::NavigationWorldRecord>> retired) noexcept;
        /** @brief Stages a validated provider or explicitly empty topology on the owner lane. */
        [[nodiscard]] Result<void> StageCandidate(const NavigationWorldActivationDescriptor &descriptor,
                                                  std::unique_ptr<INavigationQueryBackend> backend, bool emptyAllowed);
        /** @brief Checks the exact predecessor shared by both streamed staging variants. */
        [[nodiscard]] bool IsCurrentTopology(const NavigationWorldActivationDescriptor &expected) const noexcept;
        /** @brief Checks that the named candidate replaces topology within the live world. */
        [[nodiscard]] bool IsStagedTopology(const NavigationWorldActivationDescriptor &candidate) const noexcept;
        /** @brief Publishes prevalidated detached state into the owner record. */
        void PublishStaged(bool deferCancellation = false,
                           NavigationWorldRevocation topologyReason = NavigationWorldRevocation::TopologyReplaced) noexcept;
        /** @brief Check whether replacing the active record preserves the configured retirement bound. */
        [[nodiscard]] bool CanRetireActive() const noexcept;
        /** @brief Permanently close admission and request cooperative cancellation for one record. */
        static void Revoke(const std::shared_ptr<Detail::NavigationWorldRecord> &record, NavigationWorldRevocation reason) noexcept;
        /** @brief Move the active owner pin into retirement when worker leases still exist. */
        void RetireActive(NavigationWorldRevocation reason, bool deferCancellation = false) noexcept;

        std::uint32_t maximumRetiredWorlds_{};
        std::shared_ptr<Detail::NavigationWorldRecord> staged_;
        std::shared_ptr<Detail::NavigationWorldRecord> active_;
        std::vector<std::shared_ptr<Detail::NavigationWorldRecord>> retired_;
        NavigationGeneration lastStagedTopology_; /**< Retains the active world's newest issued candidate across rollback. */
        NavigationWorldLifecycleState state_{NavigationWorldLifecycleState::Empty};
    };
}  // namespace Horo::Navigation
