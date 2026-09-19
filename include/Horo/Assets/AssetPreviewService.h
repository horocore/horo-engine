#pragma once

/**
 * @file AssetPreviewService.h
 * @brief Bounded asynchronous host lifecycle for rendering-neutral asset previews.
 */

#include "Horo/Assets/AssetPreview.h"
#include "Horo/Foundation/JobSystem.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace Horo::Assets {
    /** @brief Resource limits enforced before preview work is admitted or cached. */
    struct AssetPreviewServiceLimits {
        std::size_t maximumOutstanding{128};                 /**< Maximum accepted non-terminal requests. */
        std::size_t maximumInputBytes{64U * 1024U * 1024U};  /**< Maximum source payload read for one request. */
        std::uint32_t maximumDimension{512};                 /**< Maximum requested width or height. */
        std::size_t maximumCacheEntries{256};                /**< Maximum retained successful preview images. */
        std::size_t maximumCacheBytes{128U * 1024U * 1024U}; /**< Maximum retained RGBA8 bytes. */
    };

    /** @brief Owned request submitted to the host preview scheduler. */
    struct AssetPreviewRequest {
        std::string contributionId;                            /**< Stable provider contribution identity used in the cache key. */
        std::string moduleId;                                  /**< Stable owning module identity used in the cache key. */
        std::string moduleVersion;                             /**< Owning module version used in the cache key. */
        std::string providerVersion;                           /**< Provider version used in the cache key. */
        std::filesystem::path absoluteAssetPath;               /**< Absolute imported editor-payload path. */
        AssetTypeId assetType;                                 /**< Stable type produced by the importer. */
        std::uint32_t width{128};                              /**< Requested output width. */
        std::uint32_t height{128};                             /**< Requested output height. */
        std::shared_ptr<const IAssetPreviewProvider> provider; /**< Provider lease retained through terminal completion. */
    };

    /** @brief Terminal preview image and whether provider execution was avoided by a cache hit. */
    struct AssetPreviewResult {
        AssetPreviewImage image;
        bool cacheHit{false};
    };

    /** @brief Observable lifecycle state of one accepted preview request. */
    enum class AssetPreviewState : std::uint8_t {
        Queued,
        Running,
        Succeeded,
        Failed,
        Cancelled,
    };

    /** @brief Move-only observation and cancellation handle for one preview request. */
    class AssetPreviewHandle final {
    public:
        AssetPreviewHandle() = default;
        AssetPreviewHandle(const AssetPreviewHandle &) = delete;
        AssetPreviewHandle &operator=(const AssetPreviewHandle &) = delete;
        AssetPreviewHandle(AssetPreviewHandle &&) noexcept = default;
        AssetPreviewHandle &operator=(AssetPreviewHandle &&) noexcept = default;

        /** @brief Returns the current lifecycle state without blocking. */
        [[nodiscard]] AssetPreviewState State() const noexcept;
        /** @brief Requests cooperative cancellation of queued or running work. */
        [[nodiscard]] Result<void> RequestCancel();
        /** @brief Waits for this request to reach a terminal state. */
        [[nodiscard]] Result<void> Wait() const;
        /** @brief Consumes the terminal result exactly once. */
        [[nodiscard]] Result<AssetPreviewResult> TakeResult();

    private:
        struct Request;
        friend class AssetPreviewService;

        explicit AssetPreviewHandle(std::shared_ptr<Request> request) noexcept : request_(std::move(request)) {}

        std::shared_ptr<Request> request_;
    };

    /** @brief Host-owned bounded scheduler and in-memory cache for asset previews. */
    class AssetPreviewService final {
    public:
        /**
         * @brief Creates a preview service using a process-owned job system.
         * @param jobs Job system that outlives this service.
         * @param limits Admission, payload, output, and cache bounds.
         */
        explicit AssetPreviewService(JobSystem &jobs, AssetPreviewServiceLimits limits = {});
        ~AssetPreviewService();
        AssetPreviewService(const AssetPreviewService &) = delete;
        AssetPreviewService &operator=(const AssetPreviewService &) = delete;

        /**
         * @brief Validates and schedules one provider invocation or cache lookup.
         * @param request Owned provider identity, path, dimensions, and provider lease.
         * @param parentCancellation Optional owning-operation cancellation ancestry.
         * @return Accepted request handle or a typed validation, capacity, or shutdown error.
         */
        [[nodiscard]] Result<AssetPreviewHandle> Submit(AssetPreviewRequest request, const CancellationToken &parentCancellation = {});

        /** @brief Stops admission, cancels and joins owned requests, and clears cached previews. */
        void Shutdown() noexcept;

    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Assets
