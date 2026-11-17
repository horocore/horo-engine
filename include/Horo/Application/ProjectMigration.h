#pragma once

/**
 * @file ProjectMigration.h
 * @brief Backend-neutral project migration catalog, planning, and pipeline contracts.
 */

#include "Horo/Application/ProjectVersion.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Result.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Security {
    class SecureRandomSource;
}

namespace Horo::Application {
    /** @brief Stable authored identity of one immutable migration definition. */
    struct StableMigrationId {
        std::string value;
        [[nodiscard]] bool operator==(const StableMigrationId &) const noexcept = default;
    };

    /** @brief Stable authored identity of one stage inside a definition. */
    struct MigrationStageId {
        std::string value;
        [[nodiscard]] bool operator==(const MigrationStageId &) const noexcept = default;
    };

    /** @brief Stable identity of a package or core provider required by a migration. */
    struct MigrationProviderId {
        std::string value;
        [[nodiscard]] bool operator==(const MigrationProviderId &) const noexcept = default;
    };

    /** @brief Canonical SHA-256 identity of a generated migration definition. */
    struct MigrationDefinitionHash {
        std::array<std::uint8_t, 32> bytes{};
        [[nodiscard]] bool operator==(const MigrationDefinitionHash &) const noexcept = default;
    };

    /** @brief Canonical SHA-256 identity of the complete generated definition set. */
    struct MigrationDefinitionSetHash {
        std::array<std::uint8_t, 32> bytes{};
        [[nodiscard]] bool operator==(const MigrationDefinitionSetHash &) const noexcept = default;
    };

    /** @brief Distinguishes retained sequential edges from declared checkpoint shortcuts. */
    enum class ProjectMigrationDefinitionKind : std::uint8_t {
        Sequential,
        Checkpoint,
    };

    /** @brief Portable authored document family available to migration queries. */
    enum class MigrationDocumentKind : std::uint8_t {
        ProjectMetadata,
        Scene,
        Prefab,
        AssetSidecar,
        ProjectSettings,
        Input,
        Material,
        Graph,
        Other,
    };

    /** @brief Query evaluated against the current candidate at a node boundary. */
    struct MigrationDocumentQuery {
        std::optional<MigrationDocumentKind> kind;

        [[nodiscard]] static MigrationDocumentQuery Any() noexcept {
            return {};
        }

        [[nodiscard]] static MigrationDocumentQuery Kind(MigrationDocumentKind value) noexcept {
            return MigrationDocumentQuery{.kind = value};
        }
    };

    /** @brief Operation-generation-safe inventory handle. */
    struct MigrationDocumentHandle {
        std::uint32_t index{};
        std::uint32_t generation{};
        [[nodiscard]] bool operator==(const MigrationDocumentHandle &) const noexcept = default;
    };

    /** @brief Immutable document view owned by the migration candidate. */
    struct ProjectDocumentView {
        MigrationDocumentHandle handle;
        std::string_view path;
        MigrationDocumentKind kind{MigrationDocumentKind::Other};
        std::span<const std::byte> bytes;
    };

    /** @brief Owned replacement returned by a parallel document stage. */
    struct MigrationDocumentChange {
        MigrationDocumentHandle document;
        std::vector<std::byte> replacement;
        bool remove{false};
        bool changed{true};
    };

    /** @brief Bounded migration execution limits injected by application composition. */
    struct ProjectMigrationLimits {
        std::size_t maxDocuments{100'000};
        std::uint64_t maxInputBytes{4ULL * 1024ULL * 1024ULL * 1024ULL};
        std::uint64_t maxOutputBytes{4ULL * 1024ULL * 1024ULL * 1024ULL};
        std::size_t maxDiagnostics{512};
        std::size_t maxConcurrency{8};
        std::uint64_t maxJournalBytes{128ULL * 1024ULL * 1024ULL};
        std::uint64_t maxHistoryBytes{16ULL * 1024ULL * 1024ULL};
    };

