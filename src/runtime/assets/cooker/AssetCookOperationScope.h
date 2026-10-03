#pragma once

/** @file AssetCookOperationScope.h
 * @brief Target-private ownership of one cook operation and its terminal Build Output projection. */

#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/Result.h"

#include <optional>
#include <string>

namespace Horo::Assets::Detail {
    /** @brief Publishes exactly one terminal result while the caller owns and joins all cook work.
     * @pre Borrowed stores and cancellation token outlive this scope and every submitted child. */
    class CookOperationScope final {
    public:
        /** @brief Captures host-owned publication endpoints and an already-admitted operation identity.
         * @param store Optional operation projection store.
         * @param output Optional Build Output destination.
         * @param cancellation Parent token retained until scope destruction.
         * @param sessionId Admitted Build Output session.
         * @param id Optional admitted operation identity. */
        CookOperationScope(OperationStore *store, BuildOutputStore *output, const CancellationToken &cancellation,
                           BuildOutputSessionId sessionId, std::optional<OperationId> id);
        /** @brief Publishes Failed or Cancelled when success has not explicitly completed the scope. */
        ~CookOperationScope();
        CookOperationScope(const CookOperationScope &) = delete;
        CookOperationScope &operator=(const CookOperationScope &) = delete;

        /** @brief Captures the joined outcome independently from a subsequently cancelled parent token.
         * @param cancelled Whether the authoritative result is cancellation. */
        void RecordOutcome(bool cancelled);
        /** @brief Determines terminal classification from the retained typed failure.
         * @param error Authoritative failure with domain and cancellation cause. */
        void RecordError(const Error &error);
        /** @brief Projects bounded cook progress to the admitted operation.
         * @param phase Current named phase.
         * @param message Contextual progress text.
         * @param progress Monotonic progress supplied by the coordinator. */
        void Update(std::string phase, std::string message, float progress);
        /** @brief Publishes successful completion and suppresses destructor failure projection.
         * @param message Completion text. */
        void Succeed(std::string message);
        /** @brief Adds authoritative session/operation identity and appends an owned record.
         * @param record Domain finding or per-asset result. */
        void Publish(BuildOutputRecord record) const;

    private:
        OperationStore *store_{};
        BuildOutputStore *output_{};
        const CancellationToken *cancellation_{};
        BuildOutputSessionId sessionId_;
        std::optional<OperationId> id_;
        bool completed_{false};
        std::optional<bool> cancelledResult_;
    };
}  // namespace Horo::Assets::Detail
