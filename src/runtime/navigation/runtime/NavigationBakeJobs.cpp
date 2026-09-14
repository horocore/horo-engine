#include "Horo/Navigation/NavigationBakeJobs.h"

#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <ranges>
#include <string_view>
#include <utility>

namespace Horo::Navigation {
    namespace NavigationBakeJobDetail {
        struct SharedState final {
            mutable std::mutex mutex;
            NavigationBakeJobSnapshot snapshot;
            std::shared_ptr<CancellationSource> cancellation;
            OperationStore *operations{};
        };
    }  // namespace NavigationBakeJobDetail

    namespace {
        constexpr std::array Stages{NavigationBakeJobStage::PartitionGather, NavigationBakeJobStage::TileBuild,
                                    NavigationBakeJobStage::Validation, NavigationBakeJobStage::Publication};

        [[nodiscard]] std::string_view StageName(const NavigationBakeJobStage stage) noexcept {
            switch (stage) {
                case NavigationBakeJobStage::PartitionGather:
                    return "partition_gather";
                case NavigationBakeJobStage::TileBuild:
                    return "tile_build";
                case NavigationBakeJobStage::Validation:
                    return "validation";
                case NavigationBakeJobStage::Publication:
                    return "publication";
                case NavigationBakeJobStage::Count:
                    break;
            }
            return "invalid";
        }

        [[nodiscard]] std::string_view ResourceName(const NavigationBakeBudgetResource resource) noexcept {
            switch (resource) {
                case NavigationBakeBudgetResource::ConcurrentJobs:
                    return "concurrent_jobs";
                case NavigationBakeBudgetResource::ResidentMemory:
                    return "resident_memory";
                case NavigationBakeBudgetResource::TemporaryStorage:
                    return "temporary_storage";
                case NavigationBakeBudgetResource::WorkItems:
                    return "work_items";
                case NavigationBakeBudgetResource::WorkUnits:
                    return "work_units";
                case NavigationBakeBudgetResource::Count:
                    break;
            }
            return "invalid";
        }

        template <typename T> [[nodiscard]] Result<T> BakeFailure(const ErrorCodeDescriptor &descriptor) {
            auto error = MakeError(descriptor);
            return Result<T>::Failure(std::move(error));
        }

        [[nodiscard]] bool AddWouldOverflow(const std::uint64_t left, const std::uint64_t right) noexcept {
            return right > std::numeric_limits<std::uint64_t>::max() - left;
        }

        struct Preflight final {
            std::uint64_t totalWorkUnits{};
            std::optional<NavigationBakeBudgetResource> limitingResource;
        };

        [[nodiscard]] bool HasValidBudget(const NavigationBakeJobBudget &budget) noexcept {
            return budget.maximumConcurrentJobs > 0 && budget.maximumResidentBytes > 0 && budget.maximumTemporaryBytes > 0 &&
                   budget.maximumWorkItems > 0 && budget.maximumWorkUnits > 0 && budget.childDrainTimeout.ToNanoseconds() > 0;
        }

        [[nodiscard]] Result<std::optional<NavigationBakeBudgetResource>> InspectWorkItem(const NavigationBakeWorkItem &item,
                                                                                          const NavigationBakeJobBudget &budget,
                                                                                          std::uint64_t &totalTemporaryBytes,
                                                                                          std::uint64_t &totalWorkUnits) {
            if (static_cast<std::size_t>(item.stage) >= Stages.size() || !item.execute || item.workUnits == 0)
                return BakeFailure<std::optional<NavigationBakeBudgetResource>>(NavigationErrors::BakeJobInvalid);
            if (item.residentBytes > budget.maximumResidentBytes)
                return Result<std::optional<NavigationBakeBudgetResource>>::Success(NavigationBakeBudgetResource::ResidentMemory);
            if (AddWouldOverflow(totalTemporaryBytes, item.temporaryBytes) ||
                (totalTemporaryBytes += item.temporaryBytes) > budget.maximumTemporaryBytes)
                return Result<std::optional<NavigationBakeBudgetResource>>::Success(NavigationBakeBudgetResource::TemporaryStorage);
            if (AddWouldOverflow(totalWorkUnits, item.workUnits) || (totalWorkUnits += item.workUnits) > budget.maximumWorkUnits)
                return Result<std::optional<NavigationBakeBudgetResource>>::Success(NavigationBakeBudgetResource::WorkUnits);
            return Result<std::optional<NavigationBakeBudgetResource>>::Success(std::nullopt);
        }

