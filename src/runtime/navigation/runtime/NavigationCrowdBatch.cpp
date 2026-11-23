#include "Horo/Navigation/NavigationCrowdBatch.h"

#include "Horo/Navigation/NavigationErrors.h"
#include "NavigationCrowdOutcomeValidation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <unordered_set>

namespace Horo::Navigation {
    namespace {
        using Batch = Result<std::unique_ptr<NavigationCrowdBatch>>;
        using Collection = Result<std::optional<std::vector<NavigationCrowdBatchResult>>>;
        using QualifiedResults = Result<std::vector<NavigationCrowdBatchResult>>;

        /** @brief A qualification must identify an exact, tested runtime tuple. */
        bool ValidQualification(const NavigationCrowdQualification &evidence) noexcept {
            const std::array digests{evidence.provider, evidence.buildPlatform, evidence.goldenEvidence, evidence.topologyContent,
                                     evidence.profile};
            return std::ranges::none_of(digests,
                                        [](const Sha256Digest &digest) {
                return digest == Sha256Digest{};
            }) && std::isfinite(evidence.fixedStepSeconds) &&
                   evidence.fixedStepSeconds > 0.0F && evidence.maximumAgents != 0;
        }

        /** @brief Missing constraints or a mismatched fixed delta invalidate the whole deterministic tick. */
        bool CompleteQualifiedTick(const NavigationCrowdSnapshot &capture, const std::span<const NavigationAvoidanceRequest> requests,
                                   const NavigationCrowdQualification &admitted) noexcept {
            return requests.size() == capture.Agents().size() && requests.size() <= admitted.maximumAgents &&
                   std::ranges::all_of(capture.ProfileTruncation(), [](const auto &entry) {
                return entry.affectedAgents == 0;
            }) && std::ranges::all_of(requests, [&admitted](const auto &request) {
                return request.stepSeconds == admitted.fixedStepSeconds;
            });
        }

        /** @brief Indexes distinct requests for the duration of synchronous planning. */
        Result<void> IndexRequests(const std::span<const NavigationAvoidanceRequest> requests,
                                   const std::span<const NavigationAvoidanceRequest *> indexed) {
            for (const auto &request : requests) {
                if (request.agentIndex >= indexed.size() || indexed[request.agentIndex])
                    return Result<void>::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
                if (!Detail::ValidCrowdEnvelope(request, requests.front().stepSeconds) || !Math::IsFinite(request.preferredVelocity) ||
                    request.preferredVelocity.y != 0)
                    return Result<void>::Failure(MakeError(NavigationErrors::OutcomeDescriptorInvalid));
                indexed[request.agentIndex] = &request;
            }
            return Result<void>::Success();
        }

        /** @brief Copies indexed requests into cell order. Empty cells do not consume job capacity. */
        Result<std::vector<std::size_t>> PackRequests(const NavigationCrowdSnapshot &capture,
                                                      const std::span<const NavigationAvoidanceRequest> requests,
                                                      std::vector<NavigationAvoidanceRequest> &ordered) {
            using Packed = Result<std::vector<std::size_t>>;
            std::vector<const NavigationAvoidanceRequest *> indexed(capture.Agents().size());
            if (auto valid = IndexRequests(requests, indexed); valid.HasError())
                return Packed::Failure(std::move(valid).ErrorValue());
            ordered.reserve(requests.size());
            std::vector<std::size_t> cellEnds;
            cellEnds.reserve(std::min(requests.size(), capture.Cells().size()));
            const auto indices = capture.CellAgentIndices();
            for (const auto &cell : capture.Cells()) {
                const auto first = ordered.size();
                for (const auto index : indices.subspan(cell.firstAgent, cell.agentCount)) {
                    if (const auto *request = indexed[index])
                        ordered.push_back(*request);
                }
                if (ordered.size() != first)
                    cellEnds.push_back(ordered.size());
            }
            return Packed::Success(std::move(cellEnds));
        }

        /** @brief Prevents two jobs from sharing one provider's mutable scratch state. */
        bool IndependentProviders(const std::span<const std::shared_ptr<INavigationCrowdBackend>> providers) {
            std::unordered_set<const INavigationCrowdBackend *> used;
            used.reserve(providers.size());
            return std::ranges::all_of(providers, [&used](const auto &provider) {
                return provider && used.insert(provider.get()).second;
            });
        }

        /** @brief Rejects foreign capture evidence or malformed output before the batch is collectible. */
        bool ValidResult(const NavigationCrowdSnapshot &capture, const NavigationAvoidanceRequest &request,
                         const NavigationAvoidanceOutcome &outcome) noexcept {
            return outcome.binding == capture.Binding() && outcome.captureTick == capture.CaptureTick() &&
                   outcome.dynamicRevision == capture.DynamicRevision() &&
                   Detail::ValidCrowdOutcome(outcome, request, capture.Agents()[request.agentIndex].velocity);
        }

