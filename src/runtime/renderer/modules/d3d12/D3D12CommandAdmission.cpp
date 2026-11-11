#include "D3D12CommandState.h"

#include <algorithm>

namespace Horo::Render::D3D12Detail {
    namespace {
        /** @brief Validates the closed native queue class without accepting negative enum casts. */
        bool KnownQueueKind(D3D12QueueKind kind) noexcept {
            using enum Horo::Render::D3D12QueueKind;
            return kind == Direct || kind == Compute || kind == Copy;
        }

        /** @brief Admits exact effective identity/native queue pairs without inventing role remapping. */
        Result<void> ValidateQueueBindings(std::span<const D3D12CommandQueueBinding> bindings) {
            for (std::size_t index = 0; index < bindings.size(); ++index) {
                const auto &binding = bindings[index];
                if (!binding.queue.IsValid() || !KnownQueueKind(binding.kind))
                    return Result<void>::Failure(
                        D3D12CommandError("render.d3d12.queue_invalid", "Use an exact frontend identity and existing native queue class."));
                for (std::size_t prior = 0; prior < index; ++prior)
                    if (binding.queue == bindings[prior].queue || binding.kind == bindings[prior].kind)
                        return Result<void>::Failure(
                            D3D12CommandError("render.d3d12.queue_alias", "Aliased roles must share one effective queue identity."));
            }
            return Result<void>::Success();
        }

        /** @brief Validates only the host's opaque frame and the implemented graph role. */
        Result<void> ValidateCommandFrame(const RenderGraphExecutionRequest &request) {
            if (!request.frame.IsValid() || request.role != RenderGraphExecutionRole::Scene)
                return Result<void>::Failure(D3D12CommandError("render.d3d12.command_frame",
                                                               "Provide an issued frame and supported Scene graph role; UI "
                                                               "composition belongs to its pipeline adapter."));
            return Result<void>::Success();
        }

        /** @brief Rejects reused ownership receipts before native preparation can retain any resources. */
        Result<void> ValidateCommandLease(const RenderGraphExecutionRequest &request) {
            if (request.transfer && request.transfer->authority != RenderGraphLeaseAuthority::Frontend)
                return Result<void>::Failure(
                    D3D12CommandError("render.d3d12.lease_authority",
                                      "Use a fresh frontend-owned transfer receipt, never another submission's retained receipt."));
            if (!request.resources.empty() && (!request.lease || !request.transfer))
                return Result<void>::Failure(
                    D3D12CommandError("render.d3d12.lease_missing",
                                      "Resident graph work requires its exact frontend resource lease and transfer receipt."));
            return Result<void>::Success();
        }

        /** @brief A pressure eviction cannot invalidate a dependency needed by this incoming complete batch. */
        bool ReferencesRange(RenderQueueId queue, IssuedRange range, std::span<const RenderGraphQueueSubmission> submissions) {
            for (const auto &entry : submissions)
                for (const auto wait : entry.submission.waits)
                    if (wait.queue == queue && wait.value >= range.first && wait.value <= range.last)
                        return true;
            return false;
        }

        /** @brief Projects only earlier same-queue signals while retaining exact sparse issuance boundaries. */
        struct ProjectedSignalHistory final {
            std::size_t ranges{};
            std::uint64_t lastSignal{};
        };

        ProjectedSignalHistory ProjectSignalHistory(const CommandQueueHistory &queue, std::span<const RenderGraphQueueSubmission> earlier) {
            ProjectedSignalHistory projected{queue.issued.size(), queue.lastSignal};
            for (const auto &entry : earlier) {
                if (entry.submission.queue != queue.binding.queue)
                    continue;
                if (projected.ranges == 0 || entry.submission.signal.value != projected.lastSignal + 1)
                    ++projected.ranges;
                projected.lastSignal = entry.submission.signal.value;
            }
            return projected;
        }
    }  // namespace

    bool CommandQueueHistory::Contains(const std::uint64_t value) const noexcept {
        return std::ranges::any_of(issued, [value](const IssuedRange &range) {
            return value >= range.first && value <= range.last;
        });
    }

