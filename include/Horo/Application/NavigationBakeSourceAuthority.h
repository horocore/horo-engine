#pragma once

/** @file NavigationBakeSourceAuthority.h
 * @brief Nonblocking host source-revision authority and guarded navigation artifact adoption.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Navigation/NavigationBakeInput.h"

#include <memory>
#include <mutex>

namespace Horo::Application {
    namespace NavigationBakeDetail {
        struct SourceState;
    }

    /** @brief Move-only guard that prevents authoritative source revisions changing during pointer adoption.
     * @note A background publication worker owns this guard through AssetCook replacement and live receipt adoption.
     * Host source mutation uses UpdateCurrent and is rejected without waiting while a guard exists.
     * Acquisition, ownership and destruction remain on the same thread; a lease cannot migrate between workers.
     */
    class NavigationBakeSourceLease final {
    public:
        NavigationBakeSourceLease(NavigationBakeSourceLease &&) noexcept = default;
        NavigationBakeSourceLease &operator=(NavigationBakeSourceLease &&) = delete;
        NavigationBakeSourceLease(const NavigationBakeSourceLease &) = delete;
        NavigationBakeSourceLease &operator=(const NavigationBakeSourceLease &) = delete;

    private:
        friend class NavigationBakeSourceAuthority;
        /** @brief Retains authoritative evidence and its already-acquired lock on the acquiring thread.
         * @param state Shared evidence owner that must outlive lock release.
         * @param lock Acquired publication guard.
         */
        NavigationBakeSourceLease(std::shared_ptr<NavigationBakeDetail::SourceState> state, std::unique_lock<std::mutex> lock) noexcept;
        std::shared_ptr<NavigationBakeDetail::SourceState> state_;
        std::unique_lock<std::mutex> lock_;
    };

    /** @brief Host-composed revision authority shared by source transactions and background navigation publication.
     * @note Every source/definition/Scene/catalog/profile/coordinate mutation must first UpdateCurrent with the
     * complete proposed revision evidence, and may become current only on success. A busy result means defer the
     * source transaction; no GUI/render thread waits. The mutex protects only this evidence and the bounded final
     * disk adoption interval, never voxelization or cache work. Authority storage outlives its leases by ownership.
     */
    class NavigationBakeSourceAuthority final {
    public:
        /** @brief Creates an initially closed authority; UpdateCurrent supplies its first coherent source revision. */
        NavigationBakeSourceAuthority();

        /** @brief Replaces complete authoritative revision evidence without waiting for an active adoption.
         * @param revisions Exact current revisions, separate from dependency compatibility hashes.
         * @param sources Complete current source identity/revision/digest observations.
         * @return Success, typed contention, or invalid/capacity failure; failure leaves current evidence unchanged.
         */
        [[nodiscard]] Result<void> UpdateCurrent(const Navigation::NavigationBakeInputRevisions &revisions,
                                                 std::vector<Navigation::NavigationSourceObservation> sources);

        /** @brief Acquires a nonblocking adoption guard and verifies captured input against current evidence.
         * @param input Immutable candidate capture.
         * @param cancellation Operation cancellation checked while acquiring the guard.
         * @return Guard held through commit, or typed stale/cancelled/contention failure.
         */
        [[nodiscard]] Result<NavigationBakeSourceLease> TryAcquirePublication(const Navigation::NavigationBakeInputSnapshot &input,
                                                                              const CancellationToken &cancellation = {}) const;

    private:
        std::shared_ptr<NavigationBakeDetail::SourceState> state_;
    };
}  // namespace Horo::Application
