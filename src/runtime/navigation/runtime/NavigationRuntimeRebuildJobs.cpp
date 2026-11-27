#include "Horo/Navigation/NavigationRuntimeRebuildJobs.h"

#include <atomic>
#include <new>
#include <utility>

namespace Horo::Navigation {
    namespace {
        /** @brief Transfers one worker-written candidate to the owner after its job terminalizes. */
        struct CompletedRebuild final {
            NavigationRuntimeRebuildReservation reservation;
            std::optional<NavigationRuntimeTileRebuildCandidate> candidate;
            std::atomic<bool> ready{}; /**< Set after the worker writes; read before the owner moves the candidate. */
        };

        /** @brief Validates bounded storage shape before scheduler admission. */
        [[nodiscard]] bool HasOwnedLayout(const NavigationRuntimeOwnedRebuildRequest &request) {
            return request.input && !request.layout.empty() && request.layout.size() == request.activeTiles.size() &&
                   request.layout.size() <= NavMeshArtifactLimits::MaximumTiles;
        }

        /** @brief Projects only owned callback captures into the synchronous preparation contract. */
        [[nodiscard]] NavigationRuntimeTileRebuildRequest BorrowRequest(const NavigationRuntimeOwnedRebuildRequest &request) {
            return {.expected = request.expected,
                    .nextTopology = request.nextTopology,
                    .input = *request.input,
                    .layout = request.layout,
                    .activeTiles = request.activeTiles,
                    .compatibility = request.compatibility,
                    .tileLimits = request.tileLimits,
                    .maximumRebuiltTiles = request.maximumRebuiltTiles,
                    .maximumOwnedBytes = request.maximumOwnedBytes - request.retainedInputBytes,
                    .linkProjector = request.linkProjector.get()};
        }

        /** @brief Runs exactly one rebuild and transfers its candidate only after successful preparation. */
        [[nodiscard]] Result<void> RunRebuild(const NavigationRuntimeOwnedRebuildRequest &request, const INavigationMeshBuilder &builder,
                                              const INavigationRuntimeRebuildBackendFactory &factory, CompletedRebuild &transfer,
                                              const CancellationToken &cancel) {
            auto prepared = PrepareRuntimeNavigationTileRebuild(BorrowRequest(request), builder, factory, cancel);
            if (cancel.IsCancellationRequested())
                return JobCancelled();
            if (prepared.HasError())
                return Result<void>::Failure(std::move(prepared).ErrorValue());
            transfer.candidate.emplace(std::move(prepared).Value());
            transfer.ready.store(true);
            return Result<void>::Success();
        }
    }  // namespace

    struct NavigationRuntimeRebuildJobs::Pending final {
        explicit Pending(const CancellationToken &parent) : cancel(parent) {}

        CancellationSource cancel;
        std::shared_ptr<CompletedRebuild> transfer{std::make_shared<CompletedRebuild>()};
        JobHandle job;
    };

    /** @copydoc NavigationRuntimeRebuildJobs::NavigationRuntimeRebuildJobs */
    NavigationRuntimeRebuildJobs::NavigationRuntimeRebuildJobs(
        const JobSystem &jobs, std::shared_ptr<const INavigationMeshBuilder> builder,
        std::shared_ptr<const INavigationRuntimeRebuildBackendFactory> factory) noexcept
        : jobs_(jobs), builder_(std::move(builder)), factory_(std::move(factory)) {}

    /** @copydoc NavigationRuntimeRebuildJobs::~NavigationRuntimeRebuildJobs */
    NavigationRuntimeRebuildJobs::~NavigationRuntimeRebuildJobs() {
        Cancel();
    }

    /** @copydoc NavigationRuntimeRebuildJobs::Start */
    Result<void> NavigationRuntimeRebuildJobs::Start(NavigationRuntimeOwnedRebuildRequest request,
                                                     const CancellationToken &parentCancellation,
                                                     NavigationRuntimeRebuildReservation reservation) {
        if (pending_)
            return Result<void>::Failure(MakeError(NavigationErrors::AdmissionRejected));
        if (parentCancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(NavigationErrors::BakeInputCancelled));
        if (!builder_ || !factory_)
            return Result<void>::Failure(MakeError(NavigationErrors::CapabilityUnavailable));
        if (!HasOwnedLayout(request))
            return Result<void>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        if (request.retainedInputBytes >= request.maximumOwnedBytes)
            return Result<void>::Failure(MakeError(NavigationErrors::CapacityExceeded));
        if (!reservation.Matches(request.expected, request.maximumOwnedBytes))
            return Result<void>::Failure(MakeError(NavigationErrors::StaleSnapshot));
        try {
            auto pending = std::make_unique<Pending>(parentCancellation);
            pending->transfer->reservation = std::move(reservation);
            auto submitted = jobs_.SubmitResult({.parentCancellation = pending->cancel.Token(), .priority = JobPriority::Background},
                                                [request = std::move(request), transfer = pending->transfer, builder = builder_,
                                                 factory = factory_](const CancellationToken &cancel) mutable {
                const auto ownedInput = std::move(request);
                return RunRebuild(ownedInput, *builder, *factory, *transfer, cancel);
            });
            if (submitted.HasError())
                return Result<void>::Failure(std::move(submitted).ErrorValue());
            pending->job = std::move(submitted).Value();
            pending_ = std::move(pending);
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
    }

    /** @copydoc NavigationRuntimeRebuildJobs::TakeCompleted */
    Result<std::optional<NavigationRuntimeTileRebuildCandidate>> NavigationRuntimeRebuildJobs::TakeCompleted() {
        using Return = Result<std::optional<NavigationRuntimeTileRebuildCandidate>>;
        if (!pending_)
            return Return::Success(std::nullopt);
        auto snapshot = pending_->job.Snapshot();
        if (!snapshot || !snapshot->terminalResult)
            return Return::Success(std::nullopt);
        auto pending = std::move(pending_);
        auto &terminal = *snapshot->terminalResult;
        if (pending->cancel.Token().IsCancellationRequested() || terminal.state == JobState::Cancelled)
            return Return::Failure(MakeError(NavigationErrors::BakeInputCancelled));
        if (terminal.error)
            return Return::Failure(std::move(*terminal.error));
        if (!pending->transfer->ready.load())
            return Return::Failure(MakeError(NavigationErrors::OutcomeDescriptorInvalid));
        pending->transfer->candidate->reservation_ = std::move(pending->transfer->reservation);
        pending->transfer->candidate->preparedByJob_ = true;
        return Return::Success(std::move(pending->transfer->candidate));
    }

    /** @copydoc NavigationRuntimeRebuildJobs::Cancel */
    void NavigationRuntimeRebuildJobs::Cancel() const noexcept {
        if (pending_)
            pending_->cancel.RequestCancellation();
    }

    /** @copydoc NavigationRuntimeRebuildJobs::Busy */
    bool NavigationRuntimeRebuildJobs::Busy() const noexcept {
        return pending_ != nullptr;
    }
}  // namespace Horo::Navigation