    Result<void> ValidateCommandConfiguration(const D3D12CommandConfiguration &configuration) {
        if (!configuration.deviceIncarnation || !configuration.limits.IsValid() || configuration.queues.empty() ||
            configuration.queues.size() > 3)
            return Result<void>::Failure(
                D3D12CommandError("render.d3d12.command_configuration", "Provide an exact device and one to three finite queue bindings."));
        if (configuration.barrierMode != D3D12BarrierMode::Legacy && configuration.barrierMode != D3D12BarrierMode::Enhanced)
            return Result<void>::Failure(D3D12CommandError("render.d3d12.barrier_mode", "Select Legacy or required Enhanced explicitly."));
        return ValidateQueueBindings(configuration.queues);
    }

    const CommandQueueHistory *CommandService::FindQueue(const RenderQueueId queue) const noexcept {
        const auto found = std::ranges::find(queues_, queue, [](const CommandQueueHistory &entry) {
            return entry.binding.queue;
        });
        return found == queues_.end() ? nullptr : &*found;
    }

    CommandQueueHistory *CommandService::FindQueue(const RenderQueueId queue) noexcept {
        return const_cast<CommandQueueHistory *>(std::as_const(*this).FindQueue(queue));
    }

    Result<void> CommandService::ValidatePoint(const RenderTimelinePoint point) const {
        const auto *queue = FindQueue(point.queue);
        if (!point.IsValid() || !queue)
            return Result<void>::Failure(D3D12CommandError("render.d3d12.timeline_invalid", "Use an exact admitted queue timeline point."));
        if (point.value <= queue->expiredThrough)
            return Result<void>::Failure(D3D12CommandError("render.d3d12.receipt_expired",
                                                           "Exact issuance evidence retired; retain or reconstruct dependencies "
                                                           "within the configured receipt window."));
        if (!queue->Contains(point.value))
            return Result<void>::Failure(
                D3D12CommandError("render.d3d12.timeline_unissued",
                                  "The exact completion value was not successfully submitted; gaps and future values are not receipts."));
        return Result<void>::Success();
    }

    Result<void> CommandService::ValidateWaits(const RenderQueueSubmission &submission,
                                               const std::span<const RenderGraphQueueSubmission> earlier) const {
        for (const auto point : submission.waits) {
            const bool inBatch = std::ranges::any_of(earlier, [point](const RenderGraphQueueSubmission &entry) {
                return entry.submission.signal == point;
            });
            if (!inBatch) {
                auto valid = ValidatePoint(point);
                if (valid.HasError())
                    return valid;
            }
        }
        return Result<void>::Success();
    }

    Result<void> CommandService::ValidateSignal(const RenderQueueSubmission &submission,
                                                const std::span<const RenderGraphQueueSubmission> earlier) const {
        const auto *queue = FindQueue(submission.queue);
        if (!queue || !submission.IsValid())
            return Result<void>::Failure(
                D3D12CommandError("render.d3d12.timeline_invalid", "Submission names an unavailable queue or malformed signal."));
        auto projected = ProjectSignalHistory(*queue, earlier);
        if (const auto last = projected.lastSignal; submission.signal.value <= last)
            return Result<void>::Failure(D3D12CommandError("render.d3d12.timeline_reused",
                                                           "Queue signal values must strictly increase without reuse or wraparound."));
        return Result<void>::Success();
    }

    Result<void> CommandService::ValidateLimits(const RenderGraphExecutionRequest &request) const {
        const auto &graph = request.graph;
        if (request.resources.size() > limits_.maximumResourceReferences || graph.Transitions().size() > limits_.maximumTransitions)
            return Result<void>::Failure(
                D3D12CommandError("render.d3d12.command_capacity", "Graph resources or transitions exceed the admitted startup capacity."));
        if (const auto barriers = graph.Transitions().size() + graph.ReleaseTransfers().size() + graph.AcquireTransfers().size() +
                                  graph.ExternalReleases().size();
            barriers > limits_.maximumTransitions)
            return Result<void>::Failure(
                D3D12CommandError("render.d3d12.command_capacity",
                                  "Complete transition/release/acquire work exceeds the admitted startup capacity."));
        std::size_t copies{};
        for (const auto &entry : request.workloads) {
            if (std::holds_alternative<RenderGraphBufferCopy>(entry.workload))
                ++copies;
            else if (!std::holds_alternative<std::monostate>(entry.workload))
                return Result<void>::Failure(D3D12CommandError("render.d3d12.workload_unsupported",
                                                               "This command service implements graph buffer copies and ordering; "
                                                               "graphics/shader workloads require their owning pipeline adapter."));
        }
        if (copies > limits_.maximumBufferCopies)
            return Result<void>::Failure(
                D3D12CommandError("render.d3d12.command_capacity", "Graph copies exceed the admitted startup capacity."));
        return Result<void>::Success();
    }

