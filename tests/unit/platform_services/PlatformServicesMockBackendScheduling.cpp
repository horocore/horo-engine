#include "Horo/PlatformServices/PlatformRequestErrors.h"
#include "PlatformServicesMockBackendImpl.h"

#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

namespace Horo::PlatformServices::TestSupport {
    void MockPlatformServicesBackend::Impl::ScheduleEvent(const PlatformRequestId request, const PlatformRequestGeneration generation,
                                                          const MockPlatformServicesOperation operation, const MockCompletionKind kind,
                                                          const std::uint64_t delayMilliseconds,
                                                          std::function<Result<PlatformRequestMutation>()> complete) {
        events.push({.dueMilliseconds = logicalTimeMilliseconds + delayMilliseconds,
                     .sequence = nextEventSequence++,
                     .request = request,
                     .generation = generation,
                     .operation = operation,
                     .kind = kind,
                     .complete = std::move(complete)});
        if (auto *record = FindRequest(request, generation); record != nullptr)
            ++record->pendingEvents;
    }

    MockPlatformServicesBackend::Impl::RequestRecord *MockPlatformServicesBackend::Impl::FindRequest(
        const PlatformRequestId id, const PlatformRequestGeneration generation) noexcept {
        const auto found = std::ranges::find_if(requestsInFlight, [id, generation](const RequestRecord &record) {
            return record.id == id && record.generation == generation;
        });
        return found == requestsInFlight.end() ? nullptr : std::to_address(found);
    }

    void MockPlatformServicesBackend::Impl::RetireRequestIfIdle(const PlatformRequestId id, const PlatformRequestGeneration generation) {
        const auto found = std::ranges::find_if(requestsInFlight, [id, generation](const RequestRecord &record) {
            return record.id == id && record.generation == generation;
        });
        if (found != requestsInFlight.end() && found->pendingEvents == 0)
            requestsInFlight.erase(found);
    }

    template <typename T>
    std::function<Result<PlatformRequestMutation>()> MockPlatformServicesBackend::Impl::ProviderCompletion(
        const PlatformRequestId id, const PlatformRequestGeneration generation, std::shared_ptr<const ScriptedResponse> response) {
        return [this, id, generation, response = std::move(response)]() {
            if (response->error)
                return requests.CompleteFailure<T>(id, generation, *response->error);
            if constexpr (std::is_void_v<T>)
                return requests.CompleteSuccess(id, generation);
            else
                return requests.CompleteSuccess<T>(id, generation, std::get<T>(response->payload));
        };
    }

    template <typename T>
    std::function<Result<PlatformRequestMutation>()> MockPlatformServicesBackend::Impl::TimeoutCompletion(
        const PlatformRequestId id, const PlatformRequestGeneration generation) {
        return [this, id, generation]() {
            return requests.CompleteTimedOut<T>(id, generation, MakeError(RequestErrors::TimedOut));
        };
    }

    template <typename T>
    std::function<Result<PlatformRequestMutation>()> MockPlatformServicesBackend::Impl::CancellationCompletion(
        const PlatformRequestId id, const PlatformRequestGeneration generation) {
        return [this, id, generation]() {
            return requests.CompleteCancelled<T>(id, generation, MakeError(RequestErrors::Cancelled));
        };
    }

