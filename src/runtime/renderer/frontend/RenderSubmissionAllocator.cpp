#include "RenderSubmissionAllocator.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#include <algorithm>
#include <limits>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Keeps malformed, exhausted and colliding records outside native admission. */
        Error InvalidSubmission() {
            return MakeError(RenderGraphExecutionErrors::UnsupportedWorkload,
                             "Submission reservation/import violates the frontend's bounded queue history.");
        }
    }  // namespace

    /** @copydoc RenderSubmissionAllocator::AdmitGraphSubmissions */
    Result<void> RenderSubmissionAllocator::AdmitGraphSubmissions(const std::span<const RenderGraphQueueSubmission> submissions) {
        if (submissions.empty())
            return Result<void>::Success();
        if (submissions.size() > MaximumReservations)
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
        std::array<RenderQueueSubmission, MaximumReservations> records{};
        for (std::size_t index = 0; index < submissions.size(); ++index)
            records[index] = submissions[index].submission;
        return Admit(std::span{records}.first(submissions.size()));
    }

    /** @copydoc RenderSubmissionAllocator::Configure */
    Result<void> RenderSubmissionAllocator::Configure(const std::span<const RenderQueueAssignment> assignments) {
        if (assignments.empty() || assignments.size() > MaximumQueues || assignmentCount_ != 0)
            return Result<void>::Failure(InvalidSubmission());
        RenderSubmissionAllocator candidate;
        for (const auto &assignment : assignments) {
            if (!assignment.IsValid() ||
                std::ranges::any_of(std::span{candidate.assignments_}.first(candidate.assignmentCount_), [&](const auto &prior) {
                return prior.role == assignment.role;
            }))
                return Result<void>::Failure(InvalidSubmission());
            candidate.assignments_[candidate.assignmentCount_++] = assignment;
            if (std::ranges::find(std::span{candidate.queues_}.first(candidate.queueCount_), assignment.queue, &Queue::id) ==
                std::span{candidate.queues_}.first(candidate.queueCount_).end())
                candidate.queues_[candidate.queueCount_++].id = assignment.queue;
        }
        *this = candidate;
        return Result<void>::Success();
    }

    /** @copydoc RenderSubmissionAllocator::BeginFrame */
    void RenderSubmissionAllocator::BeginFrame() noexcept {
        reservationCount_ = 0;
        admissionCount_ = 0;
    }

    /** @copydoc RenderSubmissionAllocator::Reserve */
    Result<RenderQueueSubmission> RenderSubmissionAllocator::Reserve(const RenderQueueRole role,
                                                                     const std::span<const RenderTimelinePoint> waits) {
        const auto assignments = std::span{assignments_}.first(assignmentCount_);
        const auto assignment = std::ranges::find(assignments, role, &RenderQueueAssignment::role);
        if (assignment == assignments.end() || waits.size() > MaximumQueues || reservationCount_ == MaximumReservations ||
            issuedOrder_ >= std::numeric_limits<std::uint64_t>::max() - 1)
            return Result<RenderQueueSubmission>::Failure(InvalidSubmission());
        auto queues = std::span{queues_}.first(queueCount_);
        auto queue = std::ranges::find(queues, assignment->queue, &Queue::id);
        if (queue->issued >= std::numeric_limits<std::uint64_t>::max() - 1)
            return Result<RenderQueueSubmission>::Failure(InvalidSubmission());
        for (std::size_t index = 0; index < waits.size(); ++index) {
            const auto producer = std::ranges::find(queues, waits[index].queue, &Queue::id);
            const bool reserved = std::ranges::any_of(std::span{reservations_}.first(reservationCount_), [&](const Reservation &prior) {
                return prior.signal == waits[index];
            });
            if (!waits[index].IsValid() || producer == queues.end() || (!WasAdmitted(waits[index]) && !reserved) ||
                std::ranges::any_of(waits.first(index), [&](const auto prior) {
                return prior.queue == waits[index].queue;
            }))
                return Result<RenderQueueSubmission>::Failure(InvalidSubmission());
        }
        auto &reservation = reservations_[reservationCount_++];
        reservation = {.queue = queue->id, .order = {++issuedOrder_}, .signal = {queue->id, ++queue->issued}, .waitCount = waits.size()};
        std::ranges::copy(waits, reservation.waits.begin());
        return Result<RenderQueueSubmission>::Success(
            {reservation.queue, reservation.order, std::span{reservation.waits}.first(reservation.waitCount), reservation.signal});
    }

    /** @copydoc RenderSubmissionAllocator::AdmitOne */
    Result<void> RenderSubmissionAllocator::AdmitOne(const RenderQueueSubmission &submission) {
        auto queues = std::span{queues_}.first(queueCount_);
        auto queue = std::ranges::find(queues, submission.queue, &Queue::id);
        if (!submission.IsValid() || queue == queues.end() || admissionCount_ == MaximumReservations ||
            submission.order.value <= admittedOrder_ || submission.signal.value <= queue->admitted ||
            submission.waits.size() > MaximumQueues)
            return Result<void>::Failure(InvalidSubmission());
        auto reservations = std::span{reservations_}.first(reservationCount_);
        auto reservation = std::ranges::find(reservations, submission.order, &Reservation::order);
        if (reservation != reservations.end()) {
            if (reservation->admitted || reservation->queue != submission.queue || reservation->signal != submission.signal ||
                !std::ranges::equal(std::span{reservation->waits}.first(reservation->waitCount), submission.waits))
                return Result<void>::Failure(InvalidSubmission());
            reservation->admitted = true;
        } else if (submission.order.value <= issuedOrder_ || submission.signal.value <= queue->issued) {
            return Result<void>::Failure(InvalidSubmission());
        }
        for (std::size_t index = 0; index < submission.waits.size(); ++index) {
            const auto wait = submission.waits[index];
            const auto producer = std::ranges::find(queues, wait.queue, &Queue::id);
            if (producer == queues.end() || !WasAdmitted(wait) || std::ranges::any_of(submission.waits.first(index), [&](const auto prior) {
                return prior.queue == wait.queue;
            }))
                return Result<void>::Failure(InvalidSubmission());
        }
        const auto ranges = std::span{ranges_}.first(rangeCount_);
        auto prior = std::ranges::find_if(ranges, [&](const AdmittedRange &range) {
            return range.queue == submission.queue && range.last + 1 == submission.signal.value;
        });
        if (prior != ranges.end()) {
            prior->last = submission.signal.value;
        } else {
            if (rangeCount_ == MaximumReservations)
                return Result<void>::Failure(InvalidSubmission());
            ranges_[rangeCount_++] = {submission.queue, submission.signal.value, submission.signal.value};
        }
        issuedOrder_ = std::max(issuedOrder_, submission.order.value);
        queue->issued = std::max(queue->issued, submission.signal.value);
        admittedOrder_ = submission.order.value;
        queue->admitted = submission.signal.value;
        ++admissionCount_;
        return Result<void>::Success();
    }

    /** @copydoc RenderSubmissionAllocator::WasAdmitted */
    bool RenderSubmissionAllocator::WasAdmitted(const RenderTimelinePoint point) const noexcept {
        return point.IsValid() && std::ranges::any_of(std::span{ranges_}.first(rangeCount_), [&](const AdmittedRange &range) {
            return range.queue == point.queue && point.value >= range.first && point.value <= range.last;
        });
    }

    /** @copydoc RenderSubmissionAllocator::AdmittedPoints */
    std::span<const RenderTimelinePoint> RenderSubmissionAllocator::AdmittedPoints(
        std::array<RenderTimelinePoint, MaximumQueues> &storage) const noexcept {
        std::size_t count{};
        for (const auto &queue : std::span{queues_}.first(queueCount_))
            if (queue.admitted != 0)
                storage[count++] = {queue.id, queue.admitted};
        return std::span{storage}.first(count);
    }

    /** @copydoc RenderSubmissionAllocator::Admit */
    Result<void> RenderSubmissionAllocator::Admit(const std::span<const RenderQueueSubmission> submissions) {
        if (submissions.empty() || submissions.size() > MaximumReservations)
            return Result<void>::Failure(InvalidSubmission());
        auto candidate = *this;
        for (const auto &submission : submissions)
            if (auto admitted = candidate.AdmitOne(submission); admitted.HasError())
                return admitted;
        *this = candidate;
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
