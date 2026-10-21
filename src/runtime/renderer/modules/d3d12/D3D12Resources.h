#pragma once

/** @file D3D12Resources.h
 * @brief Backend-private resource service over the canonical Horo allocation contracts.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Runtime/Render/RenderBackend.h"

#include <memory>
#include <vector>

namespace Horo::Render {
    /** @brief Explicit host allowance for metadata, descriptors and asynchronous transfer backing.
     * Resource backing itself requires a canonical RenderMemoryPlacement for every allocation.
     */
    struct D3D12ResourceLimits {
        std::uint32_t maximumResources{4096};
        std::uint32_t maximumDescriptors{2048};
        std::uint32_t maximumTransfers{64};
        std::size_t maximumTransferBytes{32U * 1024U * 1024U};

        /** @brief Validates finite practical bounds before native acquisition. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumResources > 0 && maximumResources <= 65536 && maximumDescriptors > 0 && maximumDescriptors <= 2048 &&
                   maximumTransfers > 0 && maximumTransfers <= 1024 && maximumTransferBytes > 0 &&
                   maximumTransferBytes <= 1024U * 1024U * 1024U;
        }
    };

    /** @brief Backing accounting views; retiring/staging are not additive to committed bytes. */
    struct D3D12ResourceSnapshot {
        std::uint64_t deviceIncarnation{0}; /**< Shared process-wide producer identity of this admitted native context. */
        std::size_t committedBytes{0};
        std::size_t estimatedDescriptorHeapBytes{0}; /**< Separately configured service infrastructure; not resource backing. */
        std::size_t uploadBytes{0};
        std::size_t readbackBytes{0};
        std::size_t retiringBytes{0};
        std::uint32_t resourceCount{0};
        std::uint32_t transferCount{0};
        std::uint32_t descriptorCount{0};
        bool accepting{false};
    };

    /** @brief Fresh DXGI process observations; telemetry never increases the host placement allowance.
     * UMA domains overlap; callers must not sum local and non-local observations as disjoint capacity.
     */
    struct D3D12ResidencyObservation {
        std::uint64_t localUsage{0};
        std::uint64_t localBudget{0};
        std::uint64_t nonLocalUsage{0};
        std::uint64_t nonLocalBudget{0};
    };

    /** @brief Backend-local transfer identity, never an authored or public resource identity. */
    struct D3D12TransferId {
        std::uint64_t deviceIncarnation{0};
        std::uint64_t value{0};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return deviceIncarnation != 0 && value != 0;
        }

        [[nodiscard]] constexpr bool operator==(const D3D12TransferId &) const noexcept = default;
    };

    /** @brief Owner-thread native resource service borrowed from the initialized device session.
     *
     * Calls are serialized on the creating render thread, outside frame recording. The
     * direct queue is shared with the device session. Transfer batches start/end default
     * resources in COMMON; adjacent command recording must preserve this boundary.
     * Creation uploads are ordered ahead of later direct-queue use without CPU waits.
     * Frontend dependency/submission pins remain authoritative for Destroy calls; internal
     * transfer references survive until their real fence completes. StopAdmission cancels
     * publication, not GPU execution. Poll reclaims at most maximumTransfers records;
     * shutdown must poll until drained before destroying the session. There is no GPU idle wait.
     */
    class ID3D12Resources : public IRenderResourceBackend {
    public:
        /** @brief Schedules a buffer range or complete tightly packed texture base-level upload.
         * @param instance Exact backend instance. @param bytes Borrowed bytes copied synchronously.
         * @param offset Buffer byte offset; zero for textures. @param cancellation Checked before submission.
         * @return Owned transfer identity or typed validation/native failure.
         */
        [[nodiscard]] virtual Result<D3D12TransferId> Upload(std::uint64_t instance, std::span<const std::byte> bytes, std::size_t offset,
                                                             const CancellationToken &cancellation = {}) = 0;
        /** @brief Schedules a buffer range or complete texture base-level readback.
         * @param instance Exact live backend instance. @param offset Buffer offset; zero for textures.
         * @param bytes Buffer range size, or exact tightly packed texture base-level size.
         * @param cancellation Checked before native submission.
         * @return Transfer identity; CPU bytes are unavailable until TakeReadback succeeds.
         */
        [[nodiscard]] virtual Result<D3D12TransferId> Readback(std::uint64_t instance, std::size_t offset, std::size_t bytes,
                                                               const CancellationToken &cancellation = {}) = 0;
        /** @brief Returns and consumes completed owned bytes; pending is a typed retry result. */
        [[nodiscard]] virtual Result<std::vector<std::byte>> TakeReadback(D3D12TransferId transfer) = 0;
        /** @brief Cancels CPU publication; retains submitted storage until fence evidence permits release. */
        [[nodiscard]] virtual Result<void> Cancel(D3D12TransferId transfer) = 0;
        /** @brief Nonblocking completion/reclamation; reports native device loss rather than treating it as completion. */
        [[nodiscard]] virtual Result<void> Poll() = 0;
        /** @brief Stops new native allocation/transfer admission and abandons unconsumed CPU results. */
        virtual void StopAdmission() noexcept = 0;
        /** @brief Reports true only after all queued transfers and retired native references have drained. */
        [[nodiscard]] virtual Result<bool> PollDrain() = 0;
        /** @brief Queries fresh budgets for this device's exact adapter; preserves unavailable/native errors. */
        [[nodiscard]] virtual Result<D3D12ResidencyObservation> QueryResidency() const = 0;
        /** @brief Returns separately named charged native backing and descriptor counts on the owner thread. */
        [[nodiscard]] virtual D3D12ResourceSnapshot Snapshot() const noexcept = 0;
    };
}  // namespace Horo::Render
