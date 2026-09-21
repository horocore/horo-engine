#pragma once

/**
 * @file PrefabDiagnostics.h
 * @brief Bounded typed prefab failure evidence and shared host projections.
 */

#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/Diagnostics.h"
#include "Horo/Foundation/ErrorCode.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Prefab/PrefabDependencyGraph.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Horo::Prefab {
    /** @brief Maximum typed cause identities retained by one prefab diagnostic. */
    inline constexpr std::size_t MaximumPrefabDiagnosticCauseDepth = 8;
    /** @brief Maximum source/dependency identities retained by one prefab diagnostic chain. */
    inline constexpr std::size_t MaximumPrefabDiagnosticDependencyDepth = 16;
    /** @brief Maximum UTF-8 bytes retained in one prefab diagnostic message. */
    inline constexpr std::size_t MaximumPrefabDiagnosticMessageBytes = 512;
    /** @brief Maximum bytes retained for one source-navigation path. */
    inline constexpr std::size_t MaximumPrefabDiagnosticSourcePathBytes = 1024;
    /** @brief Maximum bytes retained by one stable error-domain or code identity. */
    inline constexpr std::size_t MaximumPrefabDiagnosticIdentityBytes = 160;

    /** @brief Stable prefab failure area used by Build Output, observability and filtering adapters. */
    enum class PrefabDiagnosticCategory : std::uint8_t {
        Source,
        Graph,
        Expansion,
        Override,
        Cook,
        Migration,
        RuntimeSpawn,
        Lifecycle,
        Count,
    };

    /** @brief Typed operation that produced one prefab diagnostic. */
    enum class PrefabDiagnosticOperation : std::uint8_t {
        Parse,
        ValidateSource,
        BuildGraph,
        Expand,
        ApplyOverride,
        Cook,
        Migrate,
        Load,
        Spawn,
        Commit,
        Publish,
        Count,
    };

    /** @brief Stable domain class of an error identity retained in a prefab diagnostic chain. */
    enum class PrefabDiagnosticOrigin : std::uint8_t {
        Prefab,
        Asset,
        Scene,
        Gameplay,
        Migration,
        Count,
    };

    /** @brief One typed error identity preserved from the outer failure or an immutable cause chain. */
    struct PrefabDiagnosticCause final {
        ErrorDomainId domain;
        DiagnosticCode code;
        PrefabDiagnosticOrigin origin{PrefabDiagnosticOrigin::Prefab};
        DiagnosticSeverity severity{DiagnosticSeverity::Error};
    };

    /** @brief One stable asset edge retained in dependency order for source, graph, cook or spawn evidence. */
    struct PrefabDiagnosticDependency final {
        Assets::AssetId asset;
        PrefabDependencyKind kind{PrefabDependencyKind::Resource};
        std::optional<PrefabSourceRevision> revision;
    };

    /**
     * @brief Mandatory prefab identity plus optional stable target and host correlation evidence.
     *
     * The context contains no paths as identity and no borrowed owner state. The optional source
     * location is only a navigation hint; containment and current-revision checks remain owned by
     * the source-navigation service.
     */
    struct PrefabDiagnosticContext final {
        Assets::AssetId prefabAsset;
        PrefabSourceRevision revision;
        Assets::AssetRegistryRevision registryRevision;
        std::optional<PrefabInstanceId> instance;
        std::optional<PrefabObjectAddress> localMember;
        std::optional<PrefabPropertyAddress> property;
        std::optional<OperationId> operationId;
        std::optional<DiagnosticSourceLocation> source;
    };

    /** @brief Complete typed input for constructing one bounded immutable prefab diagnostic. */
    struct PrefabDiagnosticRecordInput final {
        PrefabDiagnosticCategory category{PrefabDiagnosticCategory::Source};
        PrefabDiagnosticOperation operation{PrefabDiagnosticOperation::ValidateSource};
        PrefabDiagnosticContext context;
        std::span<const PrefabDiagnosticDependency> dependencyChain;
    };

    /**
     * @brief Owned, backend-neutral prefab failure evidence.
     *
     * The record copies every accepted field into bounded storage. It owns no document,
     * registry snapshot, runtime entity, filesystem handle, logger, Build Output store,
     * or mutation capability, so it can safely outlive the operation that produced it.
     */
    struct PrefabDiagnosticRecord final {
        std::uint32_t schemaVersion{1};
        PrefabDiagnosticCategory category{PrefabDiagnosticCategory::Source};
        PrefabDiagnosticOperation operation{PrefabDiagnosticOperation::ValidateSource};
        PrefabDiagnosticOrigin origin{PrefabDiagnosticOrigin::Prefab};
        Assets::AssetId prefabAsset;
        PrefabSourceRevision revision;
        Assets::AssetRegistryRevision registryRevision;
        std::optional<PrefabInstanceId> instance;
        std::optional<PrefabObjectAddress> localMember;
        std::optional<PrefabPropertyAddress> property;
        std::optional<OperationId> operationId;
        ErrorDomainId domain;
        DiagnosticCode code;
        DiagnosticSeverity severity{DiagnosticSeverity::Error};
        std::string message;
        std::optional<DiagnosticSourceLocation> source;
        std::array<PrefabDiagnosticCause, MaximumPrefabDiagnosticCauseDepth> causes;
        std::uint8_t causeCount{};
        std::array<PrefabDiagnosticDependency, MaximumPrefabDiagnosticDependencyDepth> dependencyChain;
        std::uint8_t dependencyCount{};

        /** @brief Returns the retained cause identities from outermost to innermost. @return Borrowed bounded causes. */
        [[nodiscard]] std::span<const PrefabDiagnosticCause> Causes() const noexcept {
            return {causes.data(), causeCount};
        }

        /** @brief Returns the retained dependency path in captured order. @return Borrowed bounded chain. */
        [[nodiscard]] std::span<const PrefabDiagnosticDependency> DependencyChain() const noexcept {
            return {dependencyChain.data(), dependencyCount};
        }
    };

    /**
     * @brief Returns the stable dotted category name used by presentation adapters.
     * @param category Closed category value.
     * @return Process-lifetime name, or empty for an unknown value.
     */
    [[nodiscard]] std::string_view PrefabDiagnosticCategoryName(PrefabDiagnosticCategory category) noexcept;

    /**
     * @brief Returns the stable dotted operation name used as a Build Output stage.
     * @param operation Closed operation value.
     * @return Process-lifetime name, or empty for an unknown value.
     */
    [[nodiscard]] std::string_view PrefabDiagnosticOperationName(PrefabDiagnosticOperation operation) noexcept;

    /**
     * @brief Returns the canonical prefab error descriptors admitted as primary or cause identities.
     * @return Borrowed immutable process-lifetime descriptor pointers.
     */
    [[nodiscard]] std::span<const ErrorCodeDescriptor *const> PrefabDiagnosticErrorDescriptors() noexcept;

    /**
     * @brief Builds bounded prefab evidence from one typed error and stable operation context.
     * @param input Category, operation, mandatory prefab revision identity and optional target context.
     * @param error Typed operation failure; supported Asset, Scene, Gameplay and migration causes are retained exactly.
     * @return Owned record or PrefabErrors::DiagnosticInvalid/DiagnosticUnsupported/BudgetExceeded.
     */
    [[nodiscard]] Result<PrefabDiagnosticRecord> MakePrefabDiagnosticRecord(const PrefabDiagnosticRecordInput &input, const Error &error);

    /**
     * @brief Convenience overload using one explicit category, error, context and optional operation.
     * @param category Stable prefab failure area.
     * @param error Typed operation failure.
     * @param context Mandatory prefab identity and optional navigation/correlation evidence.
     * @param operation Typed operation; validation is the default for source diagnostics.
     * @param dependencyChain Borrowed dependency path copied on success.
     * @return Owned bounded diagnostic record.
     */
    [[nodiscard]] Result<PrefabDiagnosticRecord> MakePrefabDiagnosticRecord(
        PrefabDiagnosticCategory category, const Error &error, const PrefabDiagnosticContext &context,
        PrefabDiagnosticOperation operation = PrefabDiagnosticOperation::ValidateSource,
        std::span<const PrefabDiagnosticDependency> dependencyChain = {});

    /**
     * @brief Convenience overload with the operation placed before the typed error.
     * @param category Stable prefab failure area.
     * @param operation Typed operation that produced the failure.
     * @param error Typed operation failure.
     * @param context Mandatory prefab identity and optional navigation/correlation evidence.
     * @param dependencyChain Borrowed dependency path copied on success.
     * @return Owned bounded diagnostic record.
     */
    [[nodiscard]] Result<PrefabDiagnosticRecord> MakePrefabDiagnosticRecord(
        PrefabDiagnosticCategory category, PrefabDiagnosticOperation operation, const Error &error, const PrefabDiagnosticContext &context,
        std::span<const PrefabDiagnosticDependency> dependencyChain = {});

    /**
     * @brief Projects prefab evidence into the existing Foundation Build Output record contract.
     * @param record Owned prefab diagnostic detail retained separately by the caller when needed.
     * @param result Optional terminal Build Output result; failed is the default for a diagnostic.
     * @return Build Output value with stable code, severity, operation stage, message and source navigation hint.
     */
    [[nodiscard]] BuildOutputRecord MakePrefabBuildOutputRecord(const PrefabDiagnosticRecord &record,
                                                                BuildOutputResult result = BuildOutputResult::Failed);
}  // namespace Horo::Prefab