        [[nodiscard]] Result<Preflight> Validate(const NavigationBakeJobDescriptor &descriptor) {
            const auto &budget = descriptor.budget;
            if (descriptor.title.empty() || descriptor.work.empty() || !HasValidBudget(budget))
                return BakeFailure<Preflight>(NavigationErrors::BakeJobInvalid);

            if (descriptor.work.size() > budget.maximumWorkItems)
                return Result<Preflight>::Success(Preflight{.limitingResource = NavigationBakeBudgetResource::WorkItems});

            std::array<bool, Stages.size()> observed{};
            std::size_t priorStage{};
            std::uint64_t totalWorkUnits{};
            std::uint64_t totalTemporaryBytes{};
            for (const auto &item : descriptor.work) {
                const auto stageIndex = static_cast<std::size_t>(item.stage);
                if (stageIndex < priorStage)
                    return BakeFailure<Preflight>(NavigationErrors::BakeJobInvalid);
                auto inspected = InspectWorkItem(item, budget, totalTemporaryBytes, totalWorkUnits);
                if (inspected.HasError())
                    return Result<Preflight>::Failure(inspected.ErrorValue());
                if (inspected.Value().has_value())
                    return Result<Preflight>::Success(Preflight{.limitingResource = inspected.Value()});
                priorStage = stageIndex;
                observed[stageIndex] = true;
            }
            if (!std::ranges::all_of(observed, [](const bool value) {
                return value;
            }))
                return BakeFailure<Preflight>(NavigationErrors::BakeJobInvalid);
            return Result<Preflight>::Success(Preflight{.totalWorkUnits = totalWorkUnits});
        }

        void PublishProgress(const std::shared_ptr<NavigationBakeJobDetail::SharedState> &state, const NavigationBakeJobStage stage,
                             const std::uint64_t completedUnits) {
            float normalized{};
            {
                std::scoped_lock lock(state->mutex);
                if (state->snapshot.IsTerminal())
                    return;
                state->snapshot.state = NavigationBakeJobState::Running;
                state->snapshot.stage = stage;
                state->snapshot.completedWorkUnits = std::max(state->snapshot.completedWorkUnits, completedUnits);
                ++state->snapshot.revision;
                normalized = static_cast<float>(state->snapshot.Progress());
            }
            static_cast<void>(state->operations->Update(state->snapshot.operation, OperationUpdate{.state = OperationState::Running,
                                                                                                   .phase = std::string{StageName(stage)},
                                                                                                   .message = "Navigation bake in progress",
                                                                                                   .progress = normalized}));
        }

        void CountAccepted(const std::shared_ptr<NavigationBakeJobDetail::SharedState> &state) {
            std::scoped_lock lock(state->mutex);
            ++state->snapshot.acceptedChildJobs;
            ++state->snapshot.revision;
        }

        void CountTerminal(const std::shared_ptr<NavigationBakeJobDetail::SharedState> &state) {
            std::scoped_lock lock(state->mutex);
            ++state->snapshot.terminalChildJobs;
            ++state->snapshot.revision;
        }

        void Finish(const std::shared_ptr<NavigationBakeJobDetail::SharedState> &state, const NavigationBakeJobState terminal,
                    std::optional<Error> error = {}, const std::optional<NavigationBakeBudgetResource> limitingResource = {}) {
            OperationUpdate update;
            {
                std::scoped_lock lock(state->mutex);
                if (state->snapshot.IsTerminal())
                    return;
                state->snapshot.state = terminal;
                state->snapshot.limitingResource = limitingResource;
                state->snapshot.terminalError = error;
                if (terminal == NavigationBakeJobState::Succeeded)
                    state->snapshot.completedWorkUnits = state->snapshot.totalWorkUnits;
                ++state->snapshot.revision;

                update.phase = terminal == NavigationBakeJobState::Succeeded ? "complete" : std::string{StageName(state->snapshot.stage)};
                update.progress = static_cast<float>(state->snapshot.Progress());
                update.error = error;
            }
            switch (terminal) {
                case NavigationBakeJobState::Succeeded:
                    update.state = OperationState::Succeeded;
                    update.message = "Navigation bake completed";
                    break;
                case NavigationBakeJobState::Cancelled:
                    update.state = OperationState::Cancelled;
                    update.message = "Navigation bake cancelled after draining child jobs";
                    break;
                case NavigationBakeJobState::Failed:
                    update.state = OperationState::Failed;
                    update.message = limitingResource.has_value()
                                         ? "Navigation bake exceeded the " + std::string{ResourceName(*limitingResource)} + " budget"
                                         : "Navigation bake failed";
                    break;
                case NavigationBakeJobState::Queued:
                case NavigationBakeJobState::Running:
                    return;
            }
            static_cast<void>(state->operations->Update(state->snapshot.operation, std::move(update)));
        }

