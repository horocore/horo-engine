#pragma once

/**
 * @file RenderReadback.h
 * @brief Bounded backend-neutral asynchronous readback result lifecycle.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderResource.h"
#include "Horo/Runtime/Render/RenderSubmission.h"
#include "Horo/Runtime/Render/RenderTransferLimits.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <variant>

namespace Horo::Render {
    namespace detail {
        struct PreparedReadbackPayload;
    }

    /** @brief Backend-neutral source generation copied by one readback request. */
    using RenderReadbackSource = std::variant<RenderBufferHandle, RenderTextureHandle>;

    /** @brief Generation-safe identity of one readback request. */
    struct RenderReadbackId {
        RenderResourceOwnerId renderer; /**< Frontend incarnation that owns the request. */
        std::uint64_t value{0};         /**< Non-zero request-local identity. */

        /** @brief Reports whether both identity components are usable. @return True for a live-shaped identity. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return renderer.IsValid() && value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderReadbackId &) const noexcept = default;
    };

    /** @brief Observable lifecycle of one bounded asynchronous readback. */
    enum class RenderReadbackState : std::uint8_t {
        Pending,
        Submitted,
        Ready,
        Failed,
        Cancelled,
        TimedOut,
    };

    /** @brief Immutable request policy retained until completion or retirement. */
    struct RenderReadbackDescriptor {
        RenderReadbackSource source;         /**< Exact source generation; no native handle is exposed. */
        std::size_t sourceByteOffset{0};     /**< First source byte copied by the backend. */
        std::size_t byteCount{0};            /**< Exact mapped result size. */
        std::size_t alignment{1};            /**< Required power-of-two staging alignment. */
        std::chrono::nanoseconds timeout{0}; /**< Positive caller-owned completion deadline. */

        /** @brief Reports whether the source, byte range, alignment, and timeout are usable. @return True for a valid descriptor. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Finite request, staging, retained-result, and mapping bounds. */
    struct RenderReadbackLimits {
        std::size_t maximumPendingBytes{32U * 1024U * 1024U};
        std::size_t maximumRetainedResultBytes{32U * 1024U * 1024U};
        std::size_t maximumRequestBytes{8U * 1024U * 1024U};
        std::size_t maximumAlignment{64U * 1024U};
        std::uint32_t maximumRequests{256};

        /** @brief Reports whether every bound is finite, non-zero, and mutually consistent. @return True for usable limits. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return detail::IsValidBoundedRenderQueueLimits(maximumPendingBytes, maximumRequestBytes, maximumAlignment, maximumRequests) &&
                   maximumRetainedResultBytes > 0 && maximumRequestBytes <= maximumRetainedResultBytes;
        }
    };

    /** @brief Coherent bounded accounting without exposing backend-native staging objects. */
    struct RenderReadbackSnapshot {
        std::size_t pendingBytes{0};        /**< Bytes reserved by work not yet safely retired. */
        std::size_t retainedResultBytes{0}; /**< Ready or consumer-leased bytes still applying backpressure. */
        std::uint32_t requestCount{0};
        std::uint32_t submittedCount{0};
        std::uint32_t readyCount{0};
        std::uint32_t terminalCount{0};
        std::uint64_t failedAdmissionCount{0};
        bool acceptingRequests{false};
    };

    /** @brief Allocation-free expected outcomes for cold-prepared readback operations. */
    enum class RenderReadbackPreparedStatus : std::uint8_t {
        Complete,
        InvalidDescriptor,
        InvalidRequest,
        InvalidTransition,
        CapacityExceeded,
        Stopped,
        WrongThread,
        ResultPending,
        Cancelled,
        TimedOut,
        Failed
    };

    /** @brief One cold, finite writable/result pool within the existing queue. */
    struct RenderReadbackPreparedStorage final {
        std::uint32_t slotCount{};
        std::size_t maximumBytesPerResult{};
        std::size_t alignment{1};
    };

    struct RenderReadbackPreparedRequest final {
        RenderReadbackPreparedStatus status{RenderReadbackPreparedStatus::InvalidRequest};
        RenderReadbackId id;
    };

    /** @brief Synchronous writable borrow, valid only until the next queue lifecycle operation on that request. */
    struct RenderReadbackPreparedMapping final {
        RenderReadbackPreparedStatus status{RenderReadbackPreparedStatus::InvalidRequest};
        std::span<std::byte> bytes;
    };

    /**
     * @brief Move-only lease over immutable mapped bytes.
     *
     * The retained-result charge remains active until the last owning lease storage is
     * destroyed, even if its queue has already stopped admission.
     */
    class RenderReadbackResult final {
    public:
        /** @brief Creates an empty destination for allocation-free prepared acquisition. */
        RenderReadbackResult() noexcept;
        ~RenderReadbackResult();
        RenderReadbackResult(const RenderReadbackResult &) = delete;
        RenderReadbackResult &operator=(const RenderReadbackResult &) = delete;
        RenderReadbackResult(RenderReadbackResult &&) noexcept;
        RenderReadbackResult &operator=(RenderReadbackResult &&) noexcept;

        /** @brief Returns the exact completed request identity. @return Request identity, or invalid after a move. */
        [[nodiscard]] RenderReadbackId Id() const noexcept;
        /** @brief Returns the backend completion point associated with the mapped bytes. @return Exact completion point. */
        [[nodiscard]] RenderTimelinePoint Completion() const noexcept;
        /** @brief Returns immutable bytes valid for this lease lifetime. @return Borrowed immutable mapped bytes. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept;

    private:
        class Impl;
        friend class RenderReadbackQueue;
        explicit RenderReadbackResult(std::unique_ptr<Impl> implementation) noexcept;
        explicit RenderReadbackResult(std::shared_ptr<const detail::PreparedReadbackPayload>) noexcept;
        void ReleasePreparedLease() noexcept;
        std::unique_ptr<Impl> implementation_;
        std::shared_ptr<const detail::PreparedReadbackPayload> prepared_;
    };

    /**
     * @brief Owns bounded readback requests, completion publication, and result leases.
     *
     * Request lifecycle methods and destruction run serially on the frontend's render-capable
     * owner thread. Result leases may outlive the queue and may be destroyed on a consumer
     * thread. Cancelling or timing out submitted work suppresses publication but does not
     * release its pending-byte charge until Complete or Retire proves backend work is done.
     */
    class RenderReadbackQueue final {
    public:
        /**
         * @brief Creates an empty queue with preallocated bounded metadata.
         * @param renderer Exact owning frontend incarnation.
         * @param limits Finite request, staging, result, and alignment bounds.
         * @return Owned queue or a typed configuration/capacity failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<RenderReadbackQueue>> Create(RenderResourceOwnerId renderer,
                                                                                 const RenderReadbackLimits &limits = {});
        ~RenderReadbackQueue();

        RenderReadbackQueue(const RenderReadbackQueue &) = delete;
        RenderReadbackQueue &operator=(const RenderReadbackQueue &) = delete;
        RenderReadbackQueue(RenderReadbackQueue &&) = delete;
        RenderReadbackQueue &operator=(RenderReadbackQueue &&) = delete;

        /** @brief Allocates the fixed writable/result pool before any request; succeeds once on an empty accepting queue.
         * @param storage Finite slot, byte and alignment envelope within the existing queue limits.
         * @param maximumRetainedPayloadBytes Complete known queue/pool CPU payload allowance.
         * @return Actual retained object/vector-capacity payload bytes; excludes allocator/control-block and driver overhead.
         */
        [[nodiscard]] Result<std::size_t> PreparePayloadStorage(RenderReadbackPreparedStorage storage,
                                                                std::size_t maximumRetainedPayloadBytes);
        /** @brief Returns known fixed queue/metadata/prepared-storage CPU payload bytes without allocator/control-block overhead.
         * @details Legacy Complete allocations are governed by limits/Snapshot and are not part of this cold prepared-only query.
         * Hosts mixing legacy completion with prepared storage must reserve those separate dynamic result bytes as well.
         */
        [[nodiscard]] std::size_t RetainedPayloadBytes() const noexcept;
        /** @brief Admits an exact request into the existing identity/lifecycle authority without allocating, including pressure failure. */
        [[nodiscard]] RenderReadbackPreparedRequest RequestPrepared(const RenderReadbackDescriptor &) noexcept;
        /** @brief Borrows exact submitted prepared storage for synchronous native completion copy.
         * @pre The frontend has independently observed actual GPU completion before asking the backend to write these bytes.
         * No pointer may escape the synchronous copy, and terminal requests never expose writable bytes.
         */
        [[nodiscard]] RenderReadbackPreparedMapping WritablePrepared(RenderReadbackId) noexcept;
        /** @brief Publishes prepared bytes only for the exact submitted completion point; performs no copy or allocation.
         * @pre Caller supplies genuine reached completion evidence; queue validates identity/equality, not native fence progress.
         */
        [[nodiscard]] RenderReadbackPreparedStatus CompletePrepared(RenderReadbackId, RenderTimelinePoint) noexcept;
        /** @brief Transfers an immutable preallocated lease without allocation; expected pending/terminal failures are static statuses.
         * @details Failure leaves the output unchanged. Leases may outlive the queue and die on consumer threads.
         */
        [[nodiscard]] RenderReadbackPreparedStatus AcquirePrepared(RenderReadbackId, RenderReadbackResult &) noexcept;
        /** @brief Acknowledges one acquired prepared result only after every consumer lease has retired.
         * @return Complete releases its exact record/slot; ResultPending retains backpressure; unknown/foreign/stale IDs fail closed.
         * @details Prepared acquisition retains a Ready metadata record until this explicit owner acknowledgement.
         * Snapshot request/ready counts include these held records, which are not reacquirable.
         */
        [[nodiscard]] RenderReadbackPreparedStatus RetirePreparedResult(RenderReadbackId) noexcept;
        /** @brief Reclaims only pool slots whose request and every immutable consumer lease have retired; owner thread only.
         * @details Retained-byte accounting may conservatively remain charged until this collection or queue destruction.
         */
        [[nodiscard]] std::size_t CollectPreparedResults() noexcept;

        /**
         * @brief Admits one source generation and exact bounded result size.
         * @param descriptor Backend-neutral source, range, alignment, and timeout policy.
         * @return Request identity or a typed validation/admission failure.
         */
        [[nodiscard]] Result<RenderReadbackId> Request(const RenderReadbackDescriptor &descriptor);
        /**
         * @brief Records the exact GPU completion point after backend submission succeeds.
         * @param request Pending request owned by this queue.
         * @param completion Valid backend completion evidence.
         * @return Success or a typed identity/transition failure.
         */
        [[nodiscard]] Result<void> MarkSubmitted(RenderReadbackId request, RenderTimelinePoint completion);
        /**
         * @brief Copies an exact completed mapping into immutable result-owned storage.
         * @param request Submitted request whose completion point has been reached.
         * @param mappedBytes Exact synchronously borrowed mapping copied before return.
         * @return Success or a typed mapping, capacity, identity, or transition failure.
         */
        [[nodiscard]] Result<void> Complete(RenderReadbackId request, std::span<const std::byte> mappedBytes);
        /**
         * @brief Stores a backend failure and retires the request's pending staging charge.
         * @param request Pending or submitted request.
         * @param error Original typed backend failure preserved for its consumer.
         * @return Success or a typed identity/transition failure.
         */
        [[nodiscard]] Result<void> Fail(RenderReadbackId request, const Error &error);
        /**
         * @brief Requests cancellation; submitted work remains charged until retirement evidence arrives.
         * @param request Pending or submitted request.
         * @return Success or a typed identity/transition failure.
         */
        [[nodiscard]] Result<void> Cancel(RenderReadbackId request);
        /**
         * @brief Records caller-observed timeout without blocking for completion.
         * @param request Pending or submitted request whose descriptor deadline elapsed.
         * @return Success or a typed identity/transition failure.
         */
        [[nodiscard]] Result<void> Timeout(RenderReadbackId request);
        /**
         * @brief Retires cancelled or timed-out submitted work after backend completion.
         * @param request Cancelled or timed-out request with backend retirement evidence.
         * @return Success or a typed identity/transition failure.
         */
        [[nodiscard]] Result<void> Retire(RenderReadbackId request);
        /**
         * @brief Returns the current state of one live or unacknowledged terminal request.
         * @param request Request owned by this queue.
         * @return Current state or a typed invalid-request failure.
         */
        [[nodiscard]] Result<RenderReadbackState> State(RenderReadbackId request) const;
        /**
         * @brief Transfers one ready result lease, or returns its typed pending/terminal failure.
         * @param request Request owned by this queue.
         * @return Move-only result lease or the pending/original terminal failure.
         */
        [[nodiscard]] Result<RenderReadbackResult> Acquire(RenderReadbackId request);
        /**
         * @brief Removes one acknowledged failed, cancelled, or timed-out terminal record.
         * @param request Fully retired terminal request owned by this queue.
         * @return Success or a typed identity/transition failure.
         */
        [[nodiscard]] Result<void> Discard(RenderReadbackId request);
        /** @brief Returns current staging, retained-result, and bounded metadata accounting. @return Immutable value snapshot. */
        [[nodiscard]] RenderReadbackSnapshot Snapshot() const noexcept;
        /** @brief Stops producer admission while allowing outstanding work to retire. */
        void StopAdmission() noexcept;
        /**
         * @brief Clears CPU-side state after all submitted backend work has retired; idempotent.
         * @pre The backend has completed or retired every request that reached Submitted.
         */
        void Shutdown() noexcept;

    private:
        class Impl;
        explicit RenderReadbackQueue(std::unique_ptr<Impl> implementation) noexcept;
        std::unique_ptr<Impl> implementation_;
    };
}  // namespace Horo::Render