    template <typename T>
    Result<PlatformRequestHandle<T>> MockPlatformServicesBackend::Impl::Submit(const MockPlatformServicesOperation operation,
                                                                               const bool requestIsValid) {
        const auto call = RecordCall(operation);
        if (call.HasError() || !active_ || closed_ || !requestIsValid) {
            if (call.HasValue())
                AddDiagnostic({.kind = MockDiagnosticKind::InvalidRequest,
                               .actual = operation,
                               .expectationIndex = nextExpected,
                               .logicalTimeMilliseconds = logicalTimeMilliseconds});
            return detail::Failure<PlatformRequestHandle<T>>();
        }

        auto responseResult = TakeResponse(operation);
        if (responseResult.HasError())
            return detail::Failure<PlatformRequestHandle<T>>();
        auto response = std::make_shared<const ScriptedResponse>(std::move(responseResult).Value());

        const auto eventCount = 1U + static_cast<unsigned int>(response->timeoutAfterMilliseconds.has_value()) +
                                static_cast<unsigned int>(response->duplicateDelayMilliseconds.has_value());
        if (events.size() + eventCount > MockPlatformServicesBackend::MaximumScheduledEvents ||
            requestsInFlight.size() >= MockPlatformServicesBackend::MaximumExpectedCalls) {
            AddDiagnostic({.kind = MockDiagnosticKind::CapacityExceeded,
                           .actual = operation,
                           .expectationIndex = nextExpected,
                           .logicalTimeMilliseconds = logicalTimeMilliseconds});
            return detail::Failure<PlatformRequestHandle<T>>();
        }

        auto admitted = requests.Admit<T>();
        if (admitted.HasError()) {
            AddDiagnostic({.kind = MockDiagnosticKind::CapacityExceeded,
                           .actual = operation,
                           .expectationIndex = nextExpected,
                           .logicalTimeMilliseconds = logicalTimeMilliseconds});
            return Result<PlatformRequestHandle<T>>::Failure(admitted.ErrorValue());
        }
        auto handle = std::move(admitted).Value();
        const auto running = requests.MarkRunning(handle);
        if (running.HasError()) {
            AddDiagnostic({.kind = MockDiagnosticKind::CompletionRejected,
                           .actual = operation,
                           .expectationIndex = nextExpected,
                           .request = handle.Id(),
                           .logicalTimeMilliseconds = logicalTimeMilliseconds});
            return Result<PlatformRequestHandle<T>>::Failure(running.ErrorValue());
        }

        const auto id = handle.Id();
        const auto generation = handle.Generation();
        RequestRecord record{.id = id,
                             .generation = generation,
                             .operation = operation,
                             .cancellationDelayMilliseconds = response->cancellationDelayMilliseconds,
                             .acknowledgeCancellation = response->acknowledgeCancellation,
                             .requestCancellation =
                                 [this, id, generation]() {
            return requests.RequestCancel<T>(id, generation);
        },
                             .completeCancellation = CancellationCompletion<T>(id, generation)};
        requestsInFlight.push_back(std::move(record));

        ScheduleEvent(id, generation, operation, MockCompletionKind::Provider, response->delayMilliseconds,
                      ProviderCompletion<T>(id, generation, response));
        if (response->timeoutAfterMilliseconds)
            ScheduleEvent(id, generation, operation, MockCompletionKind::Timeout, *response->timeoutAfterMilliseconds,
                          TimeoutCompletion<T>(id, generation));
        if (response->duplicateDelayMilliseconds) {
            const auto duplicateDelay = response->delayMilliseconds + *response->duplicateDelayMilliseconds;
            ScheduleEvent(id, generation, operation, MockCompletionKind::Provider, duplicateDelay,
                          ProviderCompletion<T>(id, generation, response));
        }
        return Result<PlatformRequestHandle<T>>::Success(std::move(handle));
    }

    Result<void> MockPlatformServicesBackend::Impl::RequestCancel(const PlatformRequestId id, const PlatformRequestGeneration generation) {
        const auto call = RecordCall(MockPlatformServicesOperation::RequestCancel, id);
        if (call.HasError())
            return detail::Failure<void>();
        if (!active_ || closed_) {
            AddDiagnostic({.kind = MockDiagnosticKind::InvalidRequest,
                           .actual = MockPlatformServicesOperation::RequestCancel,
                           .expectationIndex = nextExpected,
                           .request = id,
                           .logicalTimeMilliseconds = logicalTimeMilliseconds});
            return detail::Failure<void>();
        }

        auto *record = FindRequest(id, generation);
        if (record == nullptr) {
            AddDiagnostic({.kind = MockDiagnosticKind::UnexpectedCancellation,
                           .actual = MockPlatformServicesOperation::RequestCancel,
                           .expectationIndex = nextExpected,
                           .request = id,
                           .logicalTimeMilliseconds = logicalTimeMilliseconds});
            return detail::Failure<void>();
        }
        if (record->terminalKind)
            return Result<void>::Success();

        const auto requested = record->requestCancellation();
        if (requested.HasError()) {
            AddDiagnostic({.kind = MockDiagnosticKind::CompletionRejected,
                           .actual = MockPlatformServicesOperation::RequestCancel,
                           .expectationIndex = nextExpected,
                           .request = id,
                           .logicalTimeMilliseconds = logicalTimeMilliseconds});
            return Result<void>::Failure(requested.ErrorValue());
        }
        if (!record->acknowledgeCancellation || record->cancellationScheduled)
            return Result<void>::Success();

        record->cancellationScheduled = true;
        ScheduleEvent(id, generation, record->operation, MockCompletionKind::Cancellation, record->cancellationDelayMilliseconds,
                      record->completeCancellation);
        return Result<void>::Success();
    }

    Result<void> MockPlatformServicesBackend::Impl::AdvanceClock(const std::chrono::milliseconds elapsed) {
        if (closed_ || elapsed.count() < 0 || static_cast<std::uint64_t>(elapsed.count()) > detail::MaximumDelayMilliseconds ||
            logicalTimeMilliseconds >
                std::numeric_limits<std::uint64_t>::max() - detail::ToMilliseconds(elapsed) - detail::MaximumDelayMilliseconds) {
            AddDiagnostic({.kind = MockDiagnosticKind::InvalidScript,
                           .expectationIndex = nextExpected,
                           .logicalTimeMilliseconds = logicalTimeMilliseconds});
            return detail::Failure<void>();
        }
        logicalTimeMilliseconds += detail::ToMilliseconds(elapsed);
        return Result<void>::Success();
    }

