#pragma once

/**
 * @file ReleasePipelineExecutor.h
 * @brief Shared typed release stage handoffs and single-target executor.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Release/ReleaseJobTracker.h"
#include "Horo/Release/ReleasePreflight.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>

namespace Horo::Release {
    /** @brief Identity of one successful target configuration. */
    struct ReleaseConfiguredTarget final {
        std::filesystem::path root;
        Sha256Digest configurationDigest;
    };

    /** @brief Exact compiled payload for one target. */
    struct ReleaseBuiltPayload final {
        std::filesystem::path root;
        Sha256Digest bytesDigest;
    };

    /** @brief Exact cooked asset payload for one target. */
    struct ReleaseCookedPayload final {
        std::filesystem::path root;
        Sha256Digest bytesDigest;
    };

    /** @brief Unsigned assembled payload whose bytes require pre-sign verification. */
    struct ReleaseStagedPayload final {
        std::filesystem::path root;
        Sha256Digest bytesDigest;
    };

    /** @brief Staged payload admitted to signing or unsigned finalization. */
    struct ReleasePreSignVerifiedPayload final {
        ReleaseStagedPayload staged;
    };

    /** @brief Payload identity after all byte-changing signing work completes. */
    struct ReleaseSignedPayload final {
        std::filesystem::path root;
        Sha256Digest bytesDigest;
    };

    /** @brief Signed or explicitly unsigned bytes selected for final metadata. */
    using ReleaseFinalBytes = std::variant<ReleasePreSignVerifiedPayload, ReleaseSignedPayload>;

    /** @brief Immutable candidate identity and canonical manifest hash. */
    struct ReleaseFinalMetadata final {
        ReleaseCandidateId candidate;
        Sha256Digest manifestDigest;
    };

    /** @brief Exact payload and metadata retained even if final verification fails. */
    struct ReleaseFinalizedCandidate final {
        ReleaseFinalBytes bytes;
        ReleaseFinalMetadata metadata;
    };

    /** @brief Only candidate type accepted by a publication worker. */
    struct ReleaseFinalVerifiedCandidate final {
        ReleaseFinalizedCandidate finalized;
    };

    /** @brief Bounded execution policy supplied by the service, never by a stage worker. */
    struct ReleasePipelineLimits final {
        std::chrono::steady_clock::duration stageTimeout{std::chrono::minutes{30}};
    };

    /** @brief Optional synchronous observer of committed job and stage boundaries. */
    class IReleaseJobObserver {
    public:
        virtual ~IReleaseJobObserver() = default;
        /** @brief Observes an immutable tracker copy after a transition; implementations must not throw. */
        virtual void OnSnapshot(const ReleaseJobSnapshot &snapshot) noexcept = 0;
    };

    /** @brief Service-owned execution limits and optional synchronous history projection. */
    struct ReleasePipelineExecutionOptions final {
        ReleasePipelineLimits limits;
        IReleaseJobObserver *observer{}; /**< Borrowed only for the synchronous Execute call. */
    };

    struct ReleaseStageContext;

    /** @brief Narrow synchronous reporting authority for one active stage. */
    class IReleaseStageReporter {
    public:
        virtual ~IReleaseStageReporter() = default;
        /** @brief Reports monotonic typed units. @param context Active attempt. @param completed Completed units.
         * @param total Positive total units. @return Success or invalid transition. */
        [[nodiscard]] virtual Result<void> ReportProgress(const ReleaseStageContext &context, std::uint64_t completed,
                                                          std::uint64_t total) = 0;
        /** @brief Retains one bounded diagnostic. @param context Active attempt. @param code Stable code.
         * @param severity Severity. @param message Bounded safe text. @return Identity or failure. */
        [[nodiscard]] virtual Result<ReleaseDiagnosticId> ReportDiagnostic(const ReleaseStageContext &context, ErrorCode code,
                                                                           ErrorSeverity severity, std::string message) = 0;
    };

    /** @brief Identity, cancellation and deadline of one stage attempt. */
    struct ReleaseStageContext final {
        ReleaseJobId job;
        ReleaseTargetId target;
        OperationId operation{};
        ReleaseStage stage{ReleaseStage::Validating};
        ReleaseStageAttemptId attempt;
        CancellationToken cancellation;
        std::chrono::steady_clock::time_point deadline;
        IReleaseStageReporter *reporter{}; /**< Borrowed for this synchronous worker call only. */

        /** @brief Publishes bounded stage progress. @param completed Completed units. @param total Positive total units.
         * @return Success or invalid transition. */
        [[nodiscard]] Result<void> ReportProgress(std::uint64_t completed, std::uint64_t total) const;
        /** @brief Publishes one bounded stage diagnostic. @param code Stable code. @param severity Severity.
         * @param message Bounded safe text. @return Assigned identity or invalid transition. */
        [[nodiscard]] Result<ReleaseDiagnosticId> ReportDiagnostic(ErrorCode code, ErrorSeverity severity, std::string message) const;
    };

    /** @brief Host-owned source of fresh read-only facts for every planned stage. */
    class IReleasePreflightFactsProvider {
    public:
        virtual ~IReleasePreflightFactsProvider() = default;
        /** @brief Captures current facts without changing release inputs. @param plan Frozen plan. @return Fresh facts or failure. */
        [[nodiscard]] virtual Result<ReleasePreflightFacts> Capture(const ReleaseExecutionPlan &plan) = 0;
    };

    /** @brief Host-owned workers; each method owns cleanup of its temporary resources on failure. */
    class IReleasePipelineStages {
    public:
        virtual ~IReleasePipelineStages() = default;
        /** @brief Validates target admission. @param context Active attempt. @return Success or typed failure. */
        [[nodiscard]] virtual Result<void> Validate(const ReleaseStageContext &context) = 0;
        /** @brief Configures one target. @param context Active attempt. @return Configuration evidence or failure. */
        [[nodiscard]] virtual Result<ReleaseConfiguredTarget> Configure(const ReleaseStageContext &context) = 0;
        /** @brief Builds configured binaries. @param context Active attempt. @param configured Exact configuration.
         * @return Compiled payload or failure. */
        [[nodiscard]] virtual Result<ReleaseBuiltPayload> Build(const ReleaseStageContext &context,
                                                                const ReleaseConfiguredTarget &configured) = 0;
        /** @brief Cooks target assets. @param context Active attempt. @param configured Exact configuration.
         * @param built Exact compiled payload. @return Cooked payload or failure. */
        [[nodiscard]] virtual Result<ReleaseCookedPayload> Cook(const ReleaseStageContext &context,
                                                                const ReleaseConfiguredTarget &configured,
                                                                const ReleaseBuiltPayload &built) = 0;
        /** @brief Packages binaries and assets. @param context Active attempt. @param built Compiled payload.
         * @param cooked Cooked payload. @return Staged bytes or failure. */
        [[nodiscard]] virtual Result<ReleaseStagedPayload> Package(const ReleaseStageContext &context, const ReleaseBuiltPayload &built,
                                                                   const ReleaseCookedPayload &cooked) = 0;
        /** @brief Verifies unsigned staged bytes. @param context Active attempt. @param staged Exact staged bytes.
         * @return Success or typed failure. */
        [[nodiscard]] virtual Result<void> PreSignVerify(const ReleaseStageContext &context, const ReleaseStagedPayload &staged) = 0;
        /** @brief Completes byte-changing signing. @param context Active attempt. @param verified Pre-sign-verified bytes.
         * @return Final signed bytes or failure. */
        [[nodiscard]] virtual Result<ReleaseSignedPayload> Sign(const ReleaseStageContext &context,
                                                                const ReleasePreSignVerifiedPayload &verified) = 0;
        /** @brief Freezes metadata after final bytes are known. @param context Active attempt.
         * @param candidate Service-assigned immutable candidate identity. @param bytes Exact final bytes.
         * @return Canonical manifest digest or failure. */
        [[nodiscard]] virtual Result<Sha256Digest> FinalizeMetadata(const ReleaseStageContext &context, ReleaseCandidateId candidate,
                                                                    const ReleaseFinalBytes &bytes) = 0;
        /** @brief Verifies final bytes against metadata. @param context Active attempt. @param candidate Finalized candidate.
         * @return Success or typed failure. */
        [[nodiscard]] virtual Result<void> FinalVerify(const ReleaseStageContext &context, const ReleaseFinalizedCandidate &candidate) = 0;
        /** @brief Publishes only verified bytes. @param context Active attempt. @param candidate Verified candidate.
         * @return Success or typed failure. */
        [[nodiscard]] virtual Result<void> Publish(const ReleaseStageContext &context, const ReleaseFinalVerifiedCandidate &candidate) = 0;
    };

    /** @brief Executes one frozen target through the same typed stage sequence for all adapters. */
    class ReleasePipelineExecutor final {
    public:
        /**
         * @brief Runs every planned stage with fresh input checks and one terminal result.
         * @param tracker Service-owned job authority in Queued state.
         * @param candidate Service-assigned candidate identity reserved for this job.
         * @param plan Immutable preflight result for the same target.
         * @param facts Host-owned source of fresh read-only observations.
         * @param stages Host-owned typed stage workers.
         * @param cancellation Service-owned cooperative cancellation token.
         * @param options Stage limits and optional host-owned durable projection.
         * @return Owned final job snapshot; the service retains the tracker.
         */
        [[nodiscard]] ReleaseJobSnapshot Execute(ReleaseJobTracker &tracker, ReleaseCandidateId candidate, const ReleaseExecutionPlan &plan,
                                                 IReleasePreflightFactsProvider &facts, IReleasePipelineStages &stages,
                                                 const CancellationToken &cancellation, ReleasePipelineExecutionOptions options = {}) const;
    };
}  // namespace Horo::Release