    /** @brief Explicit operation-owned capabilities available only during candidate preparation. */
    struct ProjectMigrationExecutionServices final {
        std::shared_ptr<Security::SecureRandomSource>
            random; /**< Host-selected entropy; absence never selects a fallback. Concurrent operations
                     sharing one instance require a provider supporting concurrent Fill calls. */
    };

    /** @brief Conservative storage-growth declaration used before transformation admission. */
    struct MigrationStorageEstimate {
        std::uint32_t maximumOutputRatioPermille{1000};
        std::uint64_t maximumAddedBytesPerDocument{};
        std::uint64_t maximumFixedBytes{};
    };

    /** @brief Stable metadata declared by one pipeline stage. */
    struct MigrationStageDescriptor {
        MigrationStageId id;
        std::vector<std::string> readFamilies;
        std::vector<std::string> writeFamilies;
        std::uint32_t estimatedWeight{1};
    };

    /** @brief Read-only context supplied to parallel document transformations. */
    struct MigrationStageContext {
        StableMigrationId definitionId;
        MigrationStageId stageId;
    };

    class ProjectMigrationContext;

    /** @brief Immutable per-document transformation suitable for bounded fan-out. */
    class IProjectMigrationDocumentStage {
    public:
        virtual ~IProjectMigrationDocumentStage() = default;
        [[nodiscard]] virtual MigrationStageDescriptor Describe() const = 0;
        [[nodiscard]] virtual Result<MigrationDocumentChange> Execute(const ProjectDocumentView &source,
                                                                      const MigrationStageContext &context,
                                                                      const CancellationToken &cancellation) const = 0;
    };

    /** @brief Serialized cross-document migration stage with constrained candidate access. */
    class IProjectMigrationStage {
    public:
        virtual ~IProjectMigrationStage() = default;
        [[nodiscard]] virtual MigrationStageDescriptor Describe() const = 0;
        [[nodiscard]] virtual Result<void> Execute(ProjectMigrationContext &context, const CancellationToken &cancellation) const = 0;
    };

    /** @brief Read-only validation barrier; it cannot access mutation methods. */
    class IProjectMigrationValidator {
    public:
        virtual ~IProjectMigrationValidator() = default;
        [[nodiscard]] virtual MigrationStageDescriptor Describe() const = 0;
        [[nodiscard]] virtual Result<void> Validate(const ProjectMigrationContext &context,
                                                    const CancellationToken &cancellation) const = 0;
    };

    /** @brief Typed node shape retained by an immutable migration pipeline. */
    enum class ProjectMigrationNodeKind : std::uint8_t {
        ForEach,
        Then,
        Validate,
    };

    /** @brief One immutable pipeline node and its owned stage implementation. */
    struct ProjectMigrationNode {
        ProjectMigrationNodeKind kind{ProjectMigrationNodeKind::Then};
        MigrationDocumentQuery query;
        std::shared_ptr<const IProjectMigrationDocumentStage> documentStage;
        std::shared_ptr<const IProjectMigrationStage> stage;
        std::shared_ptr<const IProjectMigrationValidator> validator;
    };

    /** @brief Immutable ordered pipeline for exactly one migration definition. */
    class ProjectMigrationPipeline {
    public:
        [[nodiscard]] std::span<const ProjectMigrationNode> Nodes() const noexcept {
            return nodes_;
        }

    private:
        friend class ProjectMigrationPipelineBuilder;

        explicit ProjectMigrationPipeline(std::vector<ProjectMigrationNode> nodes) : nodes_(std::move(nodes)) {}

        std::vector<ProjectMigrationNode> nodes_;
    };

    /** @brief Fluent typed builder; execution remains owned by the migration executor. */
    class ProjectMigrationPipelineBuilder {
    public:
        [[nodiscard]] static ProjectMigrationPipelineBuilder Begin(StableMigrationId definitionId);

