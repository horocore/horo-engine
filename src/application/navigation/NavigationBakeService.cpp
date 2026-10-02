#include "NavigationBakeInternal.h"

#include <algorithm>
#include <limits>

namespace Horo::Application {
    using namespace NavigationBakeDetail;

    /** @copydoc NavigationBakeDetail::CancelPending */
    void NavigationBakeDetail::CancelPending(OperationStore &operations, const std::shared_ptr<Attempt> &attempt) noexcept {
        if (!attempt)
            return;
        attempt->cancellation->RequestCancellation();
        static_cast<void>(operations.Update(attempt->operation, {.state = OperationState::Cancelled,
                                                                 .phase = "superseded",
                                                                 .message = "Navigation bake superseded before execution"}));
    }

    /** @copydoc NavigationBakeService::Create */
    Result<std::unique_ptr<NavigationBakeService>> NavigationBakeService::Create(NavigationBakeServiceConfig config,
                                                                                 OperationStore &operations, JobSystem &jobs) {
        if (!config.definition.IsValid() || config.artifactType.Value().empty() || !config.target.IsValid() || !config.builder ||
            !config.files || config.cacheRoot.empty() || !config.cacheRoot.is_absolute() || config.targetRoot.empty() ||
            !config.targetRoot.is_absolute() || config.maximumTiles == 0 || config.maximumTiles > NavMeshArtifactLimits::MaximumTiles ||
            config.maximumCandidateBytes == 0 || config.maximumCandidateBytes > config.cookLimits.maximumArtifactBytes ||
            config.maximumCandidateBytes > NavMeshArtifactLimits::MaximumOwnedBytes)
            return Result<std::unique_ptr<NavigationBakeService>>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        std::error_code error;
        config.targetRoot = std::filesystem::weakly_canonical(config.targetRoot, error);
        if (error)
            return Result<std::unique_ptr<NavigationBakeService>>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        auto state = std::make_shared<ServiceState>();
        state->config = std::move(config);
        return Result<std::unique_ptr<NavigationBakeService>>::Success(
            std::unique_ptr<NavigationBakeService>{new NavigationBakeService(std::move(state), operations, jobs)});
    }

    NavigationBakeService::NavigationBakeService(std::shared_ptr<ServiceState> state, OperationStore &operations, JobSystem &jobs)
        : state_(std::move(state)), operations_(operations), jobs_(jobs) {}

    /** @copydoc NavigationBakeService::~NavigationBakeService */
    NavigationBakeService::~NavigationBakeService() {
        Close();
    }

    /** @copydoc NavigationBakeService::Submit */
    Result<OperationId> NavigationBakeService::Submit(NavigationBakeRequest request) {
        if (closed_ || !request.input || request.tiles.empty() || request.tiles.size() > state_->config.maximumTiles ||
            nextGeneration_ >= Adopted || !std::ranges::is_sorted(request.tiles, {}, &NavigationBakeTile::key) ||
            std::ranges::adjacent_find(request.tiles, {}, &NavigationBakeTile::key) != request.tiles.end())
            return Result<OperationId>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        if (request.cancellation.IsCancellationRequested())
            return Result<OperationId>::Failure(MakeError(NavigationErrors::BakeInputCancelled));
        if (!std::ranges::all_of(request.input->Partitions(), [&request](const auto &partition) {
            return std::ranges::any_of(request.tiles, [&partition](const auto &tile) {
                return tile.key.surface == partition.surface && tile.key.profile == partition.profile;
            });
        }))
            return Result<OperationId>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        auto attempt = std::make_shared<Attempt>();
        const auto fresh =
            request.input->ValidatePublication(request.input->Revisions().requestGeneration, request.input->Revisions(), request.sources);
        if (fresh.HasError())
            return Result<OperationId>::Failure(fresh.ErrorValue());
        const auto identical = [&request](const std::shared_ptr<Attempt> &existing) {
            if (!existing || existing->cancellation->Token().IsCancellationRequested() ||
                existing->request.input->Fingerprint() != request.input->Fingerprint() ||
                existing->request.compatibility != request.compatibility || existing->request.sources != request.sources ||
                existing->request.tiles.size() != request.tiles.size())
                return false;
            return std::ranges::equal(existing->request.tiles, request.tiles, [](const auto &a, const auto &b) {
                return a.key == b.key && a.bounds.minimum == b.bounds.minimum && a.bounds.maximum == b.bounds.maximum &&
                       a.tileSizeMeters == b.tileSizeMeters;
            });
        };
        if (identical(pending_))
            return Result<OperationId>::Success(pending_->operation);
        const auto activeSnapshot = activeJob_.Snapshot();
        if (identical(active_) && activeSnapshot && !activeSnapshot->IsTerminal())
            return Result<OperationId>::Success(active_->operation);
        attempt->request = std::move(request);
        attempt->cancellation = std::make_shared<CancellationSource>(attempt->request.cancellation);
        const auto id = operations_.Begin({.kind = OperationKind::Cook,
                                           .title = "Incremental navigation bake",
                                           .phase = "queued",
                                           .progress = 0.0F,
                                           .cancellable = true,
                                           .requestCancel = [cancel = attempt->cancellation] {
            cancel->RequestCancellation();
        }});
        if (!id)
            return Result<OperationId>::Failure(MakeError(NavigationErrors::BakeJobAdmissionRejected));
        attempt->generation = nextGeneration_++;
        attempt->operation = *id;
        state_->desired.store(attempt->generation);
        CancelPending(operations_, pending_);
        if (active_)
            active_->cancellation->RequestCancellation();
        pending_ = std::move(attempt);
        Pump();
        return Result<OperationId>::Success(*id);
    }

    /** @copydoc NavigationBakeService::Invalidate */
    void NavigationBakeService::Invalidate() noexcept {
        state_->desired.store(0);
        CancelPending(operations_, pending_);
        pending_.reset();
        if (active_)
            active_->cancellation->RequestCancellation();
    }

    /** @copydoc NavigationBakeService::Pump */
    void NavigationBakeService::Pump() {
        if (closed_)
            return;
        if (activeJob_.IsValid()) {
            const auto snapshot = activeJob_.Snapshot();
            if (!snapshot || !snapshot->IsTerminal())
                return;
            activeJob_ = {};
            active_.reset();
        }
        if (!pending_)
            return;
        active_ = std::move(pending_);
        auto submitted = StartNavigationBakeJob(operations_, jobs_, Descriptor(state_, active_));
        if (submitted.HasError()) {
            static_cast<void>(operations_.Update(active_->operation,
                                                 {.state = OperationState::Failed, .phase = "admission", .error = submitted.ErrorValue()}));
            active_.reset();
        } else
            activeJob_ = std::move(submitted).Value();
    }

    /** @copydoc NavigationBakeService::Close */
    void NavigationBakeService::Close() noexcept {
        closed_ = true;
        Invalidate();
    }

    /** @copydoc NavigationBakeService::Published */
    std::shared_ptr<const NavigationBakePublication> NavigationBakeService::Published() const noexcept {
        return state_->published.load();
    }
}  // namespace Horo::Application