    std::size_t MockPlatformServicesBackend::Impl::DispatchDueCompletions(const std::size_t maxCount) {
        if (closed_ || maxCount == 0)
            return 0;

        std::size_t dispatched{};
        while (dispatched < maxCount && !events.empty() && events.top().dueMilliseconds <= logicalTimeMilliseconds) {
            auto event = events.top();
            events.pop();
            auto *record = FindRequest(event.request, event.generation);
            const auto completed = event.complete();
            if (completed.HasError()) {
                AddDiagnostic({.kind = MockDiagnosticKind::CompletionRejected,
                               .actual = event.operation,
                               .expectationIndex = nextExpected,
                               .request = event.request,
                               .logicalTimeMilliseconds = logicalTimeMilliseconds});
            } else if (completed.Value() == PlatformRequestMutation::Applied) {
                if (record != nullptr)
                    record->terminalKind = event.kind;
            } else {
                const bool duplicateProviderCompletion =
                    record != nullptr && record->terminalKind == MockCompletionKind::Provider && event.kind == MockCompletionKind::Provider;
                AddDiagnostic({.kind = duplicateProviderCompletion ? MockDiagnosticKind::DuplicateCompletionIgnored
                                                                   : MockDiagnosticKind::LateCompletionIgnored,
                               .actual = event.operation,
                               .expectationIndex = nextExpected,
                               .request = event.request,
                               .logicalTimeMilliseconds = logicalTimeMilliseconds});
            }

            completionOrder.push_back({.operation = event.operation,
                                       .kind = event.kind,
                                       .mutation = completed.HasValue() ? completed.Value() : PlatformRequestMutation::Unchanged,
                                       .request = event.request,
                                       .logicalTimeMilliseconds = logicalTimeMilliseconds});
            if (record != nullptr && record->pendingEvents > 0)
                --record->pendingEvents;
            RetireRequestIfIdle(event.request, event.generation);
            ++dispatched;
        }
        return dispatched;
    }

    Result<PlatformRequestHandle<void>> MockPlatformServicesBackend::UnlockAchievement(AchievementUnlockRequest) {
        return impl_->Submit<void>(MockPlatformServicesOperation::UnlockAchievement);
    }

    Result<PlatformRequestHandle<void>> MockPlatformServicesBackend::SubmitScore(LeaderboardScoreRequest) {
        return impl_->Submit<void>(MockPlatformServicesOperation::SubmitScore);
    }

    Result<PlatformRequestHandle<void>> MockPlatformServicesBackend::WriteStat(StatWriteRequest) {
        return impl_->Submit<void>(MockPlatformServicesOperation::WriteStat);
    }

    Result<PlatformRequestHandle<CloudReadResult>> MockPlatformServicesBackend::ReadCloudObject(CloudReadRequest) {
        return impl_->Submit<CloudReadResult>(MockPlatformServicesOperation::ReadCloudObject);
    }

    Result<PlatformRequestHandle<void>> MockPlatformServicesBackend::WriteCloudObject(CloudWriteRequest request) {
        return impl_->Submit<void>(MockPlatformServicesOperation::WriteCloudObject, request.bytes.size() <= MaximumPayloadBytes);
    }

    Result<PlatformRequestHandle<void>> MockPlatformServicesBackend::SetPresence(PresenceUpdateRequest request) {
        return impl_->Submit<void>(MockPlatformServicesOperation::SetPresence, request.detail.size() <= MaximumPayloadBytes);
    }

    Result<PlatformRequestHandle<void>> MockPlatformServicesBackend::ClearPresence(PlatformSubjectHandle) {
        return impl_->Submit<void>(MockPlatformServicesOperation::ClearPresence);
    }

    Result<PlatformRequestHandle<FriendsPage>> MockPlatformServicesBackend::QueryFriends(FriendsQuery query) {
        const bool validPageSize = query.pageSize > 0 && query.pageSize <= MaximumPageEntries;
        return impl_->Submit<FriendsPage>(MockPlatformServicesOperation::QueryFriends, validPageSize);
    }

    Result<PlatformRequestHandle<PlatformSessionSnapshot>> MockPlatformServicesBackend::QueryCurrentSession() {
        return impl_->Submit<PlatformSessionSnapshot>(MockPlatformServicesOperation::QueryCurrentSession);
    }
}  // namespace Horo::PlatformServices::TestSupport