        template <typename T, typename... Args>
        [[nodiscard]] ProjectMigrationPipelineBuilder &ForEach(MigrationDocumentQuery query, Args &&...args) {
            return AddForEach(std::move(query), std::make_shared<T>(std::forward<Args>(args)...));
        }

        template <typename T, typename... Args> [[nodiscard]] ProjectMigrationPipelineBuilder &Then(Args &&...args) {
            return AddThen(std::make_shared<T>(std::forward<Args>(args)...));
        }

        template <typename T, typename... Args> [[nodiscard]] ProjectMigrationPipelineBuilder &Validate(Args &&...args) {
            return AddValidator(std::make_shared<T>(std::forward<Args>(args)...));
        }

        [[nodiscard]] ProjectMigrationPipelineBuilder &AddForEach(MigrationDocumentQuery query,
                                                                  std::shared_ptr<const IProjectMigrationDocumentStage> stage);
        [[nodiscard]] ProjectMigrationPipelineBuilder &AddThen(std::shared_ptr<const IProjectMigrationStage> stage);
        [[nodiscard]] ProjectMigrationPipelineBuilder &AddValidator(std::shared_ptr<const IProjectMigrationValidator> validator);
        [[nodiscard]] Result<std::shared_ptr<const ProjectMigrationPipeline>> Build() &&;

    private:
        explicit ProjectMigrationPipelineBuilder(StableMigrationId definitionId) : definitionId_(std::move(definitionId)) {}

        StableMigrationId definitionId_;
        std::vector<ProjectMigrationNode> nodes_;
    };

    /** @brief Immutable exact-baseline migration edge discovered at build time. */
    struct ProjectMigrationDefinition {
        StableMigrationId id;
        ProjectMigrationDefinitionKind kind{ProjectMigrationDefinitionKind::Sequential};
        ContractBaselineVersion from;
        ContractBaselineVersion to;
        PersistentContractHash sourceContract;
        PersistentContractHash targetContract;
        std::vector<MigrationProviderId> requiredProviders;
        MigrationDefinitionHash hash;
        MigrationStorageEstimate storageEstimate;
        std::shared_ptr<const ProjectMigrationPipeline> pipeline;
    };

    /** @brief Exact checkpoint declaration frozen by the target release. */
    struct MigrationCheckpointDeclaration {
        StableMigrationId id;
        ContractBaselineVersion source;
        ContractBaselineVersion target;
    };

    /** @brief Bounded migration support horizon and canonical checkpoint choices. */
    struct ProjectMigrationSupportDescriptor {
        ContractBaselineVersion target;
        ContractBaselineVersion minimumMigratable;
        PersistentContractHash targetContract;
        std::vector<MigrationCheckpointDeclaration> checkpoints;
        std::vector<MigrationProviderId> availableProviders;
        std::shared_ptr<const IProjectMigrationValidator> targetValidator;
    };

    /** @brief Deterministic ordered definition chain without transformation execution. */
    struct ProjectMigrationPlan {
        ContractBaselineVersion source;
        ContractBaselineVersion target;
        std::vector<ProjectMigrationDefinition> definitions;
        std::uint64_t estimatedWeight{};
        std::shared_ptr<const IProjectMigrationValidator> targetValidator;
    };

    /** @brief Immutable validated migration graph and deterministic planner. */
    class ProjectMigrationRegistry {
    public:
        [[nodiscard]] static Result<ProjectMigrationRegistry> Create(std::span<const ProjectMigrationDefinition> definitions);

        [[nodiscard]] Result<ProjectMigrationPlan> Plan(const ContractBaselineVersion &source, const PersistentContractHash &sourceContract,
                                                        const ProjectMigrationSupportDescriptor &support) const;

        [[nodiscard]] std::span<const ProjectMigrationDefinition> Definitions() const noexcept {
            return definitions_;
        }

    private:
        explicit ProjectMigrationRegistry(std::vector<ProjectMigrationDefinition> definitions) : definitions_(std::move(definitions)) {}

