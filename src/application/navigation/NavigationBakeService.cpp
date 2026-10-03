#include "NavigationBakeInternal.h"

#include <algorithm>
#include <limits>

namespace Horo::Application {
    using namespace Horo::Navigation;
    using namespace NavigationBakeDetail;

    namespace {
        /** @brief Validates injected host composition and canonical-root prerequisites. */
        [[nodiscard]] bool ValidHostConfig(const NavigationBakeServiceConfig &config) {
            return config.definition.IsValid() && !config.artifactType.Value().empty() && config.target.IsValid() && config.builder &&
                   config.files && config.sourceAuthority && config.newOperationId && config.writerWaitTimeout.ToNanoseconds() > 0;
        }

        /** @brief Rejects unusable storage paths before admission or staging. */
        [[nodiscard]] bool ValidStorageConfig(const NavigationBakeServiceConfig &config) {
            return !config.cacheRoot.empty() && config.cacheRoot.is_absolute() && !config.targetRoot.empty() &&
                   config.targetRoot.is_absolute();
        }

        /** @brief Enforces both portable artifact ceilings and the caller's cook limit. */
        [[nodiscard]] bool ValidCapacityConfig(const NavigationBakeServiceConfig &config) {
            return config.maximumTiles > 0 && config.maximumTiles <= NavMeshArtifactLimits::MaximumTiles &&
                   config.maximumCandidateBytes > 0 && config.maximumCandidateBytes <= config.cookLimits.maximumArtifactBytes &&
                   config.maximumCandidateBytes <= NavMeshArtifactLimits::MaximumOwnedBytes;
        }

        /** @brief Validates complete sorted tile coverage and immutable capture freshness before admission. */
        [[nodiscard]] Result<void> ValidateRequest(const NavigationBakeRequest &request, const std::size_t maximumTiles) {
            if (!request.input || request.tiles.empty() || request.tiles.size() > maximumTiles ||
                !std::ranges::is_sorted(request.tiles, {}, &NavigationBakeTile::key) ||
                std::ranges::adjacent_find(request.tiles, {}, &NavigationBakeTile::key) != request.tiles.end())
                return Result<void>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
            if (request.cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(NavigationErrors::BakeInputCancelled));
            if (request.diagnosticSources.size() > NavigationSourceGeometryLimits::MaximumContributions ||
                !std::ranges::all_of(request.diagnosticSources, [&request](const auto &source) {
                return std::ranges::count(request.sources, source.observation) == 1 && source.target.relativePath.size() <= 1024;
            }))
                return Result<void>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
            if (!std::ranges::all_of(request.input->Partitions(), [&request](const auto &partition) {
                return std::ranges::any_of(request.tiles, [&partition](const auto &tile) {
                    return tile.key.surface == partition.surface && tile.key.profile == partition.profile;
                });
            }))
                return Result<void>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
            return request.input->ValidatePublication(request.input->Revisions().requestGeneration, request.input->Revisions(),
                                                      request.sources);
        }

        /** @brief Joins only identical uncancelled captures with the same complete layout and source observations. */
        [[nodiscard]] bool IdenticalRequest(const std::shared_ptr<Attempt> &existing, const NavigationBakeRequest &request) {
            if (!existing || existing->cancellation->Token().IsCancellationRequested() ||
                existing->request.input->Fingerprint() != request.input->Fingerprint() ||
                existing->request.compatibility != request.compatibility || existing->request.sources != request.sources ||
                existing->request.diagnosticSources != request.diagnosticSources || existing->request.tiles.size() != request.tiles.size())
                return false;
            return std::ranges::equal(existing->request.tiles, request.tiles, [](const auto &a, const auto &b) {
                return a.key == b.key && a.bounds.minimum == b.bounds.minimum && a.bounds.maximum == b.bounds.maximum &&
                       a.tileSizeMeters == b.tileSizeMeters;
            });
        }

        /** @brief Only an unfinished active operation can own a coalesced request. */
        [[nodiscard]] bool CanJoinActive(const NavigationBakeJobHandle &job, const std::shared_ptr<Attempt> &attempt,
                                         const NavigationBakeRequest &request) {
            const auto snapshot = job.Snapshot();
            return IdenticalRequest(attempt, request) && snapshot && !snapshot->IsTerminal();
        }
    }  // namespace

    /** @copydoc NavigationBakeDetail::CancelPending */
    void NavigationBakeDetail::CancelPending(OperationStore &operations, const std::shared_ptr<Attempt> &attempt) noexcept {
        if (!attempt)
            return;
        attempt->cancellation->RequestCancellation();
        static_cast<void>(operations.Update(attempt->operation, {.state = OperationState::Cancelled,
                                                                 .phase = "superseded",
                                                                 .message = "Navigation bake superseded before execution"}));
        if (attempt->diagnostics)
            attempt->diagnostics->Record({.operation = attempt->operation,
                                          .event = NavigationBakeDiagnosticEvent::Superseded,
                                          .stage = "superseded",
                                          .result = BuildOutputResult::Cancelled,
                                          .message = "Navigation bake superseded before execution"});
    }

