#pragma once

/**
 * @file ReplicationDeclarationPolicy.h
 * @brief Bounded inspection and admission of authored replication requirements.
 */

#include "Horo/Network/ReplicationDescriptorRegistry.h"

#include <optional>

namespace Horo::Network {
    /** @brief Explicit host operation; editor inspection never grants runtime admission. */
    enum class ReplicationDeclarationUse : std::uint8_t {
        Cook,
        EditorInspection,
        ServerActivation,
        ClientActivation,
        Count
    };

    /** @brief Required declarations fail closed; optional declarations explicitly permit omission, never opaque apply. */
    enum class ReplicationDeclarationRequirement : std::uint8_t {
        Required,
        Optional,
        Count
    };

    /** @brief Portable inert requirement, independent of whether its declaring module is installed. */
    struct ReplicationDeclaration final {
        ReplicationSchemaDescriptor schema; /**< Stable owner, identity, version and field semantics expected by the product. */
        ReplicationDeclarationRequirement requirement{ReplicationDeclarationRequirement::Required}; /**< Host-authored requiredness. */
    };

    /** @brief Closed diagnostic classification without native details or replicated values. */
    enum class ReplicationDeclarationProblem : std::uint8_t {
        MalformedDeclaration,
        DuplicateSchema,
        MissingModule,
        UnknownSchema,
        OwnerMismatch,
        VersionIncompatible,
        FieldIncompatible,
        Count
    };

    /** @brief One bounded diagnostic retaining identity and version evidence, not payload or executable bindings. */
    struct ReplicationDeclarationDiagnostic final {
        std::size_t declarationIndex{};
        ReplicationSchemaId schema;
        std::optional<FieldId> field;
        ReplicationSchemaVersion expectedVersion;
        std::optional<ReplicationSchemaVersion> registeredVersion;
        ReplicationDeclarationProblem problem{ReplicationDeclarationProblem::Count};
        bool blocking{true}; /**< False only for a valid explicitly optional declaration that will be omitted. */
        ModuleId owner;      /**< Authored declaring module retained even when that module is unavailable. */
    };

    /** @brief Complete finite inspection envelope; diagnostic overflow is explicit and always denies admission. */
    struct ReplicationDeclarationPolicyLimits final {
        ReplicationDescriptorLimits descriptors;
        std::size_t maximumModules{};
        std::size_t maximumDiagnostics{};
    };

    /**
     * @brief Immutable inspection projection; only matching registered schemas can enter its admitted identity set.
     *
     * Opaque declarations are copied only for editor inspection and expose no serializer or apply adapter.
     * Callers must preserve unknown authored payload separately, byte-for-byte; this policy never decodes it.
     * The result is evidence for the host admission boundary, not a session or gameplay authority grant.
     */
    class ReplicationDeclarationAssessment final {
    public:
        /** @brief Reports whether this non-editor operation passed complete required-declaration validation. @return Admission result. */
        [[nodiscard]] bool Admitted() const noexcept;
        /** @brief Returns all diagnostic evidence in declaration/field order. @return Borrow valid while this assessment lives. */
        [[nodiscard]] std::span<const ReplicationDeclarationDiagnostic> Diagnostics() const noexcept;
        /** @brief Reports truncated diagnostics explicitly; overflow always denies admission. @return True when the limit was reached. */
        [[nodiscard]] bool DiagnosticsOverflowed() const noexcept;
        /** @brief Returns registered compatible schema identities only. @return Empty for inspection or rejected operations. */
        [[nodiscard]] std::span<const ReplicationSchemaId> AdmittedSchemas() const noexcept;
        /** @brief Returns unresolved inert declarations for editor display only. @return Owned metadata without runtime bindings. */
        [[nodiscard]] std::span<const ReplicationDeclaration> OpaqueDeclarations() const noexcept;
        /**
         * @brief Projects every blocking diagnostic to a typed host failure.
         * @return Success for an admitted operation; otherwise a complete safe diagnostic list.
         */
        [[nodiscard]] Result<void> RequireAdmission() const;

    private:
        friend Result<ReplicationDeclarationAssessment> AssessReplicationDeclarations(std::span<const ReplicationDeclaration>,
                                                                                      const ReplicationDescriptorSnapshotPtr &,
                                                                                      std::span<const ModuleId>, ReplicationDeclarationUse,
                                                                                      const ReplicationDeclarationPolicyLimits &);
        ReplicationDeclarationAssessment() = default;

        ReplicationDeclarationUse use_{ReplicationDeclarationUse::Count};
        std::vector<ReplicationDeclarationDiagnostic> diagnostics_;
        std::vector<ReplicationSchemaId> admittedSchemas_;
        std::vector<ReplicationDeclaration> opaqueDeclarations_;
        bool diagnosticsOverflowed_{};
    };

    /**
     * @brief Inspects the complete authored inventory against one immutable registered generation before activation.
     * @param declarations Complete host-authored requirements, including declarations whose module is unavailable.
     * @param registered Pinned installed schema generation; null explicitly means no schemas are registered.
     * @param availableModules Complete successfully loaded module identities; not inferred from authored declarations.
     * @param use Explicit operation; editor inspection preserves inert metadata but cannot authorize activation.
     * @param limits Finite descriptor, module and diagnostic bounds checked before storage allocation.
     * @return Complete assessment or malformed/capacity error; no partial admission is published.
     */
    [[nodiscard]] Result<ReplicationDeclarationAssessment> AssessReplicationDeclarations(
        std::span<const ReplicationDeclaration> declarations, const ReplicationDescriptorSnapshotPtr &registered,
        std::span<const ModuleId> availableModules, ReplicationDeclarationUse use, const ReplicationDeclarationPolicyLimits &limits);
}  // namespace Horo::Network
