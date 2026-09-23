#include "PlatformServicesMockBackendImpl.h"

#include <algorithm>

namespace Horo::PlatformServices::TestSupport {
    MockPlatformServicesBackend::MockPlatformServicesBackend(const PlatformProviderGeneration generation)
        : impl_(std::make_unique<Impl>(generation)) {}

    MockPlatformServicesBackend::~MockPlatformServicesBackend() = default;

    Result<void> MockPlatformServicesBackend::ExpectSequence(std::vector<MockPlatformServicesOperation> operations) {
        if (impl_->expectedSequenceConfigured_ || impl_->expectationsVerified_ || !impl_->calls.empty() ||
            operations.size() > MaximumExpectedCalls || std::ranges::any_of(operations, [](const MockPlatformServicesOperation operation) {
            return !detail::IsKnownOperation(operation);
        })) {
            impl_->AddDiagnostic({.kind = operations.size() > MaximumExpectedCalls ? MockDiagnosticKind::CapacityExceeded
                                                                                   : MockDiagnosticKind::InvalidScript,
                                  .expectationIndex = impl_->nextExpected,
                                  .logicalTimeMilliseconds = impl_->logicalTimeMilliseconds});
            return detail::Failure<void>();
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
            return detail::Failure<void>();
        }

        Impl::ScriptedResponse scripted{.payload = std::move(response.payload),
                                        .delayMilliseconds = detail::ToMilliseconds(response.delay),
                                        .timeoutAfterMilliseconds =
                                            response.timeoutAfter
                                                ? std::optional<std::uint64_t>{detail::ToMilliseconds(*response.timeoutAfter)}
                                                : std::nullopt,
                                        .duplicateDelayMilliseconds =
                                            response.duplicateDelay
                                                ? std::optional<std::uint64_t>{detail::ToMilliseconds(*response.duplicateDelay)}
                                                : std::nullopt,
                                        .cancellationDelayMilliseconds = detail::ToMilliseconds(response.cancellationDelay),
                                        .acknowledgeCancellation = response.acknowledgeCancellation};
        if (response.failure)
            scripted.error = MakeError(*response.failure->descriptor, std::move(response.failure->message));

        impl_->responses[detail::OperationIndex(operation)].push_back(std::move(scripted));
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
}  // namespace Horo::PlatformServices::TestSupport