    /** @copydoc NavigationBakeService::Create */
    Result<std::unique_ptr<NavigationBakeService>> NavigationBakeService::Create(NavigationBakeServiceConfig config,
                                                                                 OperationStore &operations, JobSystem &jobs) {
        if (jobs.WorkerCount() < 2 || !ValidHostConfig(config) || !ValidStorageConfig(config) || !ValidCapacityConfig(config) ||
            !config.tileLimits.IsValid() || (config.diagnostics && !config.diagnostics->Owns(config.definition)))
            return Result<std::unique_ptr<NavigationBakeService>>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        std::error_code error;
        if (const auto canonical = std::filesystem::weakly_canonical(config.targetRoot, error);
            error || canonical != config.targetRoot.lexically_normal())
            return Result<std::unique_ptr<NavigationBakeService>>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        auto state = std::make_shared<ServiceState>();
        state->config = std::move(config);
        return Result<std::unique_ptr<NavigationBakeService>>::Success(
            std::make_unique<NavigationBakeService>(ConstructionKey{}, std::move(state), operations, jobs));
    }

    /** @copydoc NavigationBakeService::NavigationBakeService */
    NavigationBakeService::NavigationBakeService(ConstructionKey, std::shared_ptr<ServiceState> state, OperationStore &operations,
                                                 JobSystem &jobs)
        : state_(std::move(state)), operations_(operations), jobs_(jobs) {}

    /** @copydoc NavigationBakeService::~NavigationBakeService */
    NavigationBakeService::~NavigationBakeService() {
        Close();
    }

    /** @copydoc NavigationBakeService::Submit */
    Result<OperationId> NavigationBakeService::Submit(NavigationBakeRequest request) {
        if (closed_ || nextGeneration_ >= Adopted)
            return Result<OperationId>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        if (const auto valid = ValidateRequest(request, state_->config.maximumTiles); valid.HasError())
            return Result<OperationId>::Failure(valid.ErrorValue());
        // Admission never waits on a background pointer adoption. Keep this guard until desired-generation mutation.
        auto admittedSource = state_->config.sourceAuthority->TryAcquirePublication(*request.input, request.cancellation);
        if (admittedSource.HasError())
            return Result<OperationId>::Failure(admittedSource.ErrorValue());
        std::optional<NavigationBakeSourceLease> sourceLease{std::move(admittedSource).Value()};
        if (IdenticalRequest(pending_, request))
            return Result<OperationId>::Success(pending_->operation);
        if (CanJoinActive(activeJob_, active_, request))
            return Result<OperationId>::Success(active_->operation);
        auto attempt = std::make_shared<Attempt>();
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
        if (!id.has_value())
            return Result<OperationId>::Failure(MakeError(NavigationErrors::BakeJobAdmissionRejected));
        attempt->generation = nextGeneration_++;
        attempt->operation = *id;
        attempt->diagnostics = state_->config.diagnostics;
        if (attempt->diagnostics)
            attempt->diagnostics->Record({.operation = *id,
                                          .event = NavigationBakeDiagnosticEvent::Queued,
                                          .stage = "queued",
                                          .message = "Navigation bake queued",
                                          .progress = 0.0F});
        state_->desired.store(attempt->generation);
        CancelPending(operations_, pending_);
        if (active_)
            active_->cancellation->RequestCancellation();
        pending_ = std::move(attempt);
        sourceLease.reset();
        Pump();
        return Result<OperationId>::Success(*id);
    }

    /** @copydoc NavigationBakeService::Invalidate */
    void NavigationBakeService::Invalidate() noexcept {
        auto desired = state_->desired.load();
        while ((desired & Adopted) == 0 && !state_->desired.compare_exchange_weak(desired, 0)) {
            // Retry with the observed generation until invalidation succeeds or adoption owns the barrier.
        }
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
            if (const auto snapshot = activeJob_.Snapshot(); !snapshot || !snapshot->IsTerminal())
                return;
            activeJob_ = {};
            active_.reset();
        }
        if (!pending_)
            return;
        active_ = std::move(pending_);
        auto submitted = StartNavigationBakeJob(operations_, jobs_, Descriptor(state_, active_));
        if (submitted.HasError()) {
            if (const bool newlyTerminal =
                    operations_.Update(active_->operation,
                                       {.state = OperationState::Failed, .phase = "admission", .error = submitted.ErrorValue()});
                newlyTerminal && active_->diagnostics)
                active_->diagnostics->Record({.operation = active_->operation,
                                              .event = NavigationBakeDiagnosticEvent::StageFailed,
                                              .stage = "admission",
                                              .result = BuildOutputResult::Failed,
                                              .message = "Navigation bake admission failed",
                                              .causeCode = std::string{submitted.ErrorValue().code.Value()}});
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
        return state_->publication.Load();
    }
}  // namespace Horo::Application
