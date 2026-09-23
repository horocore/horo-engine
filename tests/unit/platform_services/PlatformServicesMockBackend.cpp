#include "PlatformServicesMockBackend.h"

#include "Horo/PlatformServices/PlatformRequestErrors.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

namespace Horo::PlatformServices::TestSupport {
    namespace {
        constexpr std::uint64_t MockProviderIdentity = 0x4d4f434bULL;
        constexpr std::uint64_t MaximumDelayMilliseconds = 24ULL * 60ULL * 60ULL * 1000ULL;
        constexpr std::size_t MaximumErrorTextBytes = 512;
        constexpr std::size_t MaximumErrorIdentityBytes = 128;

        const ErrorCodeDescriptor ScriptFailure{.domain = ErrorDomainId{"horo.platform.mock"},
                                                .code = ErrorCode{"platform.mock.script_failure"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "The deterministic platform mock rejected a call or script.",
                                                .remediationHint = "Correct the bounded mock script and expected call sequence.",
                                                .retryable = false,
                                                .userActionable = false};

        [[nodiscard]] bool IsKnownOperation(const MockPlatformServicesOperation operation) noexcept {
            return operation < MockPlatformServicesOperation::Count;
        }

        [[nodiscard]] bool IsServiceOperation(const MockPlatformServicesOperation operation) noexcept {
            return IsKnownOperation(operation) && operation != MockPlatformServicesOperation::RequestCancel;
        }

        [[nodiscard]] std::size_t OperationIndex(const MockPlatformServicesOperation operation) noexcept {
            return static_cast<std::size_t>(operation);
        }

        [[nodiscard]] bool IsBoundedDelay(const std::chrono::milliseconds duration) noexcept {
            return duration.count() >= 0 && static_cast<std::uint64_t>(duration.count()) <= MaximumDelayMilliseconds;
        }

        [[nodiscard]] std::uint64_t ToMilliseconds(const std::chrono::milliseconds duration) noexcept {
            return static_cast<std::uint64_t>(duration.count());
        }

        [[nodiscard]] std::uint8_t EventPriority(const MockCompletionKind kind) noexcept {
            switch (kind) {
                case MockCompletionKind::Provider:
                    return 0;
                case MockCompletionKind::Cancellation:
                    return 1;
                case MockCompletionKind::Timeout:
                    return 2;
            }
            return 3;
        }

        template <typename T> [[nodiscard]] Result<T> Failure() {
            return Result<T>::Failure(MakeError(ScriptFailure));
        }
    }  // namespace

    struct MockPlatformServicesBackend::Impl final {
        struct ScriptedResponse final {
            std::optional<Error> error;
            std::variant<std::monostate, CloudReadResult, FriendsPage, PlatformSessionSnapshot> payload;
            std::uint64_t delayMilliseconds{};
            std::optional<std::uint64_t> timeoutAfterMilliseconds;
            std::optional<std::uint64_t> duplicateDelayMilliseconds;
            std::uint64_t cancellationDelayMilliseconds{};
            bool acknowledgeCancellation{};
        };

        struct RequestRecord final {
            PlatformRequestId id;
            PlatformRequestGeneration generation;
            MockPlatformServicesOperation operation{MockPlatformServicesOperation::Count};
            std::uint64_t cancellationDelayMilliseconds{};
            bool acknowledgeCancellation{};
            bool cancellationScheduled{};
            std::size_t pendingEvents{};
            std::optional<MockCompletionKind> terminalKind;
            std::function<Result<PlatformRequestMutation>()> requestCancellation;
            std::function<Result<PlatformRequestMutation>()> completeCancellation;
        };

        struct ScheduledEvent final {
            std::uint64_t dueMilliseconds{};
            std::uint64_t sequence{};
            PlatformRequestId request;
            PlatformRequestGeneration generation;
            MockPlatformServicesOperation operation{MockPlatformServicesOperation::Count};
            MockCompletionKind kind{MockCompletionKind::Provider};
            std::function<Result<PlatformRequestMutation>()> complete;
        };

