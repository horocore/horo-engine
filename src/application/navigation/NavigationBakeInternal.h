#pragma once

#include "Horo/Application/NavigationBakeService.h"
#include "Horo/Assets/AssetCookCache.h"
#include "Horo/Navigation/NavigationErrors.h"

#include <atomic>
#include <mutex>

namespace Horo::Application::NavigationBakeDetail {
    constexpr std::uint64_t Adopted = std::uint64_t{1} << 63U;

    /** @brief Owns the immutable publication lease and its narrowly guarded cross-thread handoff. */
    class PublicationStore {
    public:
        /** @brief Copies a complete immutable lease for host or worker readers under the publication guard. */
        [[nodiscard]] std::shared_ptr<const NavigationBakePublication> Load() const noexcept {
            const std::lock_guard lock(publicationMutex_);
            return published_;
        }

        /** @brief The sequential publication worker installs a complete lease; state ownership outlives accepted jobs. */
        void Store(std::shared_ptr<const NavigationBakePublication> publication) noexcept {
            const std::lock_guard lock(publicationMutex_);
            published_.swap(publication);
            // The replaced lease is released after the guard unlocks, outside the publication critical section.
        }

    private:
        // Guards only lease copy/swap between host readers and the sequential worker; shared state survives Close and job drain.
        mutable std::mutex publicationMutex_;
        std::shared_ptr<const NavigationBakePublication> published_;
    };

    /** @brief Shared worker lifetime; desired generation fences adoption without blocking the host thread. */
    struct ServiceState {
        NavigationBakeServiceConfig config;
        std::atomic<std::uint64_t> desired{};
        PublicationStore publication;
    };

    /** @brief One immutable source capture and stage-owned detached result; stages execute sequentially. */
    struct Attempt {
        NavigationBakeRequest request;
        std::uint64_t generation{};
        OperationId operation{};
        std::shared_ptr<CancellationSource> cancellation;
        std::vector<Navigation::NavigationPreparedTile> prepared;
        std::shared_ptr<NavigationBakePublication> candidate;
        std::vector<std::uint8_t> envelope;
        std::shared_ptr<NavigationBakeDiagnostics> diagnostics;
    };

    /** @brief Creates complete concrete gather/build/validate/publish work for the process scheduler. */
    Navigation::NavigationBakeJobDescriptor Descriptor(const std::shared_ptr<ServiceState> &state, const std::shared_ptr<Attempt> &attempt);
    /** @brief Cancels a queued operation that was replaced before scheduler admission. */
    void CancelPending(OperationStore &operations, const std::shared_ptr<Attempt> &attempt) noexcept;
    /** @brief Projects scheduler progress and terminal truth into retained diagnostic checkpoints. */
    void ObserveBake(const std::shared_ptr<NavigationBakeDiagnostics> &diagnostics,
                     const Navigation::NavigationBakeJobSnapshot &snapshot) noexcept;
    /** @brief Reports stable contributing source identities for a failed tile. */
    void ReportTileFailure(const ServiceState &state, const Attempt &attempt, const Navigation::NavigationPreparedTile &tile,
                           const Error &error) noexcept;
}  // namespace Horo::Application::NavigationBakeDetail