        std::vector<ProjectMigrationDefinition> definitions_;
    };

    /** @brief Stable inventory entry reported without exposing an unrestricted filesystem path. */
    struct MigrationDocumentEntry {
        MigrationDocumentHandle handle;
        std::string path;
        MigrationDocumentKind kind{MigrationDocumentKind::Other};
        std::uint64_t inputBytes{};
    };

    /** @brief Constrained mutable candidate used by serialized stages and read-only validators. */
    class ProjectMigrationContext {
    public:
        struct State;

        [[nodiscard]] std::vector<MigrationDocumentEntry> ListDocuments(const MigrationDocumentQuery &query) const;
        [[nodiscard]] Result<ProjectDocumentView> ReadDocument(MigrationDocumentHandle document) const;
        [[nodiscard]] Result<void> ReplaceDocument(MigrationDocumentHandle document, std::vector<std::byte> replacement) const;
        [[nodiscard]] Result<void> AddDocument(const std::string &projectRelativePath, MigrationDocumentKind kind,
                                               std::vector<std::byte> document) const;
        [[nodiscard]] Result<void> RemoveDocument(MigrationDocumentHandle document) const;

        /**
         * @brief Requests bounded fresh entropy from the host-injected operation capability.
         * @param destination One through 256 writable bytes, cleared on failure.
         * @return Success or the provider's typed failure; absence fails closed without a fallback.
         * @details Available to serialized mutable stages, not const validators or parallel document stages.
         * Entropy is requested only during Execute(), never while constructing or validating descriptors.
         * The executor retains the injected provider through preparation; stages must not retain this context.
         */
        [[nodiscard]] Result<void> FillSecureRandom(std::span<std::byte> destination);

    private:
        friend class ProjectMigrationExecutor;

        explicit ProjectMigrationContext(std::shared_ptr<State> state) : state_(std::move(state)) {}

        std::shared_ptr<State> state_;
    };

    /** @brief Per-stage bounded diagnostic returned by verified execution. */
    struct ProjectMigrationDiagnostic {
        StableMigrationId definitionId;
        MigrationStageId stageId;
        std::optional<std::string> document;
        Error error;
    };

    /** @brief Verified dry-run result over a disposable real filesystem candidate. */
    struct ProjectMigrationDryRunResult {
        std::vector<std::string> changedFiles;
        std::uint64_t inputBytes{};
        std::uint64_t outputBytes{};
        std::vector<ProjectMigrationDiagnostic> diagnostics;
    };

    /** @brief Type of authoritative change represented by a prepared candidate. */
    enum class PreparedMigrationChangeKind : std::uint8_t {
        Add,
        Replace,
        Remove
    };

    /** @brief Immutable changed-file summary produced before publication authority is acquired. */
    struct PreparedMigrationChange {
        std::string path;
        PreparedMigrationChangeKind kind{PreparedMigrationChangeKind::Replace};
        std::uint64_t originalBytes{};
        std::uint64_t stagedBytes{};
    };

    /** @brief Transaction-owned document applied to a prepared candidate before final validation. */
    struct PreparedMigrationDocument {
        std::string path;
        MigrationDocumentKind kind{MigrationDocumentKind::Other};
        std::vector<std::byte> bytes;
    };

    /** @brief Move-only unpublished filesystem candidate; destruction removes its disposable state. */
    class PreparedProjectMigration {  // NOSONAR(cpp:S3624) Pimpl pattern: destructor defined in .cpp to complete State
    public:
        struct State;
        PreparedProjectMigration(const PreparedProjectMigration &) = delete;
        PreparedProjectMigration &operator=(const PreparedProjectMigration &) = delete;
        PreparedProjectMigration(PreparedProjectMigration &&) noexcept;
        PreparedProjectMigration &operator=(PreparedProjectMigration &&) noexcept;
        ~PreparedProjectMigration();

