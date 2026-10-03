#pragma once

/**
 * @file StreamingCellAssetRequest.h
 * @brief Bounded asynchronous cell-asset requests with structured cancellation.
 */

#include "Horo/Assets/AssetRegistry.h"
#include "Horo/WorldStreaming/StreamingCellCandidate.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Horo::Assets {
    class AssetLoadService;
}

namespace Horo::WorldStreaming {
    namespace Detail {
        /** @brief Tag keeping cell-asset request identities distinct from other counters. */
        struct StreamingCellAssetRequestIdTag;
    }  // namespace Detail

    /** @brief Stable process-local identity for one admitted cell asset request tree. */
    using StreamingCellAssetRequestId =
        Foundation::Detail::NonZeroId64<Detail::StreamingCellAssetRequestIdTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Admission lifecycle for a cell asset request owner. */
    enum class StreamingCellAssetRequestLifecycle : std::uint8_t {
        Active,
        Cancelling,
        Closed,
        Count
    };

    /** @brief Observable aggregate state; cancellation is not terminal until every child acknowledges. */
    enum class StreamingCellAssetRequestState : std::uint8_t {
        Loading,
        Cancelling,
        Ready,
        Failed,
        Cancelled
    };

    /** @brief Exact request identity, generation fence, and bounded child-request ceiling. */
    struct StreamingCellAssetRequestContext final {
        StreamingCellAssetRequestId request{};    /**< Stable aggregate request identity. */
        StreamingCellOperationHandle operation{}; /**< Exact cell operation and generation fence. */
        std::size_t maximumRequests{};            /**< Positive maximum including the candidate cell. */
        StreamingCellAssetRequestLifecycle lifecycle{StreamingCellAssetRequestLifecycle::Closed}; /**< Owner admission state. */
    };

    /** @brief One complete provider-owned byte result retained by the aggregate. */
    struct StreamingCellAssetBytes final {
        Assets::AssetId asset{};         /**< Exact requested cooked artifact identity. */
        std::vector<std::uint8_t> bytes; /**< Complete owned cooked bytes. */
    };

    /** @brief Move-only terminal success for one exact request tree. */
    struct StreamingCellAssetBatch final {
        StreamingCellAssetRequestId request{};            /**< Exact aggregate request. */
        StreamingCellOperationHandle operation{};         /**< Exact publication fence. */
        Assets::AssetRegistryRevision registryRevision{}; /**< Submission-time registry revision. */
        std::vector<StreamingCellAssetBytes> assets;      /**< Canonically ordered candidate then dependencies. */
    };

    /** @brief Move-only owner/controller for a bounded asynchronous cell asset request tree. */
    class StreamingCellAssetRequest final {
    public:
        /** @brief Creates an empty, non-controllable request handle. */
        StreamingCellAssetRequest() = default;
        /** @brief Requests cancellation when this handle still owns an admitted tree. */
        ~StreamingCellAssetRequest();
        StreamingCellAssetRequest(const StreamingCellAssetRequest &) = delete;
        StreamingCellAssetRequest &operator=(const StreamingCellAssetRequest &) = delete;
        StreamingCellAssetRequest(StreamingCellAssetRequest &&) noexcept = default;
        /** @brief Cancels the currently owned tree before taking ownership from another handle. @param other Source handle. @return This
         * handle. */
        StreamingCellAssetRequest &operator=(StreamingCellAssetRequest &&other) noexcept;

        /** @brief Returns the latest aggregate state without blocking. @return Loading, cancelling, or terminal state. */
        [[nodiscard]] StreamingCellAssetRequestState State() const;
        /**
         * @brief Requests cancellation for the root and every admitted child before terminal consumption; idempotent while retained.
         * @return Success, lifecycle-unavailable for a moved-from handle, or CellAssetRequestConsumed after result consumption.
         * @post An already consumed terminal result remains unchanged.
         */
        [[nodiscard]] Result<void> RequestCancel();
        /**
         * @brief Consumes the aggregate terminal result exactly once without blocking.
         * @return Owned canonical bytes, preserved child error, CellAssetRequestCancelled for suppressed ready bytes, or typed state error.
         * @details Cancellation accepted before consumption suppresses success even when every child read already succeeded.
         */
        [[nodiscard]] Result<StreamingCellAssetBatch> TakeResult();

    private:
        struct StateData;
        friend Result<StreamingCellAssetRequest> RequestStreamingCellAssets(Assets::AssetLoadService &,
                                                                            const Assets::AssetRegistrySnapshot &,
                                                                            const CookedWorldIndexManifest &,
                                                                            const StreamingCellCandidate &,
                                                                            const StreamingCellAssetRequestContext &);
        explicit StreamingCellAssetRequest(std::shared_ptr<StateData> state) noexcept;
        std::shared_ptr<StateData> state_;
    };

    /**
     * @brief Submits the candidate cell and every hard dependency beneath one cancellation root.
     * @param service Borrowed asset-load service that outlives all admitted child work.
     * @param registry Immutable asset registry captured for every child request.
     * @param manifest Immutable cell-to-package authority used to resolve dependency assets.
     * @param candidate Immutable generation-pinned preparation result.
     * @param context Exact aggregate identity, operation fence, lifecycle, and request ceiling.
     * @return Move-only aggregate request or a typed invalid, stale, unavailable, capacity, lifecycle, or admission error.
     * @post Failure publishes no aggregate; any partially admitted children are cancellation-requested.
     */
    [[nodiscard]] Result<StreamingCellAssetRequest> RequestStreamingCellAssets(Assets::AssetLoadService &service,
                                                                               const Assets::AssetRegistrySnapshot &registry,
                                                                               const CookedWorldIndexManifest &manifest,
                                                                               const StreamingCellCandidate &candidate,
                                                                               const StreamingCellAssetRequestContext &context);
}  // namespace Horo::WorldStreaming
