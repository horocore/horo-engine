#pragma once

/** @file RenderSubmissionAllocator.h
 * @brief Frontend-private bounded reservation and legacy-import history. */
#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderSubmission.h"

#include <array>

namespace Horo::Render {
    struct RenderGraphQueueSubmission;
}

namespace Horo::Render::Detail {
    /** @brief One owner-thread authority for an initialized frontend's queue lifetime. */
    class RenderSubmissionAllocator final {
    public:
        static constexpr std::size_t MaximumQueues = 3;
        static constexpr std::size_t MaximumReservations = 128;

        /** @brief Adapts bounded graph records into this same transactional queue history.
         * @param submissions Existing frontend graph records with borrowed wait storage.
         * @return Admission outcome; failure preserves the prior history and reservations. */
        [[nodiscard]] Result<void> AdmitGraphSubmissions(std::span<const RenderGraphQueueSubmission> submissions);
        /** @brief Installs admitted topology before the first frame; aliases share one timeline. */
        [[nodiscard]] Result<void> Configure(std::span<const RenderQueueAssignment> assignments);
        /** @brief Drops abandoned reservations without recycling any issued order or signal. */
        void BeginFrame() noexcept;
        /** @brief Reserves a frame-local view whose wait storage remains stable through this frame. */
        [[nodiscard]] Result<RenderQueueSubmission> Reserve(RenderQueueRole role, std::span<const RenderTimelinePoint> waits);
        /** @brief Copies the exact latest admitted producer on each queue for a dependent GUI tail. */
        [[nodiscard]] std::span<const RenderTimelinePoint> AdmittedPoints(
            std::array<RenderTimelinePoint, MaximumQueues> &storage) const noexcept;
        /** @brief Transactionally imports a complete ordered batch or consumes matching reservations. */
        [[nodiscard]] Result<void> Admit(std::span<const RenderQueueSubmission> submissions);

    private:
        struct Queue {
            RenderQueueId id;
            std::uint64_t issued{};
            std::uint64_t admitted{};
        };

        struct Reservation {
            RenderQueueId queue;
            RenderSubmissionOrder order;
            RenderTimelinePoint signal;
            std::array<RenderTimelinePoint, MaximumQueues> waits{};
            std::size_t waitCount{};
            bool admitted{};
        };

        struct AdmittedRange {
            RenderQueueId queue;
            std::uint64_t first{};
            std::uint64_t last{};
        };

        [[nodiscard]] bool WasAdmitted(RenderTimelinePoint point) const noexcept;
        [[nodiscard]] Result<void> AdmitOne(const RenderQueueSubmission &submission);
        std::array<RenderQueueAssignment, MaximumQueues> assignments_{};
        std::size_t assignmentCount_{};
        std::array<Queue, MaximumQueues> queues_{};
        std::size_t queueCount_{};
        std::array<Reservation, MaximumReservations> reservations_{};
        std::size_t reservationCount_{};
        std::size_t admissionCount_{};
        std::uint64_t issuedOrder_{};
        std::uint64_t admittedOrder_{};
        std::array<AdmittedRange, MaximumReservations> ranges_{};
        std::size_t rangeCount_{};
    };
}  // namespace Horo::Render::Detail
