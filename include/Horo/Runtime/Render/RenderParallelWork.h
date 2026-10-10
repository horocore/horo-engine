#pragma once

/**
 * @file RenderParallelWork.h
 * @brief Bounded immutable CPU frame handoff and nonblocking owner publication.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"

#include <cstddef>
#include <cstdint>

namespace Horo {
    class JobSystem;
}

namespace Horo::Render {
    struct FrameToken;

    /** @brief Explicit native-worker admission; zero passes means unsupported, never owner fallback. */
    struct RenderParallelRecordingCapabilities final {
        std::size_t maximumPasses{0};
        std::size_t maximumLiveRecordings{0}; /**< Includes cancelled callbacks and native submissions not yet retired. */
    };

    /**
     * @brief Backend-owned immutable native payload and disjoint per-pass recording slots.
     * @details Only Record may run on host workers, once per index. Every native reference is
     * owned by this lease, not borrowed from a backend, registry, compiled graph or caller.
     * Cancellation closes new recording; it neither submits nor releases frontend pins on a worker.
     * Owner acceptance revalidates the active frame and commits canonical order. The backend
     * retains GPU references and frontend resource leases until native completion. A scheduler
     * handle succeeding establishes CPU recording completion only. Destruction must be safe
     * after backend shutdown, including destruction on a worker releasing its final lease.
     */
    class IRenderParallelGraphRecording {
    public:
        virtual ~IRenderParallelGraphRecording() = default;
        /** @brief Returns the immutable captured frame generation. @return Captured frame token. */
        [[nodiscard]] virtual FrameToken Frame() const noexcept = 0;
        /** @brief Returns the bounded canonical slot count. @return Number of independent records. */
        [[nodiscard]] virtual std::size_t PassCount() const noexcept = 0;
        /**
         * @brief Records one prevalidated operation, without submitting or accessing owner state.
         * @param index Canonical pass index; distinct jobs must use distinct slots exactly once.
         * @param cancellation Cooperative host/frame cancellation.
         * @return Original typed native failure, cancellation, or recording success.
         */
        [[nodiscard]] virtual Result<void> Record(std::size_t index, const CancellationToken &cancellation) = 0;
        /** @brief Atomically closes publication and future recording without waiting for workers. */
        virtual void Cancel() noexcept = 0;
    };

    /** @brief Finite per-frame limits checked before copying source bytes or admitting jobs. */
    struct RenderParallelWorkLimits final {
        static constexpr std::size_t HardMaximumPasses = 4'096;
        static constexpr std::size_t HardMaximumBytes = 64U * 1024U * 1024U;
        std::size_t maximumPasses{256};                /**< Maximum command records and accepted jobs in this frame. */
        std::size_t maximumBytes{64U * 1024U * 1024U}; /**< Maximum owned CPU input and output payload, excluding scheduler records. */
    };

    /** @brief Nonblocking progress of frame-bound work; Executed is owner acceptance, not GPU completion. */
    enum class RenderParallelExecutionProgress : std::uint8_t {
        Pending,
        Executed,
    };
}  // namespace Horo::Render
