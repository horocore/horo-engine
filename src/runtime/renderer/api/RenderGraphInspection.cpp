#include "Horo/Runtime/Render/RenderGraphInspection.h"

#include "Horo/Runtime/Render/RenderGraphInspectionErrors.h"

#include <algorithm>
#include <array>
#include <functional>
#include <new>

namespace Horo::Render {
    namespace {
        using SnapshotResult = Result<std::shared_ptr<const RenderGraphInspectionSnapshot>>;

        /** @brief Charges each compiler array using checked aggregate count/byte arithmetic before copying. */
        class CaptureBudget {
        public:
            explicit CaptureBudget(const RenderGraphInspectionLimits limits) : limits_(limits) {}

            template <typename T> bool Charge(const std::span<const T> records) {
                return ChargeCount<T>(records.size());
            }

            template <typename T> bool ChargeCount(const std::size_t count) {
                if (count > limits_.maxRecords - records_ || count > (limits_.maxBytes - bytes_) / sizeof(T))
                    return false;
                records_ += count;
                bytes_ += count * sizeof(T);
                return true;
            }

        private:
            RenderGraphInspectionLimits limits_;
            std::size_t records_{};
            std::size_t bytes_{sizeof(RenderGraphInspectionSnapshot)};
        };

        /** @brief Checks immutable compiler provenance and exact scheduled execution order. */
        bool SourcesMatch(const RenderGraph &graph, const RenderGraphSchedule &schedule, const RenderGraphLifetimePlan &lifetime,
                          const CompiledRenderGraphExecution &execution) {
            if (const RenderGraphOwnerId owner = graph.Owner();
                !owner.IsValid() || schedule.Owner() != owner || lifetime.Owner() != owner || execution.Owner() != owner)
                return false;
            return std::ranges::equal(schedule.OrderedPasses(), execution.Passes(), {}, std::identity{}, &RenderGraphExecutionPass::pass);
        }

        /** @brief Validates finite payload allowances before checked budget subtraction. */
        bool LimitsValid(const RenderGraphInspectionLimits limits) {
            return limits.maxRecords > 0 && limits.maxRecords <= RenderGraphInspectionLimits::HardMaxRecords &&
                   limits.maxBytes >= sizeof(RenderGraphInspectionSnapshot) && limits.maxBytes <= RenderGraphInspectionLimits::HardMaxBytes;
        }

        /** @brief Admits immutable compiler provenance and the explicit host context before allocating. */
        Result<void> ValidateCaptureSources(const RenderGraph &graph, const RenderGraphSchedule &schedule,
                                            const RenderGraphLifetimePlan &lifetime, const CompiledRenderGraphExecution &execution,
                                            const RenderGraphInspectionContext context, const RenderGraphInspectionLimits limits,
                                            const std::stop_token cancellation) {
            if (!LimitsValid(limits))
                return Result<void>::Failure(MakeError(RenderGraphInspectionErrors::InvalidLimits));
            if (cancellation.stop_requested())
                return Result<void>::Failure(MakeError(RenderGraphInspectionErrors::Cancelled));
            if (!context.renderer.IsValid() || !context.frame.IsValid() || context.revision == 0 ||
                !SourcesMatch(graph, schedule, lifetime, execution))
                return Result<void>::Failure(MakeError(RenderGraphInspectionErrors::InvalidSource));
            return Result<void>::Success();
        }

        /** @brief Charges the complete closed dataset; every result must succeed before capture. */
        bool ChargeSources(const RenderGraph &graph, const RenderGraphSchedule &schedule, const RenderGraphLifetimePlan &lifetime,
                           const CompiledRenderGraphExecution &execution, const RenderGraphInspectionLimits limits) {
            CaptureBudget budget{limits};
            const std::array charged{budget.Charge(graph.Passes()),
                                     budget.Charge(schedule.PassDispositions()),
                                     budget.ChargeCount<RenderGraphInspectionExecutionPass>(execution.Passes().size()),
                                     budget.Charge(graph.Resources()),
                                     budget.Charge(graph.Exports()),
                                     budget.Charge(graph.Usages()),
                                     budget.Charge(graph.Dependencies()),
                                     budget.Charge(lifetime.Lifetimes()),
                                     budget.Charge(lifetime.AliasOpportunities()),
                                     budget.Charge(lifetime.AllocationRequirements()),
                                     budget.Charge(execution.Transitions()),
                                     budget.Charge(execution.AcquireTransfers())};
            return std::ranges::all_of(charged, std::identity{});
        }

