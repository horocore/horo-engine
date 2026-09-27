#pragma once

/**
 * @file SaveMigration.h
 * @brief Deterministic runtime-save migration definitions, planning, and staging.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Save/SaveArchiveMetadata.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Horo::Runtime {
    /** @brief Maximum canonical bytes accepted for one migration identity. */
    inline constexpr std::size_t MaximumSaveMigrationIdentityBytes = 128;

    /** @brief Maximum migration definitions retained by one mutable registry. */
    inline constexpr std::size_t MaximumSaveMigrationDefinitions = 1'024;

    /** @brief Maximum definitions selected by one migration plan. */
    inline constexpr std::size_t MaximumSaveMigrationPlanSteps = 256;

    /** @brief Maximum participant payload bytes retained by one migration state. */
    inline constexpr std::uint64_t MaximumSaveMigrationParticipantPayloadBytes = 64ULL * 1024ULL * 1024ULL;

    /** @brief Maximum aggregate payload bytes retained by one migration state. */
    inline constexpr std::uint64_t MaximumSaveMigrationTotalPayloadBytes = 256ULL * 1024ULL * 1024ULL;

    /** @brief Stable identity of one registered runtime-save migration definition. */
    struct SaveMigrationId final {
        std::string value;

        /** @brief Parses a canonical lowercase migration identity. @param text Candidate identity. @return Valid identity or a typed error.
         */
        [[nodiscard]] static Result<SaveMigrationId> Parse(std::string_view text);
        /** @brief Reports whether this identity has canonical bounded spelling. @return True for a usable identity. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const SaveMigrationId &) const noexcept = default;
    };

    /** @brief Independently evolving version axis covered by a migration edge. */
    enum class SaveMigrationAxis : std::uint8_t {
        ArchiveFormat,
        SaveSchema,
        ParticipantSchema,
        ProductCompatibility,
        Count,
    };

    /** @brief Whether an edge is a normal sequential hop or a tested shortcut. */
    enum class SaveMigrationStepKind : std::uint8_t {
        Sequential,
        Checkpoint,
        Count,
    };

    /** @brief Bounded detached bytes for one participant during migration staging. */
    struct SaveMigrationParticipantState final {
        SaveParticipantId participant;
        ParticipantSchemaVersion schemaVersion;
        bool required{true};
        std::vector<std::byte> payload;

        [[nodiscard]] auto operator<=>(const SaveMigrationParticipantState &) const noexcept = default;
    };

    /**
     * @brief Detached save state used as both a read-only source and an owned candidate.
     *
     * The executor copies this state once before invoking migration functions. Each function then
     * receives ownership of the detached candidate, never a reference to the caller's source archive
     * or live runtime state.
     */
    struct SaveMigrationState final {
        ArchiveFormatVersion archiveFormatVersion;
        SaveSchemaVersion saveSchemaVersion;
        ProductSaveCompatibilityVersion productCompatibility;
        std::vector<SaveMigrationParticipantState> participants;
        std::vector<std::byte> archiveBytes;

        [[nodiscard]] auto operator<=>(const SaveMigrationState &) const noexcept = default;
    };

    /** @brief Read-only source state supplied to a migration operation. */
    using SaveMigrationSource = SaveMigrationState;
    /** @brief Owned candidate returned by a migration operation. */
    using SaveMigrationCandidate = SaveMigrationState;

    /** @brief Context identifying the definition currently transforming detached state. */
    struct SaveMigrationStepContext final {
        SaveMigrationId id;
        SaveMigrationAxis axis{SaveMigrationAxis::ArchiveFormat};
        SaveMigrationStepKind kind{SaveMigrationStepKind::Sequential};
        std::optional<SaveParticipantId> participant;
        std::uint64_t remainingWorkBytes{};  /**< Remaining operation work before callback expansion; output is charged on return. */
        std::uint64_t maximumArchiveBytes{}; /**< Trusted archive candidate ceiling. */
        std::uint64_t maximumParticipantPayloadBytes{}; /**< Trusted per-participant candidate ceiling. */
        std::uint64_t maximumTotalPayloadBytes{};       /**< Trusted aggregate participant candidate ceiling. */
    };

    /** @brief Function receiving ownership of the current detached candidate and returning its replacement. */
    using SaveMigrationFn = std::function<Result<SaveMigrationCandidate>(SaveMigrationCandidate, const SaveMigrationStepContext &)>;

    /** @brief One archive-format migration edge. */
    struct ArchiveMigrationStep final {
        SaveMigrationId id;
        SaveMigrationStepKind kind{SaveMigrationStepKind::Sequential};
        ArchiveFormatVersion from;
        ArchiveFormatVersion to;
        SaveMigrationFn migrate;
        std::vector<SaveMigrationId> equivalentSequentialSteps;
        std::uint64_t estimatedWork{1};
    };

    /** @brief One whole-save-schema migration edge. */
    struct SaveSchemaMigrationStep final {
        SaveMigrationId id;
        SaveMigrationStepKind kind{SaveMigrationStepKind::Sequential};
        SaveSchemaVersion from;
        SaveSchemaVersion to;
        SaveMigrationFn migrate;
        std::vector<SaveMigrationId> equivalentSequentialSteps;
        std::uint64_t estimatedWork{1};
    };

    /** @brief One participant-local schema migration edge. */
    struct ParticipantMigrationStep final {
        SaveMigrationId id;
        SaveParticipantId participant;
        SaveMigrationStepKind kind{SaveMigrationStepKind::Sequential};
        ParticipantSchemaVersion from;
        ParticipantSchemaVersion to;
        SaveMigrationFn migrate;
        std::vector<SaveMigrationId> equivalentSequentialSteps;
        std::uint64_t estimatedWork{1};
    };

    /** @brief Variant containing every supported migration edge kind. */
    using SaveMigrationDefinition = std::variant<ArchiveMigrationStep, SaveSchemaMigrationStep, ParticipantMigrationStep>;

    /** @brief Explicit release declaration for one checkpoint shortcut. */
    struct SaveMigrationCheckpointDeclaration final {
        SaveMigrationId id;
        SaveMigrationAxis axis{SaveMigrationAxis::ArchiveFormat};
        std::optional<SaveParticipantId> participant;
        std::vector<SaveMigrationId> equivalentSequentialSteps;
    };

    /** @brief Release support ranges and the checkpoints explicitly authorized by that release. */
    struct SaveMigrationSupportDescriptor final {
        SaveCompatibilityPolicy compatibility;
        std::vector<SaveMigrationCheckpointDeclaration> checkpoints;
    };

    /** @brief Target participant schema selected by a migration plan. */
    struct SaveMigrationParticipantTarget final {
        SaveParticipantId participant;
        ParticipantSchemaVersion schemaVersion;
        bool required{true};

        [[nodiscard]] auto operator<=>(const SaveMigrationParticipantTarget &) const noexcept = default;
    };

    /** @brief Deterministic ordered migration route pinned to one registry snapshot. */
    struct SaveMigrationPlan final {
        ArchiveFormatVersion sourceArchiveFormat;
        SaveSchemaVersion sourceSaveSchema;
        ProductSaveCompatibilityVersion sourceProductCompatibility;
        ArchiveFormatVersion targetArchiveFormat;
        SaveSchemaVersion targetSaveSchema;
        ProductSaveCompatibilityVersion targetProductCompatibility;
        std::vector<SaveMigrationDefinition> definitions;
        std::vector<SaveMigrationParticipantTarget> participantTargets;
        bool participantRequirementChanges{}; /**< Target composition changes participant requiredness. */
        std::uint64_t estimatedWork{};
        std::uint64_t registryGeneration{};
        Sha256Digest registryIdentity;
        Sha256Digest routeIdentity;

        /** @brief Reports whether all version axes are already directly readable. @return True for a no-op plan. */
        [[nodiscard]] bool IsNoOp() const noexcept;
    };

    /** @brief Bounded limits for registry validation, planning, and detached candidate staging. */
    struct SaveMigrationLimits final {
        std::size_t maximumDefinitions{MaximumSaveMigrationDefinitions};
        std::size_t maximumPlanSteps{MaximumSaveMigrationPlanSteps};
        std::size_t maximumParticipants{256};
        std::uint64_t maximumArchiveBytes{4ULL * 1024ULL * 1024ULL * 1024ULL};
        std::uint64_t maximumParticipantPayloadBytes{MaximumSaveMigrationParticipantPayloadBytes};
        std::uint64_t maximumTotalPayloadBytes{MaximumSaveMigrationTotalPayloadBytes};
        std::uint64_t maximumCumulativeWorkBytes{8ULL * 1024ULL * 1024ULL * 1024ULL}; /**< Source, step input/output and declared work. */
    };

    /** @brief Registration evidence tied to the mutable registry generation. */
    struct SaveMigrationRegistration final {
        SaveMigrationId id;
        std::uint64_t registryGeneration{};
    };

    namespace SaveMigrationRegistryDetail {
        struct SnapshotStorage;
    }

    /** @brief Immutable registry view retained by one migration operation. */
    class SaveMigrationRegistrySnapshot final {
    public:
        SaveMigrationRegistrySnapshot() = default;

        /** @brief Reports whether this snapshot owns a sealed catalog. @return True for a valid snapshot. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the exact mutable-registry generation captured by this snapshot. @return Non-zero generation or zero when
         * invalid. */
        [[nodiscard]] std::uint64_t Generation() const noexcept;
        /** @brief Returns the canonical identity of the frozen catalog. @return Stable SHA-256 catalog identity. */
        [[nodiscard]] const Sha256Digest &CatalogIdentity() const noexcept;
        /** @brief Returns definitions in deterministic catalog order. @return Borrowed immutable definitions. */
        [[nodiscard]] std::span<const SaveMigrationDefinition> Definitions() const noexcept;

        /**
         * @brief Plans a deterministic route for every migration-required version axis.
         * @param source Detached source metadata and participant schema inventory.
         * @param support Target release ranges and declared checkpoint choices.
         * @param limits Trusted bounded planning and source-state limits.
         * @return Frozen plan or a typed actionable compatibility/catalog failure.
         */
        [[nodiscard]] Result<SaveMigrationPlan> Plan(const SaveMigrationSource &source, const SaveMigrationSupportDescriptor &support,
                                                     const SaveMigrationLimits &limits = {}) const;

    private:
        friend class SaveMigrationRegistry;
        SaveMigrationRegistrySnapshot(std::uint64_t generation,
                                      std::shared_ptr<const SaveMigrationRegistryDetail::SnapshotStorage> storage) noexcept;

        std::uint64_t generation_{};
        std::shared_ptr<const SaveMigrationRegistryDetail::SnapshotStorage> storage_;
    };

    /**
     * @brief Host-owned migration catalog with explicit immutable operation snapshots.
     *
     * Register/unregister is a composition-time operation. Runtime migration receives only a
     * snapshot, so later catalog changes cannot alter an in-flight route or unload its functions.
     */
    class SaveMigrationRegistry final {
    public:
        SaveMigrationRegistry() = default;
        SaveMigrationRegistry(const SaveMigrationRegistry &) = delete;
        SaveMigrationRegistry &operator=(const SaveMigrationRegistry &) = delete;
        SaveMigrationRegistry(SaveMigrationRegistry &&) noexcept = default;
        SaveMigrationRegistry &operator=(SaveMigrationRegistry &&) noexcept = default;

        /**
         * @brief Creates a validated registry from an initial definition set.
         * @param definitions Definitions copied into the registry.
         * @param limits Finite catalog limits.
         * @return Mutable composition registry or a typed graph/identity failure.
         */
        [[nodiscard]] static Result<SaveMigrationRegistry> Create(std::span<const SaveMigrationDefinition> definitions,
                                                                  const SaveMigrationLimits &limits = {});

        /**
         * @brief Adds one definition and advances the mutable catalog generation.
         * @param definition Definition copied into the catalog.
         * @return Registration evidence or a typed validation/capacity/lifecycle failure.
         * @pre Called by the owning composition thread while no operation is composing a snapshot.
         */
        [[nodiscard]] Result<SaveMigrationRegistration> Register(const SaveMigrationDefinition &definition);

        /**
         * @brief Removes one definition and advances the mutable catalog generation.
         * @param id Exact identity to remove.
         * @return True when removed, false when absent, or a typed lifecycle failure.
         * @pre Called by the owning composition thread while no operation is composing a snapshot.
         */
        [[nodiscard]] Result<bool> Unregister(const SaveMigrationId &id);

        /**
         * @brief Freezes the current catalog for an operation.
         * @return Immutable generation-pinned snapshot or a typed allocation failure.
         */
        [[nodiscard]] Result<SaveMigrationRegistrySnapshot> Snapshot() const;

        /** @brief Closes future catalog mutation; issued snapshots remain valid. */
        void Close() noexcept;
        /** @brief Reports whether composition mutation is closed. @return Current lifecycle state. */
        [[nodiscard]] bool IsClosed() const noexcept;
        /** @brief Returns the current mutable catalog generation. @return Non-zero generation. */
        [[nodiscard]] std::uint64_t Generation() const noexcept;

    private:
        explicit SaveMigrationRegistry(std::vector<SaveMigrationDefinition> definitions, SaveMigrationLimits limits)
            : definitions_(std::move(definitions)), limits_(limits) {}

        std::vector<SaveMigrationDefinition> definitions_;
        SaveMigrationLimits limits_;
        std::uint64_t generation_{1};
        bool closed_{false};
    };

    /**
     * @brief Validates and executes a frozen plan entirely in detached owned staging.
     *
     * The source is copied once before the first callback. Each callback receives ownership of the
     * current detached candidate, so successful execution returns a new candidate without an
     * additional full-state copy between migration steps.
     */
    class SaveMigrationExecutor final {
    public:
        /**
         * @brief Applies every planned definition and returns a new candidate.
         * @param source Immutable source state; it remains unchanged.
         * @param plan Generation-pinned deterministic plan.
         * @param limits Finite candidate and callback-output bounds.
         * @return Owned migrated candidate or a typed step/candidate/source-preservation failure.
         */
        [[nodiscard]] static Result<SaveMigrationCandidate> Migrate(const SaveMigrationSource &source, const SaveMigrationPlan &plan,
                                                                    const SaveMigrationLimits &limits = {});

        /** @brief Alias for Migrate used by operation hosts. @copydoc Migrate */
        [[nodiscard]] static Result<SaveMigrationCandidate> Execute(const SaveMigrationSource &source, const SaveMigrationPlan &plan,
                                                                    const SaveMigrationLimits &limits = {}) {
            return Migrate(source, plan, limits);
        }
    };

    /**
     * @brief Validates detached migration-state bounds and canonical participant ordering.
     * @param state Source or candidate state.
     * @param limits Trusted state limits.
     * @return Success or a typed candidate/limit failure.
     */
    [[nodiscard]] Result<void> ValidateSaveMigrationState(const SaveMigrationState &state, const SaveMigrationLimits &limits = {});
}  // namespace Horo::Runtime