        struct BatchResult final {
            std::size_t nextIndex{};
            std::uint64_t workUnits{};
        };

        [[nodiscard]] Result<BatchResult> ExecuteBatch(const std::shared_ptr<NavigationBakeJobDetail::SharedState> &state, JobSystem &jobs,
                                                       NavigationBakeJobDescriptor &descriptor, std::size_t cursor, const std::size_t end) {
            TaskGroup group(jobs, TaskGroupFailurePolicy::FailFast, state->cancellation->Token());
            std::uint64_t residentBytes{};
            std::uint64_t workUnits{};
            std::size_t batchSize{};
            while (cursor != end && batchSize < descriptor.budget.maximumConcurrentJobs &&
                   descriptor.work[cursor].residentBytes <= descriptor.budget.maximumResidentBytes - residentBytes) {
                auto &item = descriptor.work[cursor];
                auto work = std::move(item.execute);
                auto child = group.Spawn({}, [state, work = std::move(work)](const CancellationToken &cancellation) mutable {
                    struct TerminalCounter final {
                        std::shared_ptr<NavigationBakeJobDetail::SharedState> state;
                        ~TerminalCounter() {
                            CountTerminal(state);
                        }
                    } terminal{state};
                    if (cancellation.IsCancellationRequested())
                        return BakeFailure<void>(NavigationErrors::BakeInputCancelled);
                    return work(cancellation);
                });
                if (child.HasError()) {
                    group.RequestCancel();
                    static_cast<void>(
                        group.Join(JoinOptions{.waitPolicy = WaitPolicy::WorkerOnly, .timeout = descriptor.budget.childDrainTimeout}));
                    return BakeFailure<BatchResult>(NavigationErrors::BakeJobAdmissionRejected);
                }
                CountAccepted(state);
                residentBytes += item.residentBytes;
                workUnits += item.workUnits;
                ++cursor;
                ++batchSize;
            }

            const auto joined =
                group.Join(JoinOptions{.waitPolicy = WaitPolicy::WorkerOnly, .timeout = descriptor.budget.childDrainTimeout});
            if (joined.HasError())
                return Result<BatchResult>::Failure(joined.ErrorValue());
            return Result<BatchResult>::Success({.nextIndex = cursor, .workUnits = workUnits});
        }

