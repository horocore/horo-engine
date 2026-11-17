#pragma once

/**
 * @file AssetProvider.h
 * @brief Backend-neutral cooked-byte providers and bounded asynchronous loading.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/JobSystem.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace Horo::Assets {
    /** @brief Provider limits applied before allocating an owned cooked payload. */
    struct AssetProviderLimits {
        std::size_t maximumAssetBytes{256U * 1024U * 1024U};
    };

    /** @brief Source-agnostic cooked byte provider. Implementations are safe for concurrent loads. */
    class IAssetProvider {
    public:
        virtual ~IAssetProvider() = default;
        /** @brief Tests whether a cooked artifact exists. @param id Stable identity. @param cancellation Cooperative
         * cancellation token. @return Existence or a typed provider/cancellation error. */
        [[nodiscard]] virtual Result<bool> Exists(AssetId id, const CancellationToken &cancellation) const = 0;
        /** @brief Loads a complete owned cooked payload. @param id Stable identity. @param cancellation Cooperative
         * cancellation token. @return Owned bytes or a typed provider/cancellation error. */
        [[nodiscard]] virtual Result<std::vector<std::uint8_t>> Load(AssetId id, const CancellationToken &cancellation) const = 0;
    };

    /** @brief Development provider resolving canonical AssetId filenames below one cooked root. */
    class FilesystemAssetProvider final : public IAssetProvider {
    public:
        /** @brief Creates a provider rooted at one cooked-artifact directory. @param cookedRoot Native root path.
         * @param limits Allocation bounds checked before reading. */
        explicit FilesystemAssetProvider(std::filesystem::path cookedRoot, AssetProviderLimits limits = {});
        [[nodiscard]] Result<bool> Exists(AssetId id, const CancellationToken &cancellation) const override;
        [[nodiscard]] Result<std::vector<std::uint8_t>> Load(AssetId id, const CancellationToken &cancellation) const override;

    private:
        std::filesystem::path cookedRoot_;
        AssetProviderLimits limits_;
    };

    /** @brief Concurrent deterministic provider for headless hosts and tests. */
    class MemoryAssetProvider final : public IAssetProvider {
    public:
        MemoryAssetProvider();
        ~MemoryAssetProvider() override;
        MemoryAssetProvider(const MemoryAssetProvider &) = delete;
        MemoryAssetProvider &operator=(const MemoryAssetProvider &) = delete;
        /** @brief Inserts or replaces one deterministic payload. @param id Stable identity. @param bytes Owned payload. */
        void Insert(AssetId id, std::vector<std::uint8_t> bytes);
        /** @brief Removes one payload if present. @param id Stable identity. */
        void Remove(AssetId id);
        [[nodiscard]] Result<bool> Exists(AssetId id, const CancellationToken &cancellation) const override;
        [[nodiscard]] Result<std::vector<std::uint8_t>> Load(AssetId id, const CancellationToken &cancellation) const override;

    private:
        struct State;
        std::unique_ptr<State> state_;
    };

    /** @brief Terminal payload tagged with the registry revision captured at submission.
     * @details Default and move construction may allocate checked-library destination bookkeeping.
     * A failed move construction preserves the source payload; successful moves transfer its storage. */
    struct AssetLoadResult {
        /** @brief Creates an empty envelope. @throws std::bad_alloc If destination bookkeeping cannot be allocated. */
        AssetLoadResult() = default;
        /** @brief Copies an envelope and its payload. @param other Envelope to copy.
         * @throws std::bad_alloc If the payload or destination bookkeeping cannot be allocated. */
        AssetLoadResult(const AssetLoadResult &other) = default;
        /** @brief Copies an envelope into existing storage. @param other Envelope to copy. @return This envelope.
         * @throws std::bad_alloc If the copied payload requires additional storage. */
        AssetLoadResult &operator=(const AssetLoadResult &other) = default;
        /** @brief Transfers an envelope after destination bookkeeping is prepared. @param other Envelope to transfer.
         * @throws std::bad_alloc If destination preparation fails, leaving other unchanged. */
        AssetLoadResult(AssetLoadResult &&other);
        /** @brief Transfers a payload into existing destination storage. @param other Envelope to transfer.
         * @return This envelope. */
        AssetLoadResult &operator=(AssetLoadResult &&other) noexcept = default;
        /** @brief Creates a terminal envelope by copying its payload.
         * @param selected Loaded asset identity. @param revision Registry revision captured at submission.
         * @param payload Payload to copy. @throws std::bad_alloc If payload storage cannot be allocated. */
        AssetLoadResult(AssetId selected, AssetRegistryRevision revision, const std::vector<std::uint8_t> &payload);
        /** @brief Creates a terminal envelope by transferring its payload after destination preparation.
         * @param selected Loaded asset identity. @param revision Registry revision captured at submission.
         * @param payload Payload whose storage is transferred. @throws std::bad_alloc If destination preparation fails,
         * leaving payload unchanged. */
        AssetLoadResult(AssetId selected, AssetRegistryRevision revision, std::vector<std::uint8_t> &&payload);

        AssetId id;
        AssetRegistryRevision sourceRegistryRevision;
        std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(std::size_t{0});
    };

    /** @brief Observable lifecycle state of one accepted asynchronous load. */
    enum class AssetLoadState : std::uint8_t {
        Queued,
        Running,
        Succeeded,
        Failed,
        Cancelled
    };

    /** @brief Move-only observation and cancellation handle for one accepted asset load. */
    class AssetLoadHandle final {
    public:
        AssetLoadHandle() = default;
        AssetLoadHandle(const AssetLoadHandle &) = delete;
        AssetLoadHandle &operator=(const AssetLoadHandle &) = delete;
        AssetLoadHandle(AssetLoadHandle &&) noexcept = default;
        AssetLoadHandle &operator=(AssetLoadHandle &&) noexcept = default;
        /** @brief Observes the current request state without blocking. @return Current lifecycle state. */
        [[nodiscard]] AssetLoadState State() const noexcept;
        /** @brief Requests cooperative cancellation. @return Success when cancellation was requested or the request was
         * already terminal; a typed shutdown error for an empty handle. */
        [[nodiscard]] Result<void> RequestCancel();
        /** @brief Waits until the scheduled request reaches a terminal state. @return Success for completed or cancelled
         * work, or a typed job-system failure. */
        [[nodiscard]] Result<void> Wait() const;
        /** @brief Consumes the terminal result exactly once. @return Loaded payload or typed not-ready/cancel/failure.
         * @throws std::bad_alloc If preparing the transferred result fails. A terminal consumption attempt remains consumed;
         * callers must handle allocation failure at their fallible admission boundary. */
        [[nodiscard]] Result<AssetLoadResult> TakeResult();

    private:
        struct Request;
        friend class AssetLoadService;

        explicit AssetLoadHandle(std::shared_ptr<Request> request) noexcept : request_(std::move(request)) {}

        std::shared_ptr<Request> request_;
    };

    /** @brief Service-owned bounded request group scheduled through the process JobSystem. */
    class AssetLoadService final {
    public:
        /** @brief Creates a bounded asynchronous request group. @param jobs Borrowed job system that outlives the service.
         * @param provider Borrowed concurrent provider that outlives the service. @param maximumOutstanding Maximum
         * accepted non-terminal service-owned requests. */
        AssetLoadService(JobSystem &jobs, const IAssetProvider &provider, std::size_t maximumOutstanding = 128);
        ~AssetLoadService();
        AssetLoadService(const AssetLoadService &) = delete;
        AssetLoadService &operator=(const AssetLoadService &) = delete;
        /** @brief Submits a load pinned to one immutable registry revision. @param snapshot Registry state to capture.
         * @param id Registered identity to load. @return Move-only request handle or a typed missing/queue/shutdown error.
         */
        [[nodiscard]] Result<AssetLoadHandle> LoadAsync(const AssetRegistrySnapshot &snapshot, AssetId id);
        /** @brief Submits a load beneath an explicit cooperative cancellation parent. @param snapshot Registry state to
         * capture. @param id Registered identity to load. @param parentCancellation Parent operation cancellation ancestry.
         * @return Move-only request handle or a typed missing, queue, cancellation, or shutdown error. */
        [[nodiscard]] Result<AssetLoadHandle> LoadAsync(const AssetRegistrySnapshot &snapshot, AssetId id,
                                                        const CancellationToken &parentCancellation);
        /** @brief Stops admission, cancels and joins service-owned requests, and is safe to call repeatedly. */
        void Shutdown() noexcept;

    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Assets
