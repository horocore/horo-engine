#include "Horo/PlatformServices/PlatformRequestErrors.h"
#include "PlatformServicesMockBackendImpl.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Horo::PlatformServices::TestSupport {
    namespace detail {
        bool IsKnownOperation(const MockPlatformServicesOperation operation) noexcept {
            return operation < MockPlatformServicesOperation::Count;
        }

        bool IsServiceOperation(const MockPlatformServicesOperation operation) noexcept {
            return IsKnownOperation(operation) && operation != MockPlatformServicesOperation::RequestCancel;
        }

        std::size_t OperationIndex(const MockPlatformServicesOperation operation) noexcept {
            return static_cast<std::size_t>(operation);
        }

        bool IsBoundedDelay(const std::chrono::milliseconds duration) noexcept {
            return duration.count() >= 0 && static_cast<std::uint64_t>(duration.count()) <= MaximumDelayMilliseconds;
        }

        std::uint64_t ToMilliseconds(const std::chrono::milliseconds duration) noexcept {
            return static_cast<std::uint64_t>(duration.count());
        }

        std::uint8_t EventPriority(const MockCompletionKind kind) noexcept {
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
    }  // namespace detail

    MockPlatformServicesBackend::Impl::Impl(const PlatformProviderGeneration generation)
        : providerGeneration(generation),
          requests({.activeCapacity = 256, .terminalCapacity = 256, .observerCapacity = 256, .generation = {1}}) {
        expected.reserve(MockPlatformServicesBackend::MaximumExpectedCalls);
        diagnostics.reserve(MockPlatformServicesBackend::MaximumDiagnostics);
        calls.reserve(MockPlatformServicesBackend::MaximumExpectedCalls);
        completionOrder.reserve(MockPlatformServicesBackend::MaximumScheduledEvents);
        requestsInFlight.reserve(MockPlatformServicesBackend::MaximumExpectedCalls);
        for (auto &queue : responses)
            queue.reserve(MockPlatformServicesBackend::MaximumScriptedResponses);
    }

    Result<void> MockPlatformServicesBackend::Impl::RecordCall(const MockPlatformServicesOperation operation,
                                                               const PlatformRequestId request) {
        if (expectationsVerified_) {
            AddDiagnostic({.kind = MockDiagnosticKind::InvalidScript,
                           .actual = operation,
                           .expectationIndex = nextExpected,
                           .request = request,
                           .logicalTimeMilliseconds = logicalTimeMilliseconds});
            return detail::Failure<void>();
        }
        if (calls.size() >= MockPlatformServicesBackend::MaximumExpectedCalls) {
            AddDiagnostic({.kind = MockDiagnosticKind::CapacityExceeded,
                           .actual = operation,
                           .expectationIndex = nextExpected,
                           .request = request,
                           .logicalTimeMilliseconds = logicalTimeMilliseconds});
            return detail::Failure<void>();
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
            return detail::Failure<void>();
        }
        ++nextExpected;
        return Result<void>::Success();
    }

    Result<MockPlatformServicesBackend::Impl::ScriptedResponse> MockPlatformServicesBackend::Impl::TakeResponse(
        const MockPlatformServicesOperation operation) {
        const auto index = detail::OperationIndex(operation);
        if (!detail::IsServiceOperation(operation) || responseCursors[index] >= responses[index].size()) {
            AddDiagnostic({.kind = MockDiagnosticKind::MissingResponse,
                           .actual = operation,
                           .expectationIndex = nextExpected,
                           .logicalTimeMilliseconds = logicalTimeMilliseconds});
            return detail::Failure<ScriptedResponse>();
        }
        return Result<ScriptedResponse>::Success(std::move(responses[index][responseCursors[index]++]));
    }

    bool MockPlatformServicesBackend::Impl::ValidateFailureResponse(const MockPlatformServicesResponse &response) noexcept {
        if (!response.failure || response.failure->descriptor == nullptr || !std::holds_alternative<std::monostate>(response.payload) ||
            response.failure->message.size() > detail::MaximumErrorTextBytes)
            return !response.failure.has_value();
        const auto &descriptor = *response.failure->descriptor;
        const auto &code = descriptor.code.Value();
        if (code == "platform.provider.null" || code == "platform.capability.unavailable" || code == "platform.request.cancelled" ||
            code == "platform.request.timed_out")
            return false;
        return !descriptor.domain.Value().empty() && descriptor.domain.Value().size() <= detail::MaximumErrorIdentityBytes &&
               !code.empty() && code.size() <= detail::MaximumErrorIdentityBytes &&
               descriptor.summary.size() <= detail::MaximumErrorTextBytes &&
               descriptor.remediationHint.size() <= detail::MaximumErrorTextBytes;
    }

    bool MockPlatformServicesBackend::Impl::ValidateResponsePayload(const MockPlatformServicesOperation operation,
                                                                    const MockPlatformServicesResponse &response) noexcept {
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

    bool MockPlatformServicesBackend::Impl::ValidateResponse(const MockPlatformServicesOperation operation,
                                                             const MockPlatformServicesResponse &response) const noexcept {
        if (!detail::IsServiceOperation(operation) || !detail::IsBoundedDelay(response.delay) ||
            !detail::IsBoundedDelay(response.cancellationDelay) ||
            (response.timeoutAfter && !detail::IsBoundedDelay(*response.timeoutAfter)) ||
            (response.duplicateDelay && !detail::IsBoundedDelay(*response.duplicateDelay)) ||
            (response.duplicateDelay &&
             detail::ToMilliseconds(response.delay) + detail::ToMilliseconds(*response.duplicateDelay) > detail::MaximumDelayMilliseconds))
            return false;
        if (response.failure)
            return ValidateFailureResponse(response);
        return ValidateResponsePayload(operation, response);
    }

    void MockPlatformServicesBackend::Impl::AddDiagnostic(MockPlatformServicesDiagnostic diagnostic) noexcept {
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

    Result<void> MockPlatformServicesBackend::Impl::VerifyExpectations() {
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
        return AllExpectationsMet() ? Result<void>::Success() : detail::Failure<void>();
    }

    bool MockPlatformServicesBackend::Impl::AllExpectationsMet() const noexcept {
        if (!expectedSequenceConfigured_ || nextExpected != expected.size() || unexpectedCallCount != 0 || executionFailure)
            return false;
        for (std::size_t operation = 0; operation < responses.size(); ++operation) {
            if (responseCursors[operation] != responses[operation].size())
                return false;
        }
        return true;
    }

    Result<PlatformServiceCapabilitySnapshot> MockPlatformServicesBackend::Impl::InspectCapabilities() const {
        if (!providerGeneration.IsValid())
            return Result<PlatformServiceCapabilitySnapshot>::Failure(MakeError(BackendErrors::InvalidCapabilitySnapshot));

        constexpr PlatformServiceLimits Limits{.maxConcurrentRequests = 256,
                                               .maxPageEntries = MockPlatformServicesBackend::MaximumPageEntries,
                                               .maxPayloadBytes = MockPlatformServicesBackend::MaximumPayloadBytes};
        PlatformServiceCapabilitySnapshot snapshot{.interfaceVersion = {PlatformServicesBackendInterfaceMajor,
                                                                        PlatformServicesBackendInterfaceMinor},
                                                   .provider = {detail::MockProviderIdentity},
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

    Result<void> MockPlatformServicesBackend::Impl::Activate(const PlatformServicesBackendConfig &config) {
        if (closed_)
            return detail::Failure<void>();
        const auto snapshot = InspectCapabilities();
        if (snapshot.HasError())
            return Result<void>::Failure(snapshot.ErrorValue());
        if (const auto validation = ValidatePlatformServiceCapabilitySnapshot(snapshot.Value(), config); validation.HasError())
            return validation;
        active_ = true;
        return Result<void>::Success();
    }

    Result<void> MockPlatformServicesBackend::Impl::Shutdown() {
        if (closed_)
            return Result<void>::Success();
        active_ = false;
        closed_ = true;
        requests.Shutdown();
        events = {};
        requestsInFlight.clear();
        return Result<void>::Success();
    }
}  // namespace Horo::PlatformServices::TestSupport
