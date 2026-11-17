#pragma once

/** @file ProjectMigrationTransaction.h @brief Crash-safe authoritative project migration transaction. */

#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Application/ProjectMigration.h"
#include "Horo/Editor/ProjectMutation.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Editor {
    /** @brief Content-addressed identity of one frozen migration-journal reader contract. */
    struct MigrationRecoveryContractId {
        std::array<std::uint8_t, 32> bytes{};
        [[nodiscard]] bool operator==(const MigrationRecoveryContractId &) const noexcept = default;
    };

    /** @brief Parses canonical `sha256:` migration-recovery contract identity. */
    [[nodiscard]] Result<MigrationRecoveryContractId> ParseMigrationRecoveryContractId(std::string_view text);
    /** @brief Formats one recovery-contract identity as canonical lowercase SHA-256. */
    [[nodiscard]] std::string FormatMigrationRecoveryContractId(const MigrationRecoveryContractId &id);
    /** @brief Returns the recovery contract written by this Horo release. */
    [[nodiscard]] MigrationRecoveryContractId CurrentMigrationRecoveryContractId();

    /** @brief Bounded storage-admission policy supplied by application composition. */
    struct ProjectMigrationStoragePolicy {
        std::uint64_t minimumSafetyMarginBytes{64ULL * 1024ULL * 1024ULL};
        std::uint32_t safetyMarginPermille{50};
        std::uint64_t maximumCleanupBytes{4ULL * 1024ULL * 1024ULL * 1024ULL};
        std::size_t maximumCleanupDirectories{128};
    };

    /** @brief Individual values used by conservative migration storage admission. */
    struct ProjectMigrationStorageAdmission {
        std::uint64_t stagedOutputBytes{};
        std::uint64_t rollbackBytes{};
        std::uint64_t journalAndHistoryBytes{};
        std::uint64_t cleanupRemnantBytes{};
        std::uint64_t safetyMarginBytes{};
        std::uint64_t availableBytes{};
        /** @brief Returns the saturated total temporary-byte requirement. */
        [[nodiscard]] std::uint64_t RequiredBytes() const noexcept;
    };

    /** @brief Complete authoritative migration request admitted by the mutation coordinator. */
    struct ProjectMigrationTransactionRequest {
        std::filesystem::path projectRoot;
        Application::ProjectMetadata sourceMetadata;
        Application::ContractBaselineVersion sourceBaseline;
        Application::ReleaseCompatibilityDecision targetDecision;
        Application::ProjectMigrationPlan plan;
        std::string engineBuildIdentity;
        Application::ProjectMigrationLimits limits;
        CancellationToken cancellation;
    };

    /** @brief Durable commit result returned after root-last publication completes. */
    struct ProjectMigrationTransactionResult {
        std::string operationId;
        Application::MigrationHistoryHead historyHead;
        std::vector<std::string> changedFiles;
        bool cancellationDeferred{};
        bool cleanupDeferred{};
    };

    /** @brief Evidence-derived action for exactly one unfinished migration journal. */
    enum class MigrationRecoveryAction : std::uint8_t {
        None,
        DiscardUnpublishedStaging,
        ResumePublish,
        RestoreOriginals,
        FinalizeCommittedMigration,
        Unrecoverable,
    };

    /** @brief Read-only recovery classification suitable for project-open preflight. */
    struct MigrationRecoverySnapshot {
        MigrationRecoveryAction action{MigrationRecoveryAction::None};
        std::optional<std::string> operationId;
        std::optional<Error> diagnostic;
    };

    /** @brief Owns crash-safe migration publication and recovery under a mutation lease. */
    class ProjectMigrationTransactionService {
    public:
        /**
         * @brief Composes the transaction service from host-owned dependencies.
         * @param files Durable filesystem authority.
         * @param wallClock UTC timestamp source for receipts.
         * @param mutations Shared project mutation coordinator.
         * @param jobs Scheduler consumed by migration preparation.
         * @param storagePolicy Conservative admission and cleanup bounds.
         * @param executionServices Host-selected preparation capabilities retained by this service and each synchronous execution.
         */
        ProjectMigrationTransactionService(DurableFileSystem &files, WallClock &wallClock, ProjectMutationCoordinator &mutations,
                                           JobSystem &jobs, const ProjectMigrationStoragePolicy &storagePolicy = {},
                                           Application::ProjectMigrationExecutionServices executionServices = {}) noexcept;

        /** @brief Prepares, validates, and durably publishes one migration. @param request Transaction request. @return
         * Commit result or typed failure. */
        [[nodiscard]] Result<ProjectMigrationTransactionResult> Execute(const ProjectMigrationTransactionRequest &request);
        /** @brief Computes typed read-only storage admission components. @param request Proposed migration.
         * @return Conservative components or inventory/capacity query failure. */
        [[nodiscard]] Result<ProjectMigrationStorageAdmission> InspectStorageAdmission(
            const ProjectMigrationTransactionRequest &request) const;
        /** @brief Classifies an unfinished journal without mutating the project. @param projectRoot Authoritative root.
         * @return Evidence-derived recovery action. */
        [[nodiscard]] MigrationRecoverySnapshot InspectPendingRecovery(const std::filesystem::path &projectRoot) const;
        /** @brief Executes the currently provable recovery action under the mutation lock. @param projectRoot Authoritative
         * root. @param cancellation Cancellation honored before a critical publish resumes. @return Success or typed
         * fail-closed recovery error. */
        [[nodiscard]] Result<void> Recover(const std::filesystem::path &projectRoot, CancellationToken cancellation = {});
        /** @brief Removes bounded committed cleanup remnants without affecting active recovery. @param projectRoot
         * Authoritative root. @return Success; cleanup failures are returned as non-destructive diagnostics. */
        [[nodiscard]] Result<void> CleanupCommittedMigrations(const std::filesystem::path &projectRoot) const;

    private:
        DurableFileSystem &files_;
        WallClock &wallClock_;
        ProjectMutationCoordinator &mutations_;
        JobSystem &jobs_;
        ProjectMigrationStoragePolicy storagePolicy_;
        Application::ProjectMigrationExecutionServices executionServices_;
    };
}  // namespace Horo::Editor
