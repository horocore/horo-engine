#pragma once

/**
 * @file RenderUpload.h
 * @brief Backend-neutral bounded asynchronous resource upload lifecycle.
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
    /** @brief Backend-neutral destination generation for one upload. */
    using RenderUploadDestination = std::variant<RenderBufferHandle, RenderTextureHandle>;

    /** @brief Generation-safe identity of one upload request. */
    struct RenderUploadId {
        RenderResourceOwnerId renderer; /**< Frontend incarnation that owns the request. */
        std::uint64_t value{0};         /**< Non-zero request-local identity. */

        /** @brief Reports whether both identity components are usable. @return True for a live-shaped identity. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return renderer.IsValid() && value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderUploadId &) const noexcept = default;
    };

    /** @brief Observable lifecycle of one bounded asynchronous upload. */
    enum class RenderUploadState : std::uint8_t {
        Pending,
        Submitted,
        Ready,
        Failed,
        Cancelled,
        TimedOut,
    };

    /** @brief Immutable destination and staging policy retained for one upload. */
    struct RenderUploadDescriptor {
        RenderUploadDestination destination;  /**< Exact destination generation; no native handle is exposed. */
        std::size_t destinationByteOffset{0}; /**< First destination byte written by the backend. */
        std::size_t byteCount{0};             /**< Exact number of bytes staged for the upload. */
        std::size_t alignment{1};             /**< Required power-of-two staging alignment. */
        std::chrono::nanoseconds timeout{0};  /**< Positive caller-owned completion deadline. */

        /** @brief Reports whether the destination, range, alignment, and timeout are usable. @return True for a valid descriptor. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Finite request and staging bounds for one upload queue. */
    struct RenderUploadLimits {
        std::size_t maximumPendingBytes{32U * 1024U * 1024U};
        std::size_t maximumRequestBytes{8U * 1024U * 1024U};
        std::size_t maximumAlignment{64U * 1024U};
        std::uint32_t maximumRequests{256};

        /** @brief Reports whether every bound is finite, non-zero, and mutually consistent. @return True for usable limits. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return detail::IsValidBoundedRenderQueueLimits(maximumPendingBytes, maximumRequestBytes, maximumAlignment, maximumRequests);
        }
    };

    /** @brief Bounded accounting for upload staging and lifecycle records. */
    struct RenderUploadSnapshot {
        std::size_t pendingBytes{0};
        std::uint32_t requestCount{0};
        std::uint32_t submittedCount{0};
        std::uint32_t readyCount{0};
        std::uint32_t terminalCount{0};
        std::uint64_t failedAdmissionCount{0};
        bool acceptingRequests{false};
    };

    /**
     * @brief Owns copied upload payloads and their owner-thread completion lifecycle.
     *
     * Admission copies caller bytes before returning. Lifecycle methods and destruction
     * must run serially on the creating render-capable owner thread. Submitted work keeps
     * its staging charge until Complete or Retire proves that backend work has ended.
     */
    class RenderUploadQueue final {
    public:
        /**
         * @brief Creates an empty bounded upload queue.
         * @param renderer Exact owning frontend incarnation.
         * @param limits Finite staging and metadata bounds.
         * @return Owned queue or a typed configuration/capacity failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<RenderUploadQueue>> Create(RenderResourceOwnerId renderer,
                                                                               const RenderUploadLimits &limits = {});
        ~RenderUploadQueue();

        RenderUploadQueue(const RenderUploadQueue &) = delete;
        RenderUploadQueue &operator=(const RenderUploadQueue &) = delete;
        RenderUploadQueue(RenderUploadQueue &&) = delete;
        RenderUploadQueue &operator=(RenderUploadQueue &&) = delete;

        /**
         * @brief Copies and admits one bounded upload payload.
         * @param descriptor Destination, range, alignment, and timeout policy.
         * @param bytes Source bytes copied before this call returns.
         * @return Request identity or a typed validation/admission failure.
         */
        [[nodiscard]] Result<RenderUploadId> Request(const RenderUploadDescriptor &descriptor, std::span<const std::byte> bytes);
        /** @brief Returns the immutable staged payload for synchronous backend submission. */
        [[nodiscard]] Result<std::span<const std::byte>> Payload(RenderUploadId request) const;
        /** @brief Returns the current request state. */
        [[nodiscard]] Result<RenderUploadState> State(RenderUploadId request) const;
        /** @brief Reports whether backend completion has been published. */
        [[nodiscard]] Result<bool> IsReady(RenderUploadId request) const;
        /** @brief Records the exact completion point after backend submission succeeds. */
        [[nodiscard]] Result<void> MarkSubmitted(RenderUploadId request, RenderTimelinePoint completion);
        /** @brief Publishes successful backend completion and releases staging charge. */
        [[nodiscard]] Result<void> Complete(RenderUploadId request);
        /** @brief Stores an original typed backend failure and releases staging charge. */
        [[nodiscard]] Result<void> Fail(RenderUploadId request, const Error &error);
        /** @brief Requests cooperative cancellation; submitted work remains charged until retirement. */
        [[nodiscard]] Result<void> Cancel(RenderUploadId request);
        /** @brief Records a caller-observed deadline without blocking for completion. */
        [[nodiscard]] Result<void> Timeout(RenderUploadId request);
        /** @brief Retires cancelled or timed-out submitted work after backend completion evidence. */
        [[nodiscard]] Result<void> Retire(RenderUploadId request);
        /** @brief Removes an acknowledged terminal record. */
        [[nodiscard]] Result<void> Discard(RenderUploadId request);
        /** @brief Returns bounded staging and lifecycle accounting. */
        [[nodiscard]] RenderUploadSnapshot Snapshot() const noexcept;
        /** @brief Stops new producer admission while allowing outstanding work to retire. */
        void StopAdmission() noexcept;
        /** @brief Clears CPU-side state after all submitted work has retired; idempotent. */
        void Shutdown() noexcept;

    private:
        class Impl;
        explicit RenderUploadQueue(std::unique_ptr<Impl> implementation) noexcept;
        std::unique_ptr<Impl> implementation_;
    };
}  // namespace Horo::Render
