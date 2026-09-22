#pragma once

/**
 * @file UiAssetLoading.h
 * @brief Typed Runtime UI cooked-asset resolution over the shared Assets provider.
 */

#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Runtime/Ui/UiDocument.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Horo::Runtime::Ui {
    /** @brief Maximum generic cooked artifact size admitted by the Runtime UI loader by default. */
    inline constexpr std::size_t MaximumUiRuntimeArtifactBytes = 256ULL * 1024ULL * 1024ULL;

    /** @brief Bounded limits for one asynchronous Runtime UI document and dependency preparation. */
    struct UiRuntimeAssetLoadLimits final {
        std::size_t maximumOutstanding{64};
        std::size_t maximumConcurrentLoads{8};
        std::size_t maximumResidentBytes{256ULL * 1024ULL * 1024ULL};
        std::size_t maximumArtifactBytes{MaximumUiRuntimeArtifactBytes};
        UiDocumentCookLimits cookedDocument;

        /** @brief Checks that every load bound is positive and no cooked bound weakens a compiled ceiling. @return Whether usable. */
        [[nodiscard]] bool IsValid() const noexcept {
            return maximumOutstanding > 0 && maximumConcurrentLoads > 0 && maximumResidentBytes > 0 && maximumArtifactBytes > 0 &&
                   maximumArtifactBytes <= MaximumUiRuntimeArtifactBytes && cookedDocument.IsValid() &&
                   cookedDocument.maximumPayloadBytes <= maximumResidentBytes;
        }
    };

    /** @brief Immutable request for one exact cooked UI canvas and its dependency closure. */
    struct UiRuntimeAssetLoadRequest final {
        UiCanvasAssetReference canvas;           /**< Stable root asset, document, canvas, and minimum revision evidence. */
        Assets::AssetTypeId expectedAssetType;   /**< Optional registry type assertion; empty adopts the captured record type. */
        std::optional<AssetCookTargetId> target; /**< Optional exact packaged cook target. */
    };

    /** @brief Terminal result of one prepared Runtime UI asset closure. */
    struct UiRuntimeAssetLoadResult final {
        Assets::AssetId rootAsset;                      /**< Stable root asset identity. */
        Assets::AssetRegistryRevision registryRevision; /**< Registry publication pinned for the complete closure. */
        CookedUiDocument document;                      /**< Owned immutable cooked document. */
        std::vector<UiRuntimeAsset> assets;             /**< Owned immutable required/available optional dependency leases. */

        /**
         * @brief Transfers the prepared closure into one mutable runtime instance.
         * @param instance Owner-issued transient Runtime UI instance identity.
         * @return Prepared instance or a typed identity/dependency failure.
         */
        [[nodiscard]] Result<UiRuntimeInstance> CreateInstance(RuntimeUiInstanceId instance) &&;
    };

    /** @brief Observable lifecycle state of one Runtime UI asset preparation. */
    enum class UiRuntimeAssetLoadState : std::uint8_t {
        Queued,
        LoadingDocument,
        LoadingDependencies,
        Succeeded,
        Failed,
        Cancelled,
    };

    /** @brief Move-only handle for one cancellation-safe, registry-pinned Runtime UI asset preparation. */
    class UiRuntimeAssetLoadHandle final {
    public:
        /** @brief Opaque implementation record; callers only retain the owning handle. */
        struct Request;

        UiRuntimeAssetLoadHandle() = default;
        UiRuntimeAssetLoadHandle(const UiRuntimeAssetLoadHandle &) = delete;
        UiRuntimeAssetLoadHandle &operator=(const UiRuntimeAssetLoadHandle &) = delete;
        UiRuntimeAssetLoadHandle(UiRuntimeAssetLoadHandle &&) noexcept = default;
        UiRuntimeAssetLoadHandle &operator=(UiRuntimeAssetLoadHandle &&) noexcept = default;

        /** @brief Observes the current state without blocking or performing provider I/O. @return Current state. */
        [[nodiscard]] UiRuntimeAssetLoadState State() const noexcept;
        /** @brief Requests cancellation of the root and every admitted dependency load. @return Typed shutdown/error state. */
        [[nodiscard]] Result<void> RequestCancel();
        /**
         * @brief Waits for terminal preparation while advancing only this request.
         * @details This is an explicit load-time operation and must not be called from a frame-hot path.
         * @return Success once the operation is terminal, or a typed shutdown/wait failure.
         */
        [[nodiscard]] Result<void> Wait() const;
        /**
         * @brief Consumes the complete terminal result exactly once.
         * @details Polling is non-blocking; callers may call the owning service's Advance between polls.
         * @return Owned cooked document and dependency leases, or a typed not-ready/cancel/failure result.
         */
        [[nodiscard]] Result<UiRuntimeAssetLoadResult> TakeResult();

    private:
        friend class UiRuntimeAssetLoadService;

        explicit UiRuntimeAssetLoadHandle(std::shared_ptr<Request> request) noexcept : request_(std::move(request)) {}

        std::shared_ptr<Request> request_;
    };

    /** @brief Owner-thread pump that composes AssetLoadService results into one atomic Runtime UI closure. */
    class UiRuntimeAssetLoadService final {
    public:
        /** @brief Opaque implementation state; callers must not construct or access it. */
        struct State;

        /**
         * @brief Creates a loader over borrowed shared asset services.
         * @param registry Registry whose immutable snapshots pin root and dependency type evidence.
         * @param loads Shared Assets load service; it must outlive this loader.
         * @param limits Bounded outstanding, concurrency, artifact, and resident-memory limits.
         */
        UiRuntimeAssetLoadService(Assets::AssetRegistry &registry, Assets::AssetLoadService &loads, UiRuntimeAssetLoadLimits limits = {});
        ~UiRuntimeAssetLoadService();
        UiRuntimeAssetLoadService(const UiRuntimeAssetLoadService &) = delete;
        UiRuntimeAssetLoadService &operator=(const UiRuntimeAssetLoadService &) = delete;

        /**
         * @brief Captures the current registry revision and submits one root UI asset.
         * @param request Stable root canvas and optional package target/type evidence.
         * @param parentCancellation Cooperative cancellation ancestry.
         * @return Move-only request handle or typed validation/admission failure.
         */
        [[nodiscard]] Result<UiRuntimeAssetLoadHandle> LoadAsync(UiRuntimeAssetLoadRequest request,
                                                                 const CancellationToken &parentCancellation = {});

        /**
         * @brief Submits against an explicitly captured immutable registry snapshot.
         * @param snapshot Registry publication used for every root and dependency resolution.
         * @param request Stable root canvas and optional package target/type evidence.
         * @param parentCancellation Cooperative cancellation ancestry.
         * @return Move-only request handle or typed validation/admission failure.
         */
        [[nodiscard]] Result<UiRuntimeAssetLoadHandle> LoadAsync(const Assets::AssetRegistrySnapshot &snapshot,
                                                                 UiRuntimeAssetLoadRequest request,
                                                                 const CancellationToken &parentCancellation = {});

        /**
         * @brief Advances every admitted request without blocking or performing file I/O on the caller thread.
         * @details It consumes completed shared AssetLoadService results, validates envelopes and admits bounded dependency waves.
         * @return Success; per-request failures are returned by each handle's TakeResult.
         */
        [[nodiscard]] Result<void> Advance();
        /** @brief Cancels, joins and forgets every request; repeated shutdown is harmless. */
        void Shutdown() noexcept;

    private:
        std::shared_ptr<State> state_;
    };
}  // namespace Horo::Runtime::Ui
