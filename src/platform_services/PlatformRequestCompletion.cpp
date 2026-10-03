#include "PlatformRequestState.h"

namespace Horo::PlatformServices {
    /** @brief Validates and copies one SDK outcome under the request lifecycle lock without invoking user code. */
    Result<PlatformRequestMutation> PlatformRequestStore::State::Enqueue(ProviderEvidence completion) {
        std::lock_guard lock(mutex);
        if (closed)
            return Result<PlatformRequestMutation>::Failure(MakeError(RequestErrors::FrontendUnavailable));
        auto *record = FindRecord(completion.id, completion.generation, completion.type);
        if (record == nullptr)
            return Result<PlatformRequestMutation>::Failure(MakeError(RequestErrors::Stale));
        if (record->evidencePending || IsTerminal(record->snapshot.state))
            return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Unchanged);
        if (!CanComplete(record->snapshot.state, completion.terminalState) ||
            !TerminalShapeIsValid(completion.terminalState, completion.value, completion.error))
            return Result<PlatformRequestMutation>::Failure(MakeError(RequestErrors::InvalidTransition));
        if (evidence.size() >= config.completionCapacity)
            return Result<PlatformRequestMutation>::Failure(MakeError(RequestErrors::CapacityExceeded));
        try {
            completion.context = record->context;
            evidence.push_back(std::move(completion));
        } catch (const std::bad_alloc &) {
            return Result<PlatformRequestMutation>::Failure(MakeError(RequestErrors::CapacityExceeded));
        }
        record->evidencePending = true;
        return Result<PlatformRequestMutation>::Success(PlatformRequestMutation::Applied);
    }

    /** @copydoc PlatformRequestStore::CompletionSink */
    Result<PlatformRequestCompletionSink<void>::Enqueue> PlatformRequestStore::CompletionSinkErased(
        const PlatformRequestId id, const PlatformRequestGeneration generation, const std::type_index type) const {
        using Enqueue = PlatformRequestCompletionSink<void>::Enqueue;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->closed)
                return Result<Enqueue>::Failure(MakeError(RequestErrors::FrontendUnavailable));
            if (state_->FindRecord(id, generation, type) == nullptr)
                return Result<Enqueue>::Failure(MakeError(RequestErrors::Stale));
        }
        const std::weak_ptr<State> weakState = state_;
        return Result<Enqueue>::Success([weakState, id, generation, type](const PlatformRequestState terminalState,
                                                                          std::shared_ptr<const void> value, std::optional<Error> error) {
            const auto state = weakState.lock();
            if (!state)
                return Result<PlatformRequestMutation>::Failure(MakeError(RequestErrors::FrontendUnavailable));
            return state->Enqueue(State::ProviderEvidence{.id = id,
                                                          .generation = generation,
                                                          .type = type,
                                                          .terminalState = terminalState,
                                                          .value = std::move(value),
                                                          .error = std::move(error)});
        });
    }

    /** @copydoc PlatformRequestStore::DrainProviderCompletions */
    Result<PlatformCompletionDrainReport> PlatformRequestStore::DrainProviderCompletions(const std::size_t maxCount) {
        if (std::this_thread::get_id() != state_->ownerThread)
            return Result<PlatformCompletionDrainReport>::Failure(MakeError(RequestErrors::WrongThread));
        if (state_->dispatching.test() || state_->draining.test_and_set())
            return Result<PlatformCompletionDrainReport>::Failure(MakeError(RequestErrors::ReentrantDrain));

        const State::TurnGuard guard{state_->draining};
        PlatformCompletionDrainReport report;
        while (report.processed < maxCount) {
            State::ProviderEvidence completion;
            {
                std::lock_guard lock(state_->mutex);
                if (state_->closed)
                    return Result<PlatformCompletionDrainReport>::Failure(MakeError(RequestErrors::FrontendUnavailable));
                if (state_->evidence.empty())
                    break;
                completion = std::move(state_->evidence.front());
                state_->evidence.pop_front();
            }
            const Log::ScopedLogContext context(completion.context);
            const auto published = CompleteErased(completion.id, completion.generation, completion.type, completion.terminalState,
                                                  std::move(completion.value), std::move(completion.error));
            ++report.processed;
            if (published.HasValue() && published.Value() == PlatformRequestMutation::Applied)
                ++report.applied;
            else
                ++report.discarded;
        }
        // Zero-budget calls still validate the lifecycle instead of hiding a closed store.
        {
            std::lock_guard lock(state_->mutex);
            if (state_->closed)
                return Result<PlatformCompletionDrainReport>::Failure(MakeError(RequestErrors::FrontendUnavailable));
        }
        return Result<PlatformCompletionDrainReport>::Success(report);
    }

}  // namespace Horo::PlatformServices
