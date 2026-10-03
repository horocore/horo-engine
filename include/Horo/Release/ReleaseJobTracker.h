#pragma once

/**
 * @file ReleaseJobTracker.h
 * @brief Revisioned single-target release job and ordered stage authority.
 */

#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Horo::Release {
    /** @brief Service-owned identity of one release job. */
    struct ReleaseJobId final {
        std::uint64_t value{};
        bool operator==(const ReleaseJobId &) const noexcept = default;
    };

    /** @brief Service-owned identity of one immutable target. */
    struct ReleaseTargetId final {
        std::uint64_t value{};
        bool operator==(const ReleaseTargetId &) const noexcept = default;
    };

    /** @brief Service-owned identity of one immutable finalized candidate. */
    struct ReleaseCandidateId final {
        std::uint64_t value{};
        bool operator==(const ReleaseCandidateId &) const noexcept = default;
    };

    /** @brief Monotonic attempt identity within one job. */
    struct ReleaseStageAttemptId final {
        std::uint64_t value{};
        bool operator==(const ReleaseStageAttemptId &) const noexcept = default;
    };

    /** @brief Monotonic identity of one bounded release diagnostic. */
    struct ReleaseDiagnosticId final {
        std::uint64_t value{};
        bool operator==(const ReleaseDiagnosticId &) const noexcept = default;
    };

    /** @brief Ordered release work, including explicit optional stages. */
    enum class ReleaseStage : std::uint8_t {
        Validating,
        Configuring,
        Building,
        Cooking,
        Packaging,
        PreSignVerifying,
        Signing,
        FinalizingMetadata,
        FinalVerifying,
        Publishing
    };

    inline constexpr std::size_t ReleaseStageCount = 10;
    inline constexpr std::size_t MaximumReleaseDiagnostics = 64;
    inline constexpr std::size_t MaximumReleaseDiagnosticMessageBytes = 2048;

    /** @brief Whether optional signing and publication belong to the frozen plan. */
    struct ReleaseStagePlan final {
        bool signing{};
        bool publishing{};

        /** @brief Reports whether one stage is planned. @param stage Stage to inspect. @return True for required or selected work. */
        [[nodiscard]] bool Includes(ReleaseStage stage) const noexcept;
    };

    /** @brief Closed state of one stage attempt. */
    enum class ReleaseStageState : std::uint8_t {
        NotStarted,
        Running,
        Succeeded,
        Failed,
        Cancelled,
        NotApplicable
    };

    /** @brief Closed state of one service-owned job. */
    enum class ReleaseJobState : std::uint8_t {
        Queued,
        Running,
        Cancelling,
        Succeeded,
        Failed,
        Cancelled
    };

    /** @brief Candidate eligibility independent of job outcome. */
    enum class ReleaseCandidateState : std::uint8_t {
        Finalized,
        FinalVerified
    };

    /** @brief One attempt's observable state. */
    struct ReleaseStageSnapshot final {
        ReleaseStageState state{ReleaseStageState::NotStarted};
        std::optional<ReleaseStageAttemptId> attempt;
    };

    /** @brief Candidate evidence retained across later failure or cancellation. */
    struct ReleaseCandidateSnapshot final {
        ReleaseCandidateId id;
        ReleaseCandidateState state{ReleaseCandidateState::Finalized};
    };

    /** @brief Monotonic typed progress for one stage attempt. */
    struct ReleaseStageProgress final {
        ReleaseStage stage{ReleaseStage::Validating};
        ReleaseStageAttemptId attempt;
        std::uint64_t completed{};
        std::uint64_t total{};
    };

    /** @brief Bounded diagnostic record owned by the release job, not an observer. */
    struct ReleaseDiagnostic final {
        ReleaseDiagnosticId id;
        ReleaseJobId job;
        ReleaseTargetId target;
        OperationId operation{};
        ReleaseStage stage{ReleaseStage::Validating};
        ReleaseStageAttemptId attempt;
        ErrorCode code;
        ErrorSeverity severity{ErrorSeverity::Error};
        std::string message;
    };

    /** @brief Successful terminal with final-verified candidate identity. */
    struct ReleaseSucceeded final {
        ReleaseTargetId target;
        ReleaseCandidateId candidate;
    };

    /** @brief Failed terminal retaining the exact boundary and cause. */
    struct ReleaseFailed final {
        ReleaseTargetId target;
        std::optional<ReleaseStage> stage;
        std::optional<ReleaseStageAttemptId> attempt;
        Error cause;
    };

    /** @brief Cancelled terminal retaining its last active stage, if any. */
    struct ReleaseCancelled final {
        ReleaseTargetId target;
        std::optional<ReleaseStage> stage;
        std::optional<ReleaseStageAttemptId> attempt;
    };

    using ReleaseTerminalResult = std::variant<ReleaseSucceeded, ReleaseFailed, ReleaseCancelled>;

    /** @brief One internally consistent, immutable observation copy. */
    struct ReleaseJobSnapshot final {
        ReleaseJobId id;
        ReleaseTargetId target;
        OperationId operation{};
        std::uint64_t revision{};
        ReleaseJobState state{ReleaseJobState::Queued};
        std::array<ReleaseStageSnapshot, ReleaseStageCount> stages;
        std::optional<ReleaseStage> activeStage;
        std::optional<ReleaseCandidateSnapshot> candidate;
        std::optional<ReleaseStageProgress> progress;
        std::vector<ReleaseDiagnosticId> recentDiagnostics;
        std::optional<ReleaseTerminalResult> terminal;
    };

    /** @brief Serializes all job, stage, cancellation and candidate transitions. */
    class ReleaseJobTracker final {
    public:
        /** @brief Creates a queued job with immutable identities and optional-stage policy. */
        ReleaseJobTracker(ReleaseJobId id, ReleaseTargetId target, OperationId operation, ReleaseStagePlan plan);

        /** @brief Copies one consistent job observation. @return Revisioned snapshot. */
        [[nodiscard]] ReleaseJobSnapshot Snapshot() const;

        /** @brief Commits worker admission. @return Success or an illegal-transition error. */
        [[nodiscard]] Result<void> Start();
        /** @brief Begins only the next eligible stage. @param stage Required next stage. @return Unique attempt identity or failure. */
        [[nodiscard]] Result<ReleaseStageAttemptId> BeginStage(ReleaseStage stage);
        /** @brief Completes a normal stage after its worker succeeds. @param stage Active stage. @param attempt Active attempt. */
        [[nodiscard]] Result<void> CompleteStage(ReleaseStage stage, ReleaseStageAttemptId attempt);
        /** @brief Freezes a candidate only after final metadata is produced. @param attempt Active finalization attempt. @param candidate
         * New candidate identity. */
        [[nodiscard]] Result<void> CompleteFinalization(ReleaseStageAttemptId attempt, ReleaseCandidateId candidate);
        /** @brief Marks the exact finalized candidate eligible after final verification. @param attempt Active verification attempt. */
        [[nodiscard]] Result<void> CompleteFinalVerification(ReleaseStageAttemptId attempt);
        /** @brief Fails the active stage and commits one terminal. @param stage Active stage. @param attempt Active attempt. @param cause
         * Stable failure. */
        [[nodiscard]] Result<void> FailStage(ReleaseStage stage, ReleaseStageAttemptId attempt, Error cause);
        /** @brief Requests cancellation; a queued job terminalizes immediately. @return Success or terminal-state error. */
        [[nodiscard]] Result<void> RequestCancel();
        /** @brief Commits cancellation after the worker has released its resources. @return Success or illegal-transition error. */
        [[nodiscard]] Result<void> AcknowledgeCancellation();
        /** @brief Fails a queued job whose worker admission failed. @param cause Stable admission failure. */
        [[nodiscard]] Result<void> FailAdmission(Error cause);
        /** @brief Fails a running job before another stage begins. @param cause Stable pipeline failure. */
        [[nodiscard]] Result<void> FailJob(Error cause);
        /** @brief Updates monotonic progress for the active attempt. @param stage Active stage. @param attempt Active attempt.
         * @param completed Completed units. @param total Positive total units. @return Success or invalid transition. */
        [[nodiscard]] Result<void> UpdateProgress(ReleaseStage stage, ReleaseStageAttemptId attempt, std::uint64_t completed,
                                                  std::uint64_t total);
        /** @brief Retains one bounded diagnostic for the active attempt. @param stage Active stage. @param attempt Active attempt.
         * @param code Stable diagnostic code. @param severity Severity. @param message Bounded safe text.
         * @return Assigned identity or invalid diagnostic failure. */
        [[nodiscard]] Result<ReleaseDiagnosticId> AppendDiagnostic(ReleaseStage stage, ReleaseStageAttemptId attempt, ErrorCode code,
                                                                   ErrorSeverity severity, std::string message);
        /** @brief Copies a retained diagnostic. @param id Diagnostic identity. @return Record or none when expired or unknown. */
        [[nodiscard]] std::optional<ReleaseDiagnostic> Diagnostic(ReleaseDiagnosticId id) const;
        /** @brief Commits success only after every planned stage and the candidate pass. */
        [[nodiscard]] Result<void> FinishSuccess();

    private:
        /** @brief Completes an already validated active attempt under the tracker lock. */
        void CompleteActiveStage(ReleaseStage stage);
        /** @brief Checks active state and identity under the tracker lock. */
        [[nodiscard]] bool IsActiveAttempt(ReleaseStage stage, ReleaseStageAttemptId attempt) const noexcept;

        const ReleaseStagePlan plan_;
        mutable std::mutex mutex_;
        ReleaseJobSnapshot snapshot_;
        std::uint64_t nextAttempt_{1};
        std::uint64_t nextDiagnostic_{1};
        std::deque<ReleaseDiagnostic> diagnostics_;
    };
}  // namespace Horo::Release
