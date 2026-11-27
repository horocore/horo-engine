#pragma once

#include "Horo/Editor/ProjectCreationController.h"
#include "Horo/Foundation/Result.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Horo {
    class EngineDataBus;
    class DurableFileSystem;
    class JobSystem;
}  // namespace Horo

namespace Horo::Security {
    class SecureRandomSource;
}

namespace Horo::Editor {

    struct ProjectCreationServiceState;

    /** @file ProjectCreationService.h
     * @brief Job-backed, durable project-creation operation contract.
     */

    using ProjectCreationOperationId = std::uint64_t;
    using ProjectCreationRevision = std::uint64_t;

    /** @brief User-facing lifecycle state of one project creation operation. */
    enum class ProjectCreationOperationState : std::uint8_t {
        Queued,
        Running,
        Cancelling,
        Succeeded,
        Failed,
        Cancelled,
    };

    /** @brief Named creation phase reported by the authoritative operation snapshot. */
    enum class ProjectCreationOperationPhase : std::uint8_t {
        Validating,
        Staging,
        WritingMetadata,
        WritingScaffolding,
        Promoting,
        Completed,
        Cancelled,
        Failed,
    };

    /** @brief Stable project-creation failure classification. */
    enum class ProjectCreationErrorCode : std::uint8_t {
        InvalidRequest,
        DestinationOccupied,
        ParentUnavailable,
        StagingFailed,
        WriteFailed,
        PromotionFailed,
        Cancelled,
        JobSubmissionFailed,
        IdentityPreparationFailed,
        DurabilityUnconfirmed,
        UnexpectedFailure,
    };

    /** @brief Typed terminal diagnostic retained in a project creation snapshot. */
    struct ProjectCreationError {
        ProjectCreationErrorCode code = ProjectCreationErrorCode::InvalidRequest;
        std::string message;
        std::optional<Error> cause; /**< Original typed host failure, or a preallocated unexpected-boundary diagnostic. */
    };

    /** @brief Distinguishes unpublished failure from irreversible publication with uncertain durability. */
    enum class ProjectCreationPublicationState : std::uint8_t {
        Unpublished,
        Durable,
        ReconciliationRequired
    };

    /** @brief Immutable view of one project creation operation held by ProjectCreationService. */
    struct ProjectCreationSnapshot {
        ProjectCreationOperationId id = 0;
        ProjectCreationOperationState state = ProjectCreationOperationState::Queued;
        ProjectCreationOperationPhase phase = ProjectCreationOperationPhase::Validating;
        float progress = 0.0F;
        std::filesystem::path projectRoot;
        std::string projectId;
        std::optional<ProjectCreationError> error;
        ProjectCreationPublicationState publication{
            ProjectCreationPublicationState::Unpublished}; /**< Authoritative at terminal state; a running snapshot is not a commit witness.
                                                            */
    };

    /** @brief Handle returned after a project-creation operation was accepted by JobSystem. */
    struct ProjectCreationOperationHandle {
        ProjectCreationOperationId id = 0;
    };

    /** @brief Lightweight notification emitted only after a project directory is promoted.
     * @details Snapshot polling remains authoritative when notification allocation fails after publication.
     */
    struct ProjectCreatedEvent {
        static constexpr std::string_view HoroEventTypeName = "horo::editor::ProjectCreatedEvent";
        ProjectCreationOperationId operationId = 0;
        std::string projectId;
        ProjectCreationRevision revision = 0;
    };

    /** @brief Lightweight notification emitted when operation phase or progress updates during asynchronous execution. */
    struct ProjectCreationProgressEvent {
        static constexpr std::string_view HoroEventTypeName = "horo::editor::ProjectCreationProgressEvent";
        ProjectCreationOperationId operationId = 0;
        ProjectCreationOperationPhase phase = ProjectCreationOperationPhase::Validating;
        float progress = 0.0F;
        ProjectCreationRevision revision = 0;
    };

    /** @brief Lightweight invalidation notification for committed project creation state. */
    struct ProjectCreationRevisionChangedEvent {
        static constexpr std::string_view HoroEventTypeName = "horo::editor::ProjectCreationRevisionChangedEvent";
        ProjectCreationRevision revision = 0;
    };

    /** @brief Creates a project through a staged, cancellable JobSystem operation. */
    class ProjectCreationService {
    public:
        /**
         * @brief Constructs the service with process-owned execution and notification dependencies.
         * @param jobs Process job system, which must outlive this service and its accepted work.
         * @param dataBus Process data bus, which must outlive this service and its accepted work.
         * @param random Host-selected OS entropy retained through accepted jobs. Empty fails identity preparation without a fallback.
         * @param files Host-owned durable writer retained through jobs; empty fails before staging, with no native fallback.
         * @details Fill must be synchronous, bounded and nonreentrant. Background jobs serialize it under the existing operation mutex;
         * that mutex also protects snapshots. Captured shared state retains the provider until all accepted jobs release it.
         * The filesystem must support concurrent calls from accepted jobs. The host protects destination parent namespaces
         * against replacement during each operation. Success confirms requested OS flushes, not hardware power-loss behavior.
         */
        ProjectCreationService(JobSystem &jobs, EngineDataBus &dataBus, std::shared_ptr<Security::SecureRandomSource> random = {},
                               std::shared_ptr<DurableFileSystem> files = {});

        ProjectCreationService(const ProjectCreationService &) = delete;
        ProjectCreationService &operator=(const ProjectCreationService &) = delete;

        /**
         * @brief Validates and submits a project creation operation without writing from the caller thread.
         * @param request Typed creation values, including the final project root.
         * @return Accepted operation handle or a typed foundation error when validation/submission is rejected.
         * @throws std::bad_alloc Before acceptance, while retaining request/operation or scheduler submission storage.
         * @pre The caller retains the service and injected process services for the operation lifetime.
         */
        [[nodiscard]] Result<ProjectCreationOperationHandle> StartCreate(ProjectCreationRequest request) const;

        /**
         * @brief Returns the authoritative snapshot for an accepted operation, if retained.
         * @param id Accepted project creation operation identifier.
         * @return The latest immutable snapshot, or empty when the identifier is unknown.
         * @details Reconciles retained scheduler cancellation when queued work was revoked without entering its callback,
         * including Shutdown(Cancel). Missing/evicted job records are not invented cancellation evidence. Existing terminal
         * publication state is preserved. Resulting revision notifications remain queued until PumpMainThread.
         */
        [[nodiscard]] std::optional<ProjectCreationSnapshot> Query(ProjectCreationOperationId id) const;

        /**
         * @brief Requests cooperative cancellation for a non-terminal operation.
         * @param id Accepted project creation operation identifier.
         * @return Success after the cancellation request was forwarded, or a typed error when unknown.
         */
        [[nodiscard]] Result<void> RequestCancel(ProjectCreationOperationId id) const;

        /**
         * @brief Dispatches worker-published notifications at the owning main-thread synchronization point.
         * @pre Called by the owner thread of EngineDataBus, never from a worker callback.
         */
        void PumpMainThread() const;

    private:
        std::shared_ptr<ProjectCreationServiceState> state_;
        std::reference_wrapper<JobSystem> jobs_;
    };

}  // namespace Horo::Editor