        /** @brief Preserves thread-affinity precedence over closed admission for all feed operations. */
        Result<void> CheckFeedAdmission(const std::thread::id thread, const bool &closed) {
            if (std::this_thread::get_id() != thread)
                return Result<void>::Failure(MakeError(RenderGraphInspectionErrors::WrongThread));
            if (closed)
                return Result<void>::Failure(MakeError(RenderGraphInspectionErrors::Closed));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CaptureRenderGraphInspection */
    SnapshotResult CaptureRenderGraphInspection(const RenderGraph &graph, const RenderGraphSchedule &schedule,
                                                const RenderGraphLifetimePlan &lifetime, const CompiledRenderGraphExecution &execution,
                                                const RenderGraphInspectionContext context, const RenderGraphInspectionLimits limits,
                                                const std::stop_token cancellation) {
        if (const auto admitted = ValidateCaptureSources(graph, schedule, lifetime, execution, context, limits, cancellation);
            admitted.HasError())
            return SnapshotResult::Failure(admitted.ErrorValue());
        if (!ChargeSources(graph, schedule, lifetime, execution, limits))
            return SnapshotResult::Failure(MakeError(RenderGraphInspectionErrors::CapacityExceeded));
        try {
            auto snapshot = std::make_shared<RenderGraphInspectionSnapshot>(RenderGraphInspectionSnapshot::MakeConstructionKey());
            snapshot->context_ = context;
            snapshot->owner_ = graph.Owner();
            snapshot->execution_.reserve(execution.Passes().size());
            for (const auto &pass : execution.Passes()) {
                if (cancellation.stop_requested())
                    return SnapshotResult::Failure(MakeError(RenderGraphInspectionErrors::Cancelled));
                snapshot->execution_.emplace_back(pass.pass, pass.kind, pass.queue);
            }
            const auto copy = [&]<typename T>(std::vector<T> &destination, const std::span<const T> source) {
                if (cancellation.stop_requested())
                    return false;
                destination.assign(source.begin(), source.end());
                return true;
            };
            if (const std::array copied{copy(snapshot->passes_, graph.Passes()), copy(snapshot->dispositions_, schedule.PassDispositions()),
                                        copy(snapshot->resources_, graph.Resources()), copy(snapshot->exports_, graph.Exports()),
                                        copy(snapshot->usages_, graph.Usages()), copy(snapshot->dependencies_, graph.Dependencies()),
                                        copy(snapshot->lifetimes_, lifetime.Lifetimes()),
                                        copy(snapshot->aliases_, lifetime.AliasOpportunities()),
                                        copy(snapshot->allocations_, lifetime.AllocationRequirements()),
                                        copy(snapshot->transitions_, execution.Transitions()),
                                        copy(snapshot->transfers_, execution.AcquireTransfers())};
                !std::ranges::all_of(copied, std::identity{}) || cancellation.stop_requested())
                return SnapshotResult::Failure(MakeError(RenderGraphInspectionErrors::Cancelled));
            return SnapshotResult::Success(std::move(snapshot));
        } catch (const std::bad_alloc &) {
            return SnapshotResult::Failure(MakeError(RenderGraphInspectionErrors::AllocationFailed));
        }
    }

    /** @copydoc RenderGraphInspectionFeed::RenderGraphInspectionFeed */
    RenderGraphInspectionFeed::RenderGraphInspectionFeed(const RenderResourceOwnerId renderer) noexcept : renderer_(renderer) {}

    /** @copydoc RenderGraphInspectionFeed::Publish */
    Result<void> RenderGraphInspectionFeed::Publish(std::shared_ptr<const RenderGraphInspectionSnapshot> snapshot) {
        if (const auto admitted = CheckFeedAdmission(thread_, closed_); admitted.HasError())
            return admitted;
        if (!snapshot)
            return Result<void>::Failure(MakeError(RenderGraphInspectionErrors::InvalidSource));
        if (snapshot->Context().renderer != renderer_ || snapshot->Context().revision <= revision_)
            return Result<void>::Failure(MakeError(RenderGraphInspectionErrors::StalePublication));
        revision_ = snapshot->Context().revision;
        snapshot_ = std::move(snapshot);
        return Result<void>::Success();
    }

    /** @copydoc RenderGraphInspectionFeed::Read */
    Result<std::shared_ptr<const RenderGraphInspectionSnapshot>> RenderGraphInspectionFeed::Read() const {
        if (const auto admitted = CheckFeedAdmission(thread_, closed_); admitted.HasError())
            return SnapshotResult::Failure(admitted.ErrorValue());
        return SnapshotResult::Success(snapshot_);
    }

    /** @copydoc RenderGraphInspectionFeed::ReplaceRenderer */
    Result<void> RenderGraphInspectionFeed::ReplaceRenderer(const RenderResourceOwnerId renderer) {
        if (const auto admitted = CheckFeedAdmission(thread_, closed_); admitted.HasError())
            return admitted;
        if (!renderer.IsValid() || renderer == renderer_)
            return Result<void>::Failure(MakeError(RenderGraphInspectionErrors::InvalidSource));
        renderer_ = renderer;
        revision_ = 0;
        snapshot_.reset();
        return Result<void>::Success();
    }

    /** @copydoc RenderGraphInspectionFeed::Shutdown */
    Result<void> RenderGraphInspectionFeed::Shutdown() {
        if (std::this_thread::get_id() != thread_)
            return Result<void>::Failure(MakeError(RenderGraphInspectionErrors::WrongThread));
        closed_ = true;
        snapshot_.reset();
        return Result<void>::Success();
    }
}  // namespace Horo::Render