        /** @brief Runs a complete validated set in capture order. Results remain private until the entire tick succeeds. */
        QualifiedResults SolveQualifiedTick(const NavigationCrowdSnapshot &capture,
                                            const std::span<const NavigationAvoidanceRequest *const> indexed,
                                            INavigationQualifiedCrowdBackend &provider, const CancellationToken &cancellation) {
            std::vector<NavigationCrowdBatchResult> results;
            results.reserve(indexed.size());
            for (const auto agentIndex : capture.CellAgentIndices()) {
                if (cancellation.IsCancellationRequested())
                    return QualifiedResults::Failure(MakeError(NavigationErrors::QueryCancelled));
                const auto &request = *indexed[agentIndex];
                auto outcome = provider.Solve(capture, request);
                if (outcome.HasError())
                    return QualifiedResults::Failure(std::move(outcome).ErrorValue());
                if (!ValidResult(capture, request, outcome.Value()))
                    return QualifiedResults::Failure(MakeError(NavigationErrors::OutcomeDescriptorInvalid));
                results.emplace_back(agentIndex, std::move(outcome).Value());
            }
            if (cancellation.IsCancellationRequested())
                return QualifiedResults::Failure(MakeError(NavigationErrors::QueryCancelled));
            return QualifiedResults::Success(std::move(results));
        }
    }  // namespace

    struct NavigationCrowdBatch::State final {
        State(const NavigationCrowdSnapshot &snapshot, const CancellationToken &parent) : capture(snapshot), cancel(parent) {}

        /** @brief Each worker writes only its disjoint range. The owner reads it after the job's terminal publication. */
        Result<void> Solve(const std::size_t first, const std::size_t end, INavigationCrowdBackend &provider,
                           const CancellationToken &cancellation) {
            for (auto index = first; index < end; ++index) {
                if (cancellation.IsCancellationRequested())
                    return JobCancelled();
                const auto &request = work[index];
                auto outcome = provider.Solve(capture, request);
                if (outcome.HasError())
                    return Result<void>::Failure(std::move(outcome).ErrorValue());
                if (!ValidResult(capture, request, outcome.Value()))
                    return Result<void>::Failure(MakeError(NavigationErrors::OutcomeDescriptorInvalid));
                results[index] = {request.agentIndex, std::move(outcome).Value()};
            }
            return cancellation.IsCancellationRequested() ? JobCancelled() : Result<void>::Success();
        }

        const NavigationCrowdSnapshot capture;
        CancellationSource cancel;
        std::vector<NavigationAvoidanceRequest> work;
        std::vector<NavigationCrowdBatchResult> results;
    };