        explicit Impl(const PlatformProviderGeneration generation)
            : providerGeneration(generation),
              requests({.activeCapacity = 256, .terminalCapacity = 256, .observerCapacity = 256, .generation = {1}}) {
            expected.reserve(MockPlatformServicesBackend::MaximumExpectedCalls);
            diagnostics.reserve(MockPlatformServicesBackend::MaximumDiagnostics);
            calls.reserve(MockPlatformServicesBackend::MaximumExpectedCalls);
            completionOrder.reserve(MockPlatformServicesBackend::MaximumScheduledEvents);
            requestsInFlight.reserve(MockPlatformServicesBackend::MaximumExpectedCalls);
            events.reserve(MockPlatformServicesBackend::MaximumScheduledEvents);
            for (auto &queue : responses)
                queue.reserve(MockPlatformServicesBackend::MaximumScriptedResponses);
        }

        [[nodiscard]] Result<void> RecordCall(const MockPlatformServicesOperation operation, const PlatformRequestId request = {}) {
            if (expectationsVerified_) {
                AddDiagnostic({.kind = MockDiagnosticKind::InvalidScript,
                               .actual = operation,
                               .expectationIndex = nextExpected,
                               .request = request,
                               .logicalTimeMilliseconds = logicalTimeMilliseconds});
                return Failure<void>();
            }
            if (calls.size() >= MockPlatformServicesBackend::MaximumExpectedCalls) {
                AddDiagnostic({.kind = MockDiagnosticKind::CapacityExceeded,
                               .actual = operation,
                               .expectationIndex = nextExpected,
                               .request = request,
                               .logicalTimeMilliseconds = logicalTimeMilliseconds});
                return Failure<void>();
            }

            calls.push_back(operation);
            const auto expectedOperation = nextExpected < expected.size() ? expected[nextExpected] : MockPlatformServicesOperation::Count;
            if (!expectedSequenceConfigured_ || expectedOperation != operation) {
                ++unexpectedCallCount;
                AddDiagnostic({.kind = MockDiagnosticKind::UnexpectedCall,
                               .expected = expectedOperation,
                               .actual = operation,
                               .expectationIndex = nextExpected,
                               .request = request,
                               .logicalTimeMilliseconds = logicalTimeMilliseconds});
                return Failure<void>();
            }
            ++nextExpected;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<ScriptedResponse> TakeResponse(const MockPlatformServicesOperation operation) {
            const auto index = OperationIndex(operation);
            if (!IsServiceOperation(operation) || responseCursors[index] >= responses[index].size()) {
                AddDiagnostic({.kind = MockDiagnosticKind::MissingResponse,
                               .actual = operation,
                               .expectationIndex = nextExpected,
                               .logicalTimeMilliseconds = logicalTimeMilliseconds});
                return Failure<ScriptedResponse>();
            }
            return Result<ScriptedResponse>::Success(std::move(responses[index][responseCursors[index]++]));
        }

        [[nodiscard]] bool ValidateResponse(const MockPlatformServicesOperation operation,
                                            const MockPlatformServicesResponse &response) const noexcept {
            if (!IsServiceOperation(operation) || !IsBoundedDelay(response.delay) || !IsBoundedDelay(response.cancellationDelay))
                return false;
            if (response.timeoutAfter && !IsBoundedDelay(*response.timeoutAfter))
                return false;
            if (response.duplicateDelay && !IsBoundedDelay(*response.duplicateDelay))
                return false;
            if (response.duplicateDelay &&
                ToMilliseconds(response.delay) + ToMilliseconds(*response.duplicateDelay) > MaximumDelayMilliseconds)
                return false;

            if (response.failure) {
                if (response.failure->descriptor == nullptr || !std::holds_alternative<std::monostate>(response.payload) ||
                    response.failure->message.size() > MaximumErrorTextBytes)
                    return false;
                const auto &descriptor = *response.failure->descriptor;
                const auto &code = descriptor.code.Value();
                if (code == "platform.provider.null" || code == "platform.capability.unavailable" || code == "platform.request.cancelled" ||
                    code == "platform.request.timed_out")
                    return false;
                return !descriptor.domain.Value().empty() && descriptor.domain.Value().size() <= MaximumErrorIdentityBytes &&
                       !code.empty() && code.size() <= MaximumErrorIdentityBytes && descriptor.summary.size() <= MaximumErrorTextBytes &&
                       descriptor.remediationHint.size() <= MaximumErrorTextBytes;
            }

            switch (operation) {
                case MockPlatformServicesOperation::UnlockAchievement:
                case MockPlatformServicesOperation::SubmitScore:
                case MockPlatformServicesOperation::WriteStat:
                case MockPlatformServicesOperation::WriteCloudObject:
                case MockPlatformServicesOperation::SetPresence:
                case MockPlatformServicesOperation::ClearPresence:
                    return std::holds_alternative<std::monostate>(response.payload);
                case MockPlatformServicesOperation::ReadCloudObject: {
                    const auto *payload = std::get_if<CloudReadResult>(&response.payload);
                    return payload != nullptr && payload->bytes.size() <= MockPlatformServicesBackend::MaximumPayloadBytes;
                }
                case MockPlatformServicesOperation::QueryFriends: {
                    const auto *payload = std::get_if<FriendsPage>(&response.payload);
                    if (payload == nullptr || payload->entries.size() > MockPlatformServicesBackend::MaximumPageEntries)
                        return false;
                    std::size_t displayBytes{};
                    for (const auto &entry : payload->entries) {
                        if (entry.displayName.size() > MockPlatformServicesBackend::MaximumPayloadBytes - displayBytes)
                            return false;
                        displayBytes += entry.displayName.size();
                    }
                    return true;
                }
                case MockPlatformServicesOperation::QueryCurrentSession:
                    return std::holds_alternative<PlatformSessionSnapshot>(response.payload);
                case MockPlatformServicesOperation::RequestCancel:
                case MockPlatformServicesOperation::Count:
                    return false;
            }
            return false;
        }

        void AddDiagnostic(MockPlatformServicesDiagnostic diagnostic) noexcept {
            if (diagnostic.kind != MockDiagnosticKind::DuplicateCompletionIgnored &&
                diagnostic.kind != MockDiagnosticKind::LateCompletionIgnored)
                executionFailure = true;
            if (diagnostics.size() < MockPlatformServicesBackend::MaximumDiagnostics - 1) {
                diagnostics.push_back(diagnostic);
                return;
            }
            ++diagnosticOverflowCount;
            executionFailure = true;
            if (diagnostics.size() == MockPlatformServicesBackend::MaximumDiagnostics - 1) {
                diagnostic.kind = MockDiagnosticKind::DiagnosticOverflow;
                diagnostic.expected = MockPlatformServicesOperation::Count;
                diagnostic.actual = MockPlatformServicesOperation::Count;
                diagnostic.request = {};
                diagnostics.push_back(diagnostic);
            }
        }

        void ScheduleEvent(const PlatformRequestId request, const PlatformRequestGeneration generation,
                           const MockPlatformServicesOperation operation, const MockCompletionKind kind,
                           const std::uint64_t delayMilliseconds, std::function<Result<PlatformRequestMutation>()> complete) {
            const auto dueMilliseconds = logicalTimeMilliseconds + delayMilliseconds;
            events.push_back({.dueMilliseconds = dueMilliseconds,
                              .sequence = nextEventSequence++,
                              .request = request,
                              .generation = generation,
                              .operation = operation,
                              .kind = kind,
                              .complete = std::move(complete)});
            if (auto *record = FindRequest(request, generation); record != nullptr)
                ++record->pendingEvents;
        }

        [[nodiscard]] RequestRecord *FindRequest(const PlatformRequestId id, const PlatformRequestGeneration generation) noexcept {
            const auto found = std::ranges::find_if(requestsInFlight, [id, generation](const RequestRecord &record) {
                return record.id == id && record.generation == generation;
            });
            return found == requestsInFlight.end() ? nullptr : std::to_address(found);
        }

        void RetireRequestIfIdle(const PlatformRequestId id, const PlatformRequestGeneration generation) {
            const auto found = std::ranges::find_if(requestsInFlight, [id, generation](const RequestRecord &record) {
                return record.id == id && record.generation == generation;
            });
            if (found != requestsInFlight.end() && found->pendingEvents == 0)
                requestsInFlight.erase(found);
        }

        template <typename T>
        [[nodiscard]] std::function<Result<PlatformRequestMutation>()> ProviderCompletion(
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
        [[nodiscard]] std::function<Result<PlatformRequestMutation>()> TimeoutCompletion(const PlatformRequestId id,
                                                                                         const PlatformRequestGeneration generation) {
            return [this, id, generation]() {
                return requests.CompleteTimedOut<T>(id, generation, MakeError(RequestErrors::TimedOut));
            };
        }

        template <typename T>
        [[nodiscard]] std::function<Result<PlatformRequestMutation>()> CancellationCompletion(const PlatformRequestId id,
                                                                                              const PlatformRequestGeneration generation) {
            return [this, id, generation]() {
                return requests.CompleteCancelled<T>(id, generation, MakeError(RequestErrors::Cancelled));
            };
        }

        template <typename T>
        [[nodiscard]] Result<PlatformRequestHandle<T>> Submit(const MockPlatformServicesOperation operation,
                                                              const bool requestIsValid = true) {
            const auto call = RecordCall(operation);
            if (call.HasError())
                return Failure<PlatformRequestHandle<T>>();
            if (!active_ || closed_) {
                AddDiagnostic({.kind = MockDiagnosticKind::InvalidRequest,
                               .actual = operation,
                               .expectationIndex = nextExpected,
                               .logicalTimeMilliseconds = logicalTimeMilliseconds});
                return Failure<PlatformRequestHandle<T>>();
            }
            if (!requestIsValid) {
                AddDiagnostic({.kind = MockDiagnosticKind::InvalidRequest,
                               .actual = operation,
                               .expectationIndex = nextExpected,
                               .logicalTimeMilliseconds = logicalTimeMilliseconds});
                return Failure<PlatformRequestHandle<T>>();
            }

            auto responseResult = TakeResponse(operation);
            if (responseResult.HasError())
                return Failure<PlatformRequestHandle<T>>();
            auto response = std::make_shared<const ScriptedResponse>(std::move(responseResult).Value());

            const auto eventCount = 1U + static_cast<unsigned int>(response->timeoutAfterMilliseconds.has_value()) +
                                    static_cast<unsigned int>(response->duplicateDelayMilliseconds.has_value());
            if (events.size() + eventCount > MockPlatformServicesBackend::MaximumScheduledEvents ||
                requestsInFlight.size() >= MockPlatformServicesBackend::MaximumExpectedCalls) {
                AddDiagnostic({.kind = MockDiagnosticKind::CapacityExceeded,
                               .actual = operation,
                               .expectationIndex = nextExpected,
                               .logicalTimeMilliseconds = logicalTimeMilliseconds});
                return Failure<PlatformRequestHandle<T>>();
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

        [[nodiscard]] Result<void> RequestCancel(const PlatformRequestId id, const PlatformRequestGeneration generation) {
            const auto call = RecordCall(MockPlatformServicesOperation::RequestCancel, id);
            if (call.HasError())
                return Failure<void>();
            if (!active_ || closed_) {
                AddDiagnostic({.kind = MockDiagnosticKind::InvalidRequest,
                               .actual = MockPlatformServicesOperation::RequestCancel,
                               .expectationIndex = nextExpected,
                               .request = id,
                               .logicalTimeMilliseconds = logicalTimeMilliseconds});
                return Failure<void>();
            }

            auto *record = FindRequest(id, generation);
            if (record == nullptr) {
                AddDiagnostic({.kind = MockDiagnosticKind::UnexpectedCancellation,
                               .actual = MockPlatformServicesOperation::RequestCancel,
                               .expectationIndex = nextExpected,
                               .request = id,
                               .logicalTimeMilliseconds = logicalTimeMilliseconds});
                return Failure<void>();
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

        [[nodiscard]] Result<void> VerifyExpectations() {
            if (!expectationsVerified_) {
                expectationsVerified_ = true;
                if (!expectedSequenceConfigured_) {
                    AddDiagnostic({.kind = MockDiagnosticKind::InvalidScript,
                                   .expectationIndex = nextExpected,
                                   .logicalTimeMilliseconds = logicalTimeMilliseconds});
                }
                for (std::size_t index = nextExpected; index < expected.size(); ++index) {
                    AddDiagnostic({.kind = MockDiagnosticKind::MissingExpectedCall,
                                   .expected = expected[index],
                                   .expectationIndex = index,
                                   .logicalTimeMilliseconds = logicalTimeMilliseconds});
                }
                for (std::size_t operation = 0; operation < responses.size(); ++operation) {
                    for (std::size_t response = responseCursors[operation]; response < responses[operation].size(); ++response) {
                        static_cast<void>(response);
                        AddDiagnostic({.kind = MockDiagnosticKind::UnusedResponse,
                                       .actual = static_cast<MockPlatformServicesOperation>(operation),
                                       .expectationIndex = nextExpected,
                                       .logicalTimeMilliseconds = logicalTimeMilliseconds});
                    }
                }
            }
            return AllExpectationsMet() ? Result<void>::Success() : Failure<void>();
        }

        [[nodiscard]] bool AllExpectationsMet() const noexcept {
            if (!expectedSequenceConfigured_ || nextExpected != expected.size() || unexpectedCallCount != 0 || executionFailure)
                return false;
            for (std::size_t operation = 0; operation < responses.size(); ++operation) {
                if (responseCursors[operation] != responses[operation].size())
                    return false;
            }
            return true;
        }

        [[nodiscard]] Result<void> AdvanceClock(const std::chrono::milliseconds elapsed) {
            if (closed_ || elapsed.count() < 0 || static_cast<std::uint64_t>(elapsed.count()) > MaximumDelayMilliseconds ||
                logicalTimeMilliseconds > std::numeric_limits<std::uint64_t>::max() - ToMilliseconds(elapsed) - MaximumDelayMilliseconds) {
                AddDiagnostic({.kind = MockDiagnosticKind::InvalidScript,
                               .expectationIndex = nextExpected,
                               .logicalTimeMilliseconds = logicalTimeMilliseconds});
                return Failure<void>();
            }
            logicalTimeMilliseconds += ToMilliseconds(elapsed);
            return Result<void>::Success();
        }

        [[nodiscard]] std::size_t DispatchDueCompletions(const std::size_t maxCount) {
            if (closed_ || maxCount == 0)
                return 0;

            std::size_t dispatched{};
            while (dispatched < maxCount) {
                auto next = events.end();
                for (auto candidate = events.begin(); candidate != events.end(); ++candidate) {
                    if (candidate->dueMilliseconds > logicalTimeMilliseconds)
                        continue;
                    if (next == events.end() || candidate->dueMilliseconds < next->dueMilliseconds ||
                        (candidate->dueMilliseconds == next->dueMilliseconds &&
                         EventPriority(candidate->kind) < EventPriority(next->kind)) ||
                        (candidate->dueMilliseconds == next->dueMilliseconds && candidate->kind == next->kind &&
                         candidate->sequence < next->sequence))
                        next = candidate;
                }
                if (next == events.end())
                    break;

                auto event = std::move(*next);
                events.erase(next);
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
                    const bool duplicateProviderCompletion = record != nullptr && record->terminalKind == MockCompletionKind::Provider &&
                                                             event.kind == MockCompletionKind::Provider;
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

        [[nodiscard]] Result<PlatformServiceCapabilitySnapshot> InspectCapabilities() const {
            if (!providerGeneration.IsValid())
                return Result<PlatformServiceCapabilitySnapshot>::Failure(MakeError(BackendErrors::InvalidCapabilitySnapshot));

            constexpr PlatformServiceLimits Limits{.maxConcurrentRequests = 256,
                                                   .maxPageEntries = MockPlatformServicesBackend::MaximumPageEntries,
                                                   .maxPayloadBytes = MockPlatformServicesBackend::MaximumPayloadBytes};
            PlatformServiceCapabilitySnapshot snapshot{.interfaceVersion = {PlatformServicesBackendInterfaceMajor,
                                                                            PlatformServicesBackendInterfaceMinor},
                                                       .provider = {MockProviderIdentity},
                                                       .providerGeneration = providerGeneration};
            for (std::size_t index = 0; index < snapshot.services.size(); ++index) {
                snapshot.services[index] = {.service = static_cast<PlatformServiceKind>(index),
                                            .availability = PlatformServiceAvailability::Available,
                                            .limits = Limits,
                                            .binding = PlatformServiceBindingId{index + 1},
                                            .unavailableReason = std::nullopt};
            }
            return Result<PlatformServiceCapabilitySnapshot>::Success(std::move(snapshot));
        }

        [[nodiscard]] Result<void> Activate(const PlatformServicesBackendConfig &config) {
            if (closed_)
                return Failure<void>();
            const auto snapshot = InspectCapabilities();
            if (snapshot.HasError())
                return Result<void>::Failure(snapshot.ErrorValue());
            if (const auto validation = ValidatePlatformServiceCapabilitySnapshot(snapshot.Value(), config); validation.HasError())
                return validation;
            active_ = true;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> Shutdown() {
            if (closed_)
                return Result<void>::Success();
            active_ = false;
            closed_ = true;
            requests.Shutdown();
            events.clear();
            requestsInFlight.clear();
            return Result<void>::Success();
        }

        PlatformProviderGeneration providerGeneration;
        PlatformRequestStore requests;
        std::array<std::vector<ScriptedResponse>, static_cast<std::size_t>(MockPlatformServicesOperation::Count)> responses;
        std::array<std::size_t, static_cast<std::size_t>(MockPlatformServicesOperation::Count)> responseCursors{};
        std::vector<MockPlatformServicesOperation> expected;
        std::size_t nextExpected{};
        std::vector<MockPlatformServicesDiagnostic> diagnostics;
        std::vector<MockPlatformServicesOperation> calls;
        std::vector<MockPlatformServicesCompletion> completionOrder;
        std::vector<RequestRecord> requestsInFlight;
        std::vector<ScheduledEvent> events;
        std::size_t scriptedResponseCount{};
        std::size_t unexpectedCallCount{};
        std::uint64_t diagnosticOverflowCount{};
        std::uint64_t logicalTimeMilliseconds{};
        std::uint64_t nextEventSequence{};
        bool expectedSequenceConfigured_{};
        bool expectationsVerified_{};
        bool executionFailure{};
        bool active_{};
        bool closed_{};
    };

    MockPlatformServicesBackend::MockPlatformServicesBackend(const PlatformProviderGeneration generation)
        : impl_(std::make_unique<Impl>(generation)) {}

    MockPlatformServicesBackend::~MockPlatformServicesBackend() = default;

    Result<void> MockPlatformServicesBackend::ExpectSequence(std::vector<MockPlatformServicesOperation> operations) {
        if (impl_->expectedSequenceConfigured_ || impl_->expectationsVerified_ || !impl_->calls.empty() ||
            operations.size() > MaximumExpectedCalls || std::ranges::any_of(operations, [](const MockPlatformServicesOperation operation) {
            return !IsKnownOperation(operation);
        })) {
            impl_->AddDiagnostic({.kind = operations.size() > MaximumExpectedCalls ? MockDiagnosticKind::CapacityExceeded
                                                                                   : MockDiagnosticKind::InvalidScript,
                                  .expectationIndex = impl_->nextExpected,
                                  .logicalTimeMilliseconds = impl_->logicalTimeMilliseconds});
            return Failure<void>();
        }
        impl_->expected = std::move(operations);
        impl_->expectedSequenceConfigured_ = true;
        return Result<void>::Success();
    }

    Result<void> MockPlatformServicesBackend::SetResponse(const MockPlatformServicesOperation operation,
                                                          MockPlatformServicesResponse response) {
        if (impl_->expectationsVerified_ || !impl_->ValidateResponse(operation, response) ||
            impl_->scriptedResponseCount >= MaximumScriptedResponses) {
            const bool atCapacity = impl_->scriptedResponseCount >= MaximumScriptedResponses;
            impl_->AddDiagnostic({.kind = atCapacity ? MockDiagnosticKind::CapacityExceeded : MockDiagnosticKind::InvalidScript,
                                  .actual = operation,
                                  .expectationIndex = impl_->nextExpected,
                                  .logicalTimeMilliseconds = impl_->logicalTimeMilliseconds});
            return Failure<void>();
        }

        Impl::ScriptedResponse scripted{.payload = std::move(response.payload),
                                        .delayMilliseconds = ToMilliseconds(response.delay),
                                        .timeoutAfterMilliseconds =
                                            response.timeoutAfter ? std::optional<std::uint64_t>{ToMilliseconds(*response.timeoutAfter)}
                                                                  : std::nullopt,
                                        .duplicateDelayMilliseconds =
                                            response.duplicateDelay ? std::optional<std::uint64_t>{ToMilliseconds(*response.duplicateDelay)}
                                                                    : std::nullopt,
                                        .cancellationDelayMilliseconds = ToMilliseconds(response.cancellationDelay),
                                        .acknowledgeCancellation = response.acknowledgeCancellation};
        if (response.failure)
            scripted.error = MakeError(*response.failure->descriptor, std::move(response.failure->message));

        impl_->responses[OperationIndex(operation)].push_back(std::move(scripted));
        ++impl_->scriptedResponseCount;
        return Result<void>::Success();
    }

    Result<void> MockPlatformServicesBackend::VerifyExpectations() {
        return impl_->VerifyExpectations();
    }

    bool MockPlatformServicesBackend::AllExpectationsMet() const noexcept {
        return impl_->AllExpectationsMet();
    }

    std::span<const MockPlatformServicesDiagnostic> MockPlatformServicesBackend::Diagnostics() const noexcept {
        return impl_->diagnostics;
    }

    std::uint64_t MockPlatformServicesBackend::DiagnosticOverflowCount() const noexcept {
        return impl_->diagnosticOverflowCount;
    }

    std::span<const MockPlatformServicesCompletion> MockPlatformServicesBackend::CompletionOrder() const noexcept {
        return impl_->completionOrder;
    }

    std::span<const MockPlatformServicesOperation> MockPlatformServicesBackend::Calls() const noexcept {
        return impl_->calls;
    }

    PlatformRequestStore &MockPlatformServicesBackend::Requests() noexcept {
        return impl_->requests;
    }

    std::uint64_t MockPlatformServicesBackend::CurrentTimeMilliseconds() const noexcept {
        return impl_->logicalTimeMilliseconds;
    }

    Result<void> MockPlatformServicesBackend::AdvanceClock(const std::chrono::milliseconds elapsed) {
        return impl_->AdvanceClock(elapsed);
    }

    std::size_t MockPlatformServicesBackend::DispatchDueCompletions(const std::size_t maxCount) {
        return impl_->DispatchDueCompletions(maxCount);
    }

    Result<PlatformServiceCapabilitySnapshot> MockPlatformServicesBackend::InspectCapabilities() const {
        return impl_->InspectCapabilities();
    }

    Result<void> MockPlatformServicesBackend::Activate(const PlatformServicesBackendConfig &config) {
        return impl_->Activate(config);
    }

    Result<void> MockPlatformServicesBackend::RequestCancel(const PlatformRequestId request, const PlatformRequestGeneration generation) {
        return impl_->RequestCancel(request, generation);
    }

    Result<void> MockPlatformServicesBackend::Shutdown() {
        return impl_->Shutdown();
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