    Result<void> CommandService::ValidateBatch(const std::span<const RenderGraphQueueSubmission> submissions) const {
        auto order = lastOrder_;
        for (std::size_t index = 0; index < submissions.size(); ++index) {
            const auto &submission = submissions[index].submission;
            if (submission.order.value <= order)
                return Result<void>::Failure(
                    D3D12CommandError("render.d3d12.submission_order",
                                      "Frontend submission order must increase across the entire device lifetime."));
            if (auto signal = ValidateSignal(submission, submissions.first(index)); signal.HasError())
                return signal;
            if (auto waits = ValidateWaits(submission, submissions.first(index)); waits.HasError())
                return waits;
            order = submission.order.value;
        }
        return Result<void>::Success();
    }

    Result<void> CommandService::ValidateRequest(const RenderGraphExecutionRequest &request) const {
        if (auto frame = ValidateCommandFrame(request); frame.HasError())
            return frame;
        if (auto lease = ValidateCommandLease(request); lease.HasError())
            return lease;
        if (request.submissions.empty() || request.submissions.size() > RenderGraphSubmissionLimits::MaximumSubmissions)
            return Result<void>::Failure(D3D12CommandError("render.d3d12.submission_required",
                                                           "Provide the complete bounded frontend-issued submission partition; "
                                                           "no implicit timeline is synthesized."));
        return ValidateRenderGraphExecutionRequest(request);
    }

    Result<void> CommandService::ReclaimQueueHistory(CommandQueueHistory &queue,
                                                     const std::span<const RenderGraphQueueSubmission> submissions) {
        auto required = ProjectSignalHistory(queue, submissions).ranges;
        while (required > limits_.maximumTimelineRanges) {
            if (queue.issued.empty() || ReferencesRange(queue.binding.queue, queue.issued.front(), submissions))
                return Result<void>::Failure(D3D12CommandError("render.d3d12.receipt_capacity",
                                                               "Retained dependencies or this batch exceed exact receipt history; "
                                                               "increase startup bounds or retain a smaller active window."));
            const auto range = queue.issued.front();
            auto completed = runtime_->Observe({{queue.binding.queue, range.last}, RenderCpuWaitPurpose::Poll, {}, deviceIncarnation_});
            if (completed.HasError())
                return Result<void>::Failure(completed.ErrorValue());
            if (!completed.Value())
                return Result<void>::Failure(D3D12CommandError("render.d3d12.receipt_capacity",
                                                               "Pending native work prevents exact receipt retirement; poll later "
                                                               "without discarding its ownership."));
            queue.expiredThrough = range.last;
            queue.issued.erase(queue.issued.begin());
            --required;
        }
        return Result<void>::Success();
    }

    Result<void> CommandService::ReclaimHistory(const std::span<const RenderGraphQueueSubmission> submissions) {
        for (auto &queue : queues_) {
            auto reclaimed = ReclaimQueueHistory(queue, submissions);
            if (reclaimed.HasError())
                return reclaimed;
        }
        return Result<void>::Success();
    }

    void CommandService::CommitHistory(const std::span<const RenderGraphQueueSubmission> submissions) noexcept {
        for (const auto &entry : submissions) {
            auto &queue = *FindQueue(entry.submission.queue);
            const auto value = entry.submission.signal.value;
            if (!queue.issued.empty() && queue.issued.back().last + 1 == value)
                queue.issued.back().last = value;
            else
                queue.issued.push_back({value, value});
            queue.lastSignal = value;
            lastOrder_ = entry.submission.order.value;
        }
    }
}  // namespace Horo::Render::D3D12Detail
