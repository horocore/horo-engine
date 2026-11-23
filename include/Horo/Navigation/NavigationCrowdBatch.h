#pragma once

/** @file NavigationCrowdBatch.h
 * @brief Owned spatial crowd work and one-time fixed-tick collection.
 */

#include "Horo/Foundation/JobSystem.h"
#include "Horo/Navigation/NavigationCrowdAvoidance.h"

namespace Horo::Navigation {
    /** @brief Recorded same-build, same-platform qualification supplied by the host for one exact provider and profile. */
    struct NavigationCrowdQualification final {
        Sha256Digest provider;
        Sha256Digest buildPlatform;
        Sha256Digest goldenEvidence;
        Sha256Digest topologyContent;
        Sha256Digest profile;
        float fixedStepSeconds{};
        std::uint32_t maximumAgents{};

        bool operator==(const NavigationCrowdQualification &) const noexcept = default;
    };

    /** @brief Explicit qualification boundary. Best-effort providers implement only INavigationCrowdBackend. */
    class INavigationQualifiedCrowdBackend : public INavigationCrowdBackend {
    public:
        /** @brief Reports the immutable qualification record for this provider instance. */
        [[nodiscard]] virtual const NavigationCrowdQualification &Qualification() const noexcept = 0;
    };

    /** @brief One solver result addressed to the request's index in the pinned crowd snapshot. */
    struct NavigationCrowdBatchResult final {
        std::size_t agentIndex{};
        NavigationAvoidanceOutcome outcome;
    };

    /** @brief One bounded batch owned by the Navigation publication lane. Workers share only detached values. */
    class NavigationCrowdBatch final {
        struct State;

        class ConstructionKey {
            friend class NavigationCrowdBatch;
            ConstructionKey() = default;
        };

    public:
        /** @brief Constructs the value for the checked factory. @param state Owned detached work and cancellation scope. */
        explicit NavigationCrowdBatch(ConstructionKey, std::shared_ptr<State> state) noexcept;
        /** @brief Cancels outstanding jobs and withdraws all results without joining. */
        ~NavigationCrowdBatch();
        NavigationCrowdBatch(const NavigationCrowdBatch &) = delete;
        NavigationCrowdBatch &operator=(const NavigationCrowdBatch &) = delete;

        /** @brief Enqueues spatial groups in the capture's stable cell and agent order.
         * @param jobs Existing host-owned CPU scheduler.
         * @param capture Exact best-effort snapshot pinned until collection or worker retirement.
         * @param requests Nonempty set of distinct agent indices with one shared fixed step and finite planar solver envelopes.
         * @param providers Distinct, host-selected bounded solver instances. The selected prefix caps job count.
         * @param cancellation Scene lifetime or superseding operation cancellation.
         * @return Owner handle or an admission error. On partial admission, cancellation withdraws the whole batch.
         * @pre Called on the publication lane. Each provider lease stays exclusive until retirement unless the provider supports overlap.
         */
        [[nodiscard]] static Result<std::unique_ptr<NavigationCrowdBatch>> Submit(
            const JobSystem &jobs, const NavigationCrowdSnapshot &capture, std::span<const NavigationAvoidanceRequest> requests,
            std::span<const std::shared_ptr<INavigationCrowdBackend>> providers, const CancellationToken &cancellation = {});

        /** @brief Evaluates every captured agent in stable cell and agent order on the deterministic owner.
         * @param capture Exact immutable fact capture with host-resolved topology and profile evidence.
         * @param requests Exactly one finite fixed-step request for each captured agent.
         * @param provider Host-composed qualified provider. Its numerical contract must match the admitted evidence.
         * @param admitted Verified qualification for the active build, platform, topology content and profile.
         * @param activeTopology Current topology generation from the publishing owner.
         * @param cancellation Scene lifetime or superseding operation cancellation.
         * @return Full ordered result set, or one typed failure without partial publication.
         * @pre Runs at the declared fixed tick on the deterministic owner. No background completion is consulted.
         */
        [[nodiscard]] static Result<std::vector<NavigationCrowdBatchResult>> RunQualified(
            const NavigationCrowdSnapshot &capture, std::span<const NavigationAvoidanceRequest> requests,
            INavigationQualifiedCrowdBackend &provider, const NavigationCrowdQualification &admitted, NavigationGeneration activeTopology,
            const CancellationToken &cancellation = {});

        /** @brief Collects complete results once in dispatch order without waiting on a job.
         * @param current Exact retained capture currently eligible for publication.
         * @param tick Navigation publication tick.
         * @return Complete ordered results, empty while pending, or the typed rejection with no partial results.
         * @details Callers still validate the exact live per-agent work and Character binding before publishing motion.
         */
        [[nodiscard]] Result<std::optional<std::vector<NavigationCrowdBatchResult>>> TryTake(const NavigationCrowdSnapshot &current,
                                                                                             std::uint64_t tick);

        /** @brief Withholds all results and cooperatively cancels the batch, including an unconsumed successful batch. */
        void Cancel() noexcept;

    private:
        /** @brief Schedules one job per selected provider over contiguous whole-cell ranges. */
        [[nodiscard]] Result<void> SubmitGroups(const JobSystem &jobs, std::span<const std::size_t> cellEnds,
                                                std::span<const std::shared_ptr<INavigationCrowdBackend>> providers);
        /** @brief Observes terminal states through the scheduler's synchronized job records. */
        [[nodiscard]] Result<bool> ObserveJobs() const;

        std::shared_ptr<State> state_;
        std::vector<JobHandle> jobs_;
    };
}  // namespace Horo::Navigation
