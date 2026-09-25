#pragma once

#include "PlatformServicesMockBackend.h"

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <type_traits>
#include <variant>
#include <vector>

namespace Horo::PlatformServices::TestSupport {
    namespace detail {
        inline constexpr std::uint64_t MockProviderIdentity = 0x4d4f434bULL;
        inline constexpr std::uint64_t MaximumDelayMilliseconds = 24ULL * 60ULL * 60ULL * 1000ULL;
        inline constexpr std::size_t MaximumErrorTextBytes = 512;
        inline constexpr std::size_t MaximumErrorIdentityBytes = 128;
        // Mirrors the mock request store's retained terminal capacity.
        inline constexpr std::size_t MaximumRetainedTerminalRequests = 256;

        inline const ErrorCodeDescriptor ScriptFailure{.domain = ErrorDomainId{"horo.platform.mock"},
                                                       .code = ErrorCode{"platform.mock.script_failure"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "The deterministic platform mock rejected a call or script.",
                                                       .remediationHint = "Correct the bounded mock script and expected call sequence.",
                                                       .retryable = false,
                                                       .userActionable = false};

        [[nodiscard]] bool IsKnownOperation(MockPlatformServicesOperation operation) noexcept;
        [[nodiscard]] bool IsServiceOperation(MockPlatformServicesOperation operation) noexcept;
        [[nodiscard]] std::size_t OperationIndex(MockPlatformServicesOperation operation) noexcept;
        [[nodiscard]] bool IsBoundedDelay(std::chrono::milliseconds duration) noexcept;
        [[nodiscard]] std::uint64_t ToMilliseconds(std::chrono::milliseconds duration) noexcept;
        [[nodiscard]] std::uint8_t EventPriority(MockCompletionKind kind) noexcept;

        template <typename T> [[nodiscard]] Result<T> Failure() {
            return Result<T>::Failure(MakeError(ScriptFailure));
        }
    }  // namespace detail

    struct MockPlatformServicesBackend::Impl final {
        struct ScriptedResponse final {
            std::optional<Error> error;
            std::variant<std::monostate, CloudReadResult, FriendsPage, PlatformSessionSnapshot, LeaderboardEntriesPage,
                         LeaderboardAroundSubjectResult>
                payload;
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

        struct TerminalRequestIdentity final {
            PlatformRequestId id;
            PlatformRequestGeneration generation;
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

        struct ScheduledEventCompare final {
            [[nodiscard]] bool operator()(const ScheduledEvent &left, const ScheduledEvent &right) const noexcept {
                if (left.dueMilliseconds != right.dueMilliseconds)
                    return left.dueMilliseconds > right.dueMilliseconds;
                if (left.kind != right.kind)
                    return detail::EventPriority(left.kind) > detail::EventPriority(right.kind);
                return left.sequence > right.sequence;
            }
        };

        explicit Impl(PlatformProviderGeneration generation);

        [[nodiscard]] Result<void> RecordCall(MockPlatformServicesOperation operation, PlatformRequestId request = {});
        [[nodiscard]] Result<ScriptedResponse> TakeResponse(MockPlatformServicesOperation operation);
        [[nodiscard]] static bool ValidateFailureResponse(const MockPlatformServicesResponse &response) noexcept;
        [[nodiscard]] static bool ValidateResponsePayload(MockPlatformServicesOperation operation,
                                                          const MockPlatformServicesResponse &response) noexcept;
        [[nodiscard]] bool ValidateResponse(MockPlatformServicesOperation operation,
                                            const MockPlatformServicesResponse &response) const noexcept;
        void AddDiagnostic(MockPlatformServicesDiagnostic diagnostic) noexcept;

        void ScheduleEvent(PlatformRequestId request, PlatformRequestGeneration generation, MockPlatformServicesOperation operation,
                           MockCompletionKind kind, std::uint64_t delayMilliseconds,
                           std::function<Result<PlatformRequestMutation>()> complete);
        [[nodiscard]] RequestRecord *FindRequest(PlatformRequestId id, PlatformRequestGeneration generation) noexcept;
        void RetireRequestIfIdle(PlatformRequestId id, PlatformRequestGeneration generation);

        template <typename T>
        [[nodiscard]] std::function<Result<PlatformRequestMutation>()> ProviderCompletion(PlatformRequestId id,
                                                                                          PlatformRequestGeneration generation,
                                                                                          std::shared_ptr<const ScriptedResponse> response);
        template <typename T>
        [[nodiscard]] std::function<Result<PlatformRequestMutation>()> TimeoutCompletion(PlatformRequestId id,
                                                                                         PlatformRequestGeneration generation);
        template <typename T>
        [[nodiscard]] std::function<Result<PlatformRequestMutation>()> CancellationCompletion(PlatformRequestId id,
                                                                                              PlatformRequestGeneration generation);
        [[nodiscard]] bool HasSubmissionCapacity(const ScriptedResponse &response) const noexcept;
        template <typename T> [[nodiscard]] Result<PlatformRequestHandle<T>> AdmitAndStart(MockPlatformServicesOperation operation);
        template <typename T>
        void RegisterRequest(MockPlatformServicesOperation operation, std::shared_ptr<const ScriptedResponse> response,
                             const PlatformRequestHandle<T> &handle);
        template <typename T>
        [[nodiscard]] Result<PlatformRequestHandle<T>> Submit(MockPlatformServicesOperation operation, bool requestIsValid = true);

        [[nodiscard]] Result<void> RequestCancel(PlatformRequestId id, PlatformRequestGeneration generation);
        [[nodiscard]] Result<void> VerifyExpectations();
        [[nodiscard]] bool AllExpectationsMet() const noexcept;
        [[nodiscard]] Result<void> AdvanceClock(std::chrono::milliseconds elapsed);
        [[nodiscard]] std::size_t DispatchDueCompletions(std::size_t maxCount);
        [[nodiscard]] Result<PlatformServiceCapabilitySnapshot> InspectCapabilities() const;
        [[nodiscard]] Result<void> Activate(const PlatformServicesBackendConfig &config);
        [[nodiscard]] Result<void> Shutdown();

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
        std::deque<TerminalRequestIdentity> retainedTerminalRequests;
        std::priority_queue<ScheduledEvent, std::vector<ScheduledEvent>, ScheduledEventCompare> events;
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
}  // namespace Horo::PlatformServices::TestSupport