        [[nodiscard]] Result<void> ExecutePipeline(const std::shared_ptr<NavigationBakeJobDetail::SharedState> &state, JobSystem &jobs,
                                                   NavigationBakeJobDescriptor descriptor) {
            std::uint64_t completedUnits{};
            for (const auto stage : Stages) {
                PublishProgress(state, stage, completedUnits);
                auto cursor = static_cast<std::size_t>(
                    std::ranges::distance(descriptor.work.begin(),
                                          std::ranges::find(descriptor.work, stage, &NavigationBakeWorkItem::stage)));
                const auto last = static_cast<std::size_t>(
                    std::ranges::distance(descriptor.work.begin(),
                                          std::ranges::find_if(descriptor.work.begin() + static_cast<std::ptrdiff_t>(cursor),
                                                               descriptor.work.end(), [stage](const auto &item) {
                    return item.stage != stage;
                })));
                while (cursor != last) {
                    if (state->cancellation->Token().IsCancellationRequested()) {
                        Finish(state, NavigationBakeJobState::Cancelled);
                        return BakeFailure<void>(NavigationErrors::BakeInputCancelled);
                    }
                    auto batch = ExecuteBatch(state, jobs, descriptor, cursor, last);
                    if (batch.HasError()) {
                        if (state->cancellation->Token().IsCancellationRequested()) {
                            Finish(state, NavigationBakeJobState::Cancelled);
                            return BakeFailure<void>(NavigationErrors::BakeInputCancelled);
                        }
                        Finish(state, NavigationBakeJobState::Failed, batch.ErrorValue());
                        return Result<void>::Failure(batch.ErrorValue());
                    }
                    cursor = batch.Value().nextIndex;
                    completedUnits += batch.Value().workUnits;
                    PublishProgress(state, stage, completedUnits);
                }
            }
            Finish(state, NavigationBakeJobState::Succeeded);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc NavigationBakeJobSnapshot::IsTerminal */
    bool NavigationBakeJobSnapshot::IsTerminal() const noexcept {
        return state == NavigationBakeJobState::Succeeded || state == NavigationBakeJobState::Failed ||
               state == NavigationBakeJobState::Cancelled;
    }

    /** @copydoc NavigationBakeJobSnapshot::Progress */
    double NavigationBakeJobSnapshot::Progress() const noexcept {
        return totalWorkUnits == 0 ? 0.0 : std::min(1.0, static_cast<double>(completedWorkUnits) / static_cast<double>(totalWorkUnits));
    }

    NavigationBakeJobHandle::NavigationBakeJobHandle(std::shared_ptr<NavigationBakeJobDetail::SharedState> state) noexcept
        : state_(std::move(state)) {}

    /** @copydoc NavigationBakeJobHandle::IsValid */
    bool NavigationBakeJobHandle::IsValid() const noexcept {
        return static_cast<bool>(state_);
    }

    /** @copydoc NavigationBakeJobHandle::Id */
    OperationId NavigationBakeJobHandle::Id() const noexcept {
        if (!state_)
            return {};
        std::scoped_lock lock(state_->mutex);
        return state_->snapshot.operation;
    }

    /** @copydoc NavigationBakeJobHandle::Snapshot */
    std::optional<NavigationBakeJobSnapshot> NavigationBakeJobHandle::Snapshot() const {
        if (!state_)
            return std::nullopt;
        std::scoped_lock lock(state_->mutex);
        return state_->snapshot;
    }

    /** @copydoc NavigationBakeJobHandle::RequestCancellation */
    bool NavigationBakeJobHandle::RequestCancellation() const noexcept {
        if (!state_)
            return false;
        state_->cancellation->RequestCancellation();
        return true;
    }

    /** @copydoc StartNavigationBakeJob */
    Result<NavigationBakeJobHandle> StartNavigationBakeJob(OperationStore &operations, JobSystem &jobs,
                                                           NavigationBakeJobDescriptor descriptor) {
        auto validation = Validate(descriptor);
        if (validation.HasError())
            return Result<NavigationBakeJobHandle>::Failure(validation.ErrorValue());

        auto cancellation = std::make_shared<CancellationSource>(descriptor.parentCancellation);
        const auto operation = operations.Begin(OperationDescriptor{.kind = OperationKind::Cook,
                                                                    .title = descriptor.title,
                                                                    .phase = "queued",
                                                                    .message = "Navigation bake queued",
                                                                    .progress = 0.0F,
                                                                    .cancellable = true,
                                                                    .requestCancel = [cancellation] {
            cancellation->RequestCancellation();
        }});
        if (!operation.has_value())
            return BakeFailure<NavigationBakeJobHandle>(NavigationErrors::BakeJobAdmissionRejected);

        auto state = std::make_shared<NavigationBakeJobDetail::SharedState>();
        state->snapshot.operation = *operation;
        state->snapshot.totalWorkUnits = std::max<std::uint64_t>(1, validation.Value().totalWorkUnits);
        state->cancellation = std::move(cancellation);
        state->operations = &operations;
        NavigationBakeJobHandle handle{state};

        if (validation.Value().limitingResource.has_value()) {
            const auto resource = *validation.Value().limitingResource;
            auto error = MakeError(NavigationErrors::BakeJobBudgetExceeded,
                                   "Navigation bake exceeds the " + std::string{ResourceName(resource)} + " budget.");
            Finish(state, NavigationBakeJobState::Failed, error, resource);
            return Result<NavigationBakeJobHandle>::Success(std::move(handle));
        }

        // The coordinator record itself must run even when cancellation is already requested; otherwise the scheduler may
        // terminalize it before the callback can drain children and publish the application operation's terminal truth.
        JobDescriptor outerDescriptor{.operationId = *operation};
        auto submitted = jobs.SubmitResult(std::move(outerDescriptor),
                                           [state, &jobs, descriptor = std::move(descriptor)](const CancellationToken &) mutable {
            try {
                return ExecutePipeline(state, jobs, std::move(descriptor));
            } catch (...) {
                auto error = MakeError(NavigationErrors::ProviderFailed, "Navigation bake stage threw an exception.");
                Finish(state, NavigationBakeJobState::Failed, error);
                return Result<void>::Failure(std::move(error));
            }
        });
        if (submitted.HasError()) {
            Finish(state, NavigationBakeJobState::Failed, submitted.ErrorValue());
            return BakeFailure<NavigationBakeJobHandle>(NavigationErrors::BakeJobAdmissionRejected);
        }
        return Result<NavigationBakeJobHandle>::Success(std::move(handle));
    }
}  // namespace Horo::Navigation
