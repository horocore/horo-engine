#pragma once

/**
 * @file AssetCookOperation.h
 * @brief Target-private cook operation lifecycle and optional history projection.
 */

#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/CancellationToken.h"

#include <optional>
#include <string>

namespace Horo::Assets {

    /**
     * @brief Owns one admitted cook operation's terminal projection until its scope ends.
     * @details The cook control thread exclusively owns this scope. Borrowed stores and
     *          cancellation must outlive it; accepted cook jobs drain before it is destroyed.
     *          An irreversible publication takes precedence over later cancellation or
     *          optional history failures. Destruction contains standard and foreign exceptions.
     */
    class AssetCookOperation final {
    public:
        /**
         * @brief Borrows projection authorities for an already admitted cook operation.
         * @param store Optional operation authority that outlives this scope.
         * @param output Optional build-output authority that outlives this scope.
         * @param cancellation Cancellation token that outlives this scope.
         * @param sessionId Previously allocated output identity, or invalid when output is absent.
         * @param id Previously admitted operation identity, or no value when store is absent.
         */
        AssetCookOperation(OperationStore *store, BuildOutputStore *output, const CancellationToken &cancellation,
                           BuildOutputSessionId sessionId, std::optional<OperationId> id);

        /** @brief Attempts the remaining terminal projection without allowing notification exceptions to escape. */
        ~AssetCookOperation() noexcept;

        AssetCookOperation(const AssetCookOperation &) = delete;
        AssetCookOperation &operator=(const AssetCookOperation &) = delete;

        /**
         * @brief Records the precommit terminal classification for cleanup.
         * @param cancelled Whether the result represents cancellation rather than failure.
         */
        void RecordOutcome(bool cancelled);

        /**
         * @brief Records irreversible publication before optional success projections run.
         * @pre The durable publication authority has confirmed current-pointer replacement.
         * @post Cleanup can only project success, regardless of later cancellation or errors.
         */
        void RecordCommitted() noexcept;

        /**
         * @brief Classifies a typed cook cancellation error for precommit cleanup.
         * @param error Error whose domain and code determine cancellation classification.
         */
        void RecordError(const Error &error);

        /**
         * @brief Updates the optional operation authority with a running stage.
         * @param phase Stable stage identifier.
         * @param message User-facing progress message.
         * @param progress Completion fraction for the operation.
         * @throws Propagates allocation or store exceptions before terminal success.
         */
        void Update(std::string phase, std::string message, float progress);

        /**
         * @brief Independently attempts successful output and operation projections, containing all exceptions.
         * @param message Success description shared by both optional projections.
         * @post The scope is complete after both projections have been attempted.
         */
        void Succeed(std::string message) noexcept;

        /**
         * @brief Appends a record correlated with this cook's identities when output is present.
         * @param record Owned diagnostic to append.
         * @throws Propagates allocation or output-store exceptions to the owning control scope.
         */
        void Publish(BuildOutputRecord record) const;

    private:
        /** @brief Selects committed success or the recorded precommit outcome when no terminal projection was attempted. */
        void FinalizeProjection();

        /** @brief Projects precommit cancellation or failure without rewriting committed success. */
        void PublishUncommittedOutcome();

        OperationStore *store_{};
        BuildOutputStore *output_{};
        const CancellationToken *cancellation_{};
        BuildOutputSessionId sessionId_;
        std::optional<OperationId> id_;
        bool completed_{false};
        bool committed_{false};
        std::optional<bool> cancelledResult_;
    };

}  // namespace Horo::Assets