    /** @copydoc NavigationCrowdBatch::NavigationCrowdBatch */
    NavigationCrowdBatch::NavigationCrowdBatch(ConstructionKey, std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc NavigationCrowdBatch::~NavigationCrowdBatch */
    NavigationCrowdBatch::~NavigationCrowdBatch() {
        Cancel();
    }

    /** @copydoc NavigationCrowdBatch::RunQualified */
    QualifiedResults NavigationCrowdBatch::RunQualified(const NavigationCrowdSnapshot &capture,
                                                        const std::span<const NavigationAvoidanceRequest> requests,
                                                        INavigationQualifiedCrowdBackend &provider,
                                                        const NavigationCrowdQualification &admitted,
                                                        const NavigationGeneration activeTopology, const CancellationToken &cancellation) {
        if (!capture.IsValid() || capture.Mode() != AvoidanceExecutionMode::DeterministicQualified)
            return QualifiedResults::Failure(MakeError(NavigationErrors::OperationUnsupported));
        if (!ValidQualification(admitted) || provider.Qualification() != admitted)
            return QualifiedResults::Failure(MakeError(NavigationErrors::CapabilityDescriptorInvalid));
        if (!activeTopology.IsValid() ||
            capture.DeterministicStamp() != NavigationCrowdDeterministicStamp{activeTopology, admitted.topologyContent, admitted.profile})
            return QualifiedResults::Failure(MakeError(NavigationErrors::StaleSnapshot));
        if (!CompleteQualifiedTick(capture, requests, admitted))
            return QualifiedResults::Failure(MakeError(NavigationErrors::AdmissionRejected));
        if (cancellation.IsCancellationRequested())
            return QualifiedResults::Failure(MakeError(NavigationErrors::QueryCancelled));
        try {
            std::vector<const NavigationAvoidanceRequest *> indexed(capture.Agents().size());
            if (const auto valid = IndexRequests(requests, indexed); valid.HasError())
                return QualifiedResults::Failure(valid.ErrorValue());
            return SolveQualifiedTick(capture, indexed, provider, cancellation);
        } catch (const std::bad_alloc &) {
            return QualifiedResults::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
    }

    /** @copydoc NavigationCrowdBatch::Submit */
    Batch NavigationCrowdBatch::Submit(const JobSystem &jobs, const NavigationCrowdSnapshot &capture,
                                       const std::span<const NavigationAvoidanceRequest> requests,
                                       const std::span<const std::shared_ptr<INavigationCrowdBackend>> providers,
                                       const CancellationToken &cancellation) {
        if (!capture.IsValid() || requests.empty() || requests.size() > capture.Agents().size() || providers.empty())
            return Batch::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
        if (capture.Mode() != AvoidanceExecutionMode::BestEffortBounded)
            return Batch::Failure(MakeError(NavigationErrors::OperationUnsupported));
        if (cancellation.IsCancellationRequested())
            return Batch::Failure(MakeError(NavigationErrors::QueryCancelled));
        try {
            auto state = std::make_shared<State>(capture, cancellation);
            auto ends = PackRequests(capture, requests, state->work);
            if (ends.HasError())
                return Batch::Failure(std::move(ends).ErrorValue());
            state->results.resize(requests.size());
            auto batch = std::make_unique<NavigationCrowdBatch>(ConstructionKey{}, std::move(state));
            if (auto submitted = batch->SubmitGroups(jobs, ends.Value(), providers); submitted.HasError())
                return Batch::Failure(std::move(submitted).ErrorValue());
            return Batch::Success(std::move(batch));
        } catch (const std::bad_alloc &) {
            return Batch::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
    }

    /** @copydoc NavigationCrowdBatch::SubmitGroups */
    Result<void> NavigationCrowdBatch::SubmitGroups(const JobSystem &jobs, const std::span<const std::size_t> cellEnds,
                                                    const std::span<const std::shared_ptr<INavigationCrowdBackend>> providers) {
        const auto count = std::min(cellEnds.size(), providers.size());
        if (!IndependentProviders(providers.first(count)))
            return Result<void>::Failure(MakeError(NavigationErrors::CapabilityDescriptorInvalid));
        jobs_.reserve(count);
        std::size_t cell = 0;
        std::size_t first = 0;
        // Like path dispatch, publication-lane admission cannot wait on a host-configured blocking queue.
        JobProducerScope producer{JobProducerRole::MainEditor};
        for (std::size_t group = 0; group < count; ++group) {
            cell += cellEnds.size() / count + static_cast<std::size_t>(group < cellEnds.size() % count);
            const auto end = cellEnds[cell - 1];
            auto accepted = jobs.SubmitResult({.parentCancellation = state_->cancel.Token()},
                                              [state = state_, provider = providers[group], first, end](const CancellationToken &token) {
                return state->Solve(first, end, *provider, token);
            });
            if (accepted.HasError())
                return Result<void>::Failure(std::move(accepted).ErrorValue());
            jobs_.push_back(std::move(accepted).Value());
            first = end;
        }
        return Result<void>::Success();
    }

    /** @copydoc NavigationCrowdBatch::ObserveJobs */
    Result<bool> NavigationCrowdBatch::ObserveJobs() const {
        bool ready = true;
        for (const auto &job : jobs_) {
            const auto observed = job.Snapshot();
            if (!observed || !observed->terminalResult) {
                ready = false;
                continue;
            }
            if (const auto &terminal = *observed->terminalResult; terminal.state != JobState::Succeeded)
                return Result<bool>::Failure(terminal.error ? *terminal.error : MakeError(NavigationErrors::ProviderFailed));
        }
        return Result<bool>::Success(ready);
    }

    /** @copydoc NavigationCrowdBatch::TryTake */
    Collection NavigationCrowdBatch::TryTake(const NavigationCrowdSnapshot &current, const std::uint64_t tick) {
        if (!state_)
            return Collection::Failure(MakeError(NavigationErrors::AdmissionRejected));
        if (state_->cancel.Token().IsCancellationRequested()) {
            Cancel();
            return Collection::Failure(MakeError(NavigationErrors::QueryCancelled));
        }
        // Nonempty batch work pins this exact storage, so a replacement capture cannot reuse its address.
        if (tick != state_->capture.CaptureTick() || current.Agents().data() != state_->capture.Agents().data()) {
            Cancel();
            return Collection::Failure(MakeError(NavigationErrors::StaleSnapshot));
        }
        auto observed = ObserveJobs();
        if (observed.HasError()) {
            Cancel();
            return Collection::Failure(std::move(observed).ErrorValue());
        }
        if (!observed.Value())
            return Collection::Success(std::nullopt);
        if (state_->cancel.Token().IsCancellationRequested()) {
            Cancel();
            return Collection::Failure(MakeError(NavigationErrors::QueryCancelled));
        }
        auto completed = std::move(state_->results);
        state_.reset();
        jobs_.clear();
        return Collection::Success(std::move(completed));
    }

    /** @copydoc NavigationCrowdBatch::Cancel */
    void NavigationCrowdBatch::Cancel() noexcept {
        if (!state_)
            return;
        state_->cancel.RequestCancellation();
        state_.reset();
        jobs_.clear();
    }
}  // namespace Horo::Navigation
