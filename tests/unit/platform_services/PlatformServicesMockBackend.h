#pragma once

#include "Horo/PlatformServices/PlatformServicesBackend.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Horo::PlatformServices::TestSupport {
    /** @brief Typed provider entry points observed by the deterministic test backend. */
    enum class MockPlatformServicesOperation : std::uint8_t {
        UnlockAchievement,
        SubmitScore,
        WriteStat,
        ReadCloudObject,
        WriteCloudObject,
        SetPresence,
        ClearPresence,
        QueryFriends,
        QueryCurrentSession,
        RequestCancel,
        Count
    };

    /** @brief Origin of one deterministic completion event. */
    enum class MockCompletionKind : std::uint8_t {
        Provider,
        Cancellation,
        Timeout
    };

    /** @brief Machine-readable test diagnostic; it contains no provider or user text. */
    enum class MockDiagnosticKind : std::uint8_t {
        UnexpectedCall,
        MissingExpectedCall,
        MissingResponse,
        UnusedResponse,
        InvalidScript,
        InvalidRequest,
        CapacityExceeded,
        UnexpectedCancellation,
        DuplicateCompletionIgnored,
        LateCompletionIgnored,
        CompletionRejected,
        DiagnosticOverflow
    };

    /** @brief Bounded typed failure script normalized when it is installed. */
    struct MockPlatformServicesFailure final {
        const ErrorCodeDescriptor *descriptor{};
        std::string message;
    };

    /** @brief Scripted result and logical timing policy for one operation. */
    struct MockPlatformServicesResponse final {
        std::optional<MockPlatformServicesFailure> failure;
        std::variant<std::monostate, CloudReadResult, FriendsPage, PlatformSessionSnapshot> payload;
        std::chrono::milliseconds delay{};
        std::optional<std::chrono::milliseconds> timeoutAfter;
        std::optional<std::chrono::milliseconds> duplicateDelay;
        std::chrono::milliseconds cancellationDelay{};
        bool acknowledgeCancellation{true};
    };

    /** @brief Bounded diagnostic for an observed call, script, or completion outcome. */
    struct MockPlatformServicesDiagnostic final {
        MockDiagnosticKind kind{MockDiagnosticKind::InvalidScript};
        MockPlatformServicesOperation expected{MockPlatformServicesOperation::Count};
        MockPlatformServicesOperation actual{MockPlatformServicesOperation::Count};
        std::size_t expectationIndex{};
        PlatformRequestId request;
        std::uint64_t logicalTimeMilliseconds{};
    };

    /** @brief Deterministically ordered record of a dispatched mock completion event. */
    struct MockPlatformServicesCompletion final {
        MockPlatformServicesOperation operation{MockPlatformServicesOperation::Count};
        MockCompletionKind kind{MockCompletionKind::Provider};
        PlatformRequestMutation mutation{PlatformRequestMutation::Unchanged};
        PlatformRequestId request;
        std::uint64_t logicalTimeMilliseconds{};
    };

    /**
     * @brief Scripted, bounded Platform Services backend for contract tests without an SDK or network.
     * @details Tests advance a logical millisecond clock and explicitly dispatch due completions. Same-time events use stable
     *          provider, cancellation, timeout priority followed by insertion order. Completion and diagnostic histories have
     *          finite capacities; terminal request state remains owned by the real PlatformRequestStore.
     */
    class MockPlatformServicesBackend final : public IPlatformServicesBackend {
    public:
        static constexpr std::size_t MaximumExpectedCalls = 512;
        static constexpr std::size_t MaximumScriptedResponses = 256;
        static constexpr std::size_t MaximumDiagnostics = 128;
        static constexpr std::size_t MaximumScheduledEvents = 2048;
        static constexpr std::uint64_t MaximumPayloadBytes = 64U * 1024U;
        static constexpr std::uint32_t MaximumPageEntries = 128;

        /** @brief Creates a deterministic backend with one provider capability generation. */
        explicit MockPlatformServicesBackend(PlatformProviderGeneration generation = {1});
        ~MockPlatformServicesBackend() override;
        MockPlatformServicesBackend(const MockPlatformServicesBackend &) = delete;
        MockPlatformServicesBackend &operator=(const MockPlatformServicesBackend &) = delete;

        /**
         * @brief Installs the exact provider-call sequence expected by a contract test.
         * @param operations Ordered service and cancellation entry points.
         * @return Success or typed invalid-script/capacity failure.
         */
        [[nodiscard]] Result<void> ExpectSequence(std::vector<MockPlatformServicesOperation> operations);

        /**
         * @brief Appends one bounded response to the selected operation's response queue.
         * @param operation Typed provider entry point; cancellation has no response script.
         * @param response Owned payload/error plus logical delay, timeout, duplicate, and cancellation policy.
         * @return Success or typed invalid-script/capacity failure.
         */
        [[nodiscard]] Result<void> SetResponse(MockPlatformServicesOperation operation, MockPlatformServicesResponse response);

        /** @brief Verifies missing calls and unused scripts, recording each through typed diagnostics. */
        [[nodiscard]] Result<void> VerifyExpectations();
        /** @brief Reports whether the configured sequence and response queues were consumed exactly. */
        [[nodiscard]] bool AllExpectationsMet() const noexcept;
        /** @brief Returns the bounded call and completion diagnostics captured so far. */
        [[nodiscard]] std::span<const MockPlatformServicesDiagnostic> Diagnostics() const noexcept;
        /** @brief Returns the number of diagnostics omitted after the fixed diagnostic buffer filled. */
        [[nodiscard]] std::uint64_t DiagnosticOverflowCount() const noexcept;
        /** @brief Returns deterministic completion-dispatch order. */
        [[nodiscard]] std::span<const MockPlatformServicesCompletion> CompletionOrder() const noexcept;
        /** @brief Returns observed backend entry points in call order. */
        [[nodiscard]] std::span<const MockPlatformServicesOperation> Calls() const noexcept;
        /** @brief Returns the test-owned request store for immutable result inspection. */
        [[nodiscard]] PlatformRequestStore &Requests() noexcept;
        /** @brief Returns the current logical time without consulting a wall or monotonic system clock. */
        [[nodiscard]] std::uint64_t CurrentTimeMilliseconds() const noexcept;

        /** @brief Advances logical time without sleeping or automatically dispatching completions. */
        [[nodiscard]] Result<void> AdvanceClock(std::chrono::milliseconds elapsed);
        /** @brief Dispatches up to maxCount due completion events in stable deterministic order. */
        [[nodiscard]] std::size_t DispatchDueCompletions(std::size_t maxCount = MaximumScheduledEvents);

        [[nodiscard]] Result<PlatformServiceCapabilitySnapshot> InspectCapabilities() const override;
        [[nodiscard]] Result<void> Activate(const PlatformServicesBackendConfig &config) override;
        [[nodiscard]] Result<void> RequestCancel(PlatformRequestId request, PlatformRequestGeneration generation) override;
        [[nodiscard]] Result<void> Shutdown() override;

        [[nodiscard]] Result<PlatformRequestHandle<void>> UnlockAchievement(AchievementUnlockRequest request) override;
        [[nodiscard]] Result<PlatformRequestHandle<void>> SubmitScore(LeaderboardScoreRequest request) override;
        [[nodiscard]] Result<PlatformRequestHandle<void>> WriteStat(StatWriteRequest request) override;
        [[nodiscard]] Result<PlatformRequestHandle<CloudReadResult>> ReadCloudObject(CloudReadRequest request) override;
        [[nodiscard]] Result<PlatformRequestHandle<void>> WriteCloudObject(CloudWriteRequest request) override;
        [[nodiscard]] Result<PlatformRequestHandle<void>> SetPresence(PresenceUpdateRequest request) override;
        [[nodiscard]] Result<PlatformRequestHandle<void>> ClearPresence(PlatformSubjectHandle subject) override;
        [[nodiscard]] Result<PlatformRequestHandle<FriendsPage>> QueryFriends(FriendsQuery query) override;
        [[nodiscard]] Result<PlatformRequestHandle<PlatformSessionSnapshot>> QueryCurrentSession() override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::PlatformServices::TestSupport