        [[nodiscard]] const std::filesystem::path &CandidateRoot() const noexcept;
        [[nodiscard]] std::span<const PreparedMigrationChange> Changes() const noexcept;
        [[nodiscard]] std::uint64_t InputBytes() const noexcept;
        [[nodiscard]] std::uint64_t OutputBytes() const noexcept;
        /**
         * @brief Copies one live document from the unpublished candidate.
         * @param projectRelativePath Canonical project-relative inventory path.
         * @return Owned bytes, or a typed failure when the path is absent or removed.
         */
        [[nodiscard]] Result<std::vector<std::byte>> ReadCandidateDocument(std::string_view projectRelativePath) const;
        [[nodiscard]] Result<void> VerifySourceUnchanged() const;
        /** @brief Transfers cleanup ownership to a durable recovery journal before authoritative publication. */
        void PreserveForRecovery() const noexcept;

    private:
        friend class ProjectMigrationExecutor;
        explicit PreparedProjectMigration(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };

    /** @brief Executes planned definitions without owning authoritative publication. */
    class ProjectMigrationExecutor {
    public:
        /**
         * @brief Builds an unpublished candidate through all definition-local validation barriers.
         * @param projectRoot Read-only authoritative source root.
         * @param candidateRoot Disposable unpublished output root.
         * @param plan Validated immutable migration plan.
         * @param jobs Operation-owned scheduler.
         * @param limits Finite inventory and execution bounds.
         * @param cancellation Parent cancellation intent.
         * @param services Host-selected capabilities retained through preparation; empty preserves capability-free pipelines.
         * @return Unpublished candidate or typed inventory, capability, stage or validation failure.
         */
        [[nodiscard]] static Result<PreparedProjectMigration> Prepare(const std::filesystem::path &projectRoot,
                                                                      const std::filesystem::path &candidateRoot,
                                                                      const ProjectMigrationPlan &plan, JobSystem &jobs,
                                                                      const ProjectMigrationLimits &limits = {},
                                                                      CancellationToken cancellation = {},
                                                                      const ProjectMigrationExecutionServices &services = {});

        /**
         * @brief Applies transaction-owned documents and runs the final target-contract validator.
         * @param prepared Unpublished candidate returned by Prepare().
         * @param documents Root metadata, history, or other transaction-owned candidate documents.
         * @param targetValidator Optional validator for the complete target candidate.
         * @param cancellation Parent operation cancellation, honored before publication begins.
         * @return Success after materialization and source revalidation, or a typed failure.
         */
        [[nodiscard]] static Result<void> Finalize(PreparedProjectMigration &prepared, std::span<const PreparedMigrationDocument> documents,
                                                   std::shared_ptr<const IProjectMigrationValidator> targetValidator,
                                                   CancellationToken cancellation = {});

        /**
         * @brief Runs the real pipeline against disposable same-filesystem state and deletes it afterward.
         * @param projectRoot Authoritative project root, read-only for this operation.
         * @param plan Deterministic plan from ProjectMigrationRegistry.
         * @param jobs Injected scheduler used only through operation-owned TaskGroup scopes.
         * @param limits Bounded inventory and execution limits.
         * @param cancellation Parent operation cancellation.
         * @param services Same explicit capabilities used by real preparation; no native defaults are selected.
         * @return Verified change summary or a typed inventory/stage/validation failure.
         */
        [[nodiscard]] static Result<ProjectMigrationDryRunResult> VerifiedDryRun(const std::filesystem::path &projectRoot,
                                                                                 const ProjectMigrationPlan &plan, JobSystem &jobs,
                                                                                 const ProjectMigrationLimits &limits = {},
                                                                                 CancellationToken cancellation = {},
                                                                                 const ProjectMigrationExecutionServices &services = {});
    };

    /** @brief Constructs the build-generated core migration catalog. */
    [[nodiscard]] Result<std::vector<ProjectMigrationDefinition>> BuildBuiltInProjectMigrationCatalog();
}  // namespace Horo::Application
