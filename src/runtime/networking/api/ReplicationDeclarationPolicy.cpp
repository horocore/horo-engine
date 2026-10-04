#include "Horo/Network/ReplicationDeclarationPolicy.h"

#include <algorithm>
#include <format>
#include <new>

namespace Horo::Network {
    namespace {
        /** @brief Matches field semantics by stable identity, never by input order or native layout. */
        [[nodiscard]] const ReplicationFieldDescriptor *FindField(const ReplicationSchemaDescriptor &schema, const FieldId id) noexcept {
            const auto found = std::ranges::find(schema.fields, id, &ReplicationFieldDescriptor::id);
            return found == schema.fields.end() ? nullptr : std::to_address(found);
        }

        /** @brief Collects all incompatible fields, including required omissions and undeclared semantic changes. */
        [[nodiscard]] std::vector<FieldId> IncompatibleFields(const ReplicationSchemaDescriptor &expected,
                                                              const ReplicationSchemaDescriptor &registered) {
            std::vector<FieldId> result;
            for (const auto &field : expected.fields) {
                const auto *actual = FindField(registered, field.id);
                if (actual != nullptr
                        ? *actual != field
                        : registered.version == expected.version || field.requirement == ReplicationFieldRequirement::Required ||
                              std::ranges::find(registered.tombstonedFields, field.id) == registered.tombstonedFields.end())
                    result.push_back(field.id);
            }
            for (const auto &field : registered.fields) {
                if (FindField(expected, field.id) == nullptr && (field.requirement != ReplicationFieldRequirement::Optional ||
                                                                 !field.canonicalDefault || field.introducedVersion <= expected.version))
                    result.push_back(field.id);
            }
            for (const auto id : expected.tombstonedFields) {
                if (std::ranges::find(registered.tombstonedFields, id) == registered.tombstonedFields.end())
                    result.push_back(id);
            }
            if (registered.version == expected.version)
                for (const auto id : registered.tombstonedFields)
                    if (std::ranges::find(expected.tombstonedFields, id) == expected.tombstonedFields.end())
                        result.push_back(id);
            std::ranges::sort(result);
            result.erase(std::unique(result.begin(), result.end()), result.end());
            return result;
        }

        /** @brief One declaration's complete incompatibility evidence before optional omission is considered. */
        struct DeclarationProblems final {
            std::vector<ReplicationDeclarationDiagnostic> diagnostics;
            std::size_t maximumDiagnostics{};
            bool malformed{};
            bool found{};
            bool overflowed{};

            /** @brief Retains only the bounded deterministic prefix while recording every omitted failure. */
            void Add(const ReplicationDeclaration &declaration, const std::size_t index, const ReplicationDeclarationProblem problem,
                     const std::optional<FieldId> field = {}, const std::optional<ReplicationSchemaVersion> version = {}) {
                found = true;
                if (diagnostics.size() == maximumDiagnostics) {
                    overflowed = true;
                    return;
                }
                const auto &expected = declaration.schema;
                diagnostics.push_back(
                    {index, expected.id, field, expected.version, version, problem, true, malformed ? ModuleId{} : expected.owner});
            }
        };

        /** @brief Checks one inert declaration against actual installed module and schema evidence. */
        [[nodiscard]] DeclarationProblems Inspect(const ReplicationDeclaration &declaration, const std::size_t index,
                                                  const ReplicationDescriptorSnapshotPtr &registered,
                                                  const std::span<const ModuleId> modules, const ReplicationDescriptorLimits &limits,
                                                  const std::size_t maximumDiagnostics) {
            DeclarationProblems result;
            result.maximumDiagnostics = maximumDiagnostics;
            const auto &expected = declaration.schema;
            const auto add = [&](const ReplicationDeclarationProblem problem, const std::optional<FieldId> field = {},
                                 const std::optional<ReplicationSchemaVersion> version = {}) {
                result.Add(declaration, index, problem, field, version);
            };
            if (declaration.requirement >= ReplicationDeclarationRequirement::Count ||
                ValidateReplicationSchemaDescriptor(expected, limits).HasError()) {
                result.malformed = true;
                add(ReplicationDeclarationProblem::MalformedDeclaration);
                return result;
            }
            if (std::ranges::find(modules, expected.owner) == modules.end()) {
                add(ReplicationDeclarationProblem::MissingModule);
                return result;
            }
            const ReplicationSchemaDescriptor *actual = nullptr;
            if (registered != nullptr) {
                const auto found = registered->Find(expected.id);
                if (found.HasValue())
                    actual = found.Value();
            }
            if (actual == nullptr) {
                add(ReplicationDeclarationProblem::UnknownSchema);
                return result;
            }
            if (ValidateReplicationSchemaDescriptor(*actual, limits).HasError()) {
                add(ReplicationDeclarationProblem::MalformedDeclaration, {}, actual->version);
                result.malformed = true;
                return result;
            }
            if (actual->owner != expected.owner)
                add(ReplicationDeclarationProblem::OwnerMismatch, {}, actual->version);
            // A newer additive schema can receive an older projection only under its explicit interval.
            if (actual->version < expected.version || !actual->compatibility.Contains(expected.version) ||
                (actual->version == expected.version && actual->compatibility != expected.compatibility))
                add(ReplicationDeclarationProblem::VersionIncompatible, {}, actual->version);
            for (const auto field : IncompatibleFields(expected, *actual))
                add(ReplicationDeclarationProblem::FieldIncompatible, field, actual->version);
            return result;
        }

        /** @brief Safe closed text for local diagnostic presentation; no remote input or payload is embedded. */
        [[nodiscard]] const char *ProblemText(const ReplicationDeclarationProblem problem) noexcept {
            using enum ReplicationDeclarationProblem;
            switch (problem) {
                case MalformedDeclaration:
                    return "malformed declaration";
                case DuplicateSchema:
                    return "duplicate schema identity";
                case MissingModule:
                    return "declaring module unavailable";
                case UnknownSchema:
                    return "schema not registered";
                case OwnerMismatch:
                    return "declaring owner mismatch";
                case VersionIncompatible:
                    return "incompatible schema version";
                case FieldIncompatible:
                    return "incompatible field semantics";
                case Count:
                    return "invalid policy operation";
            }
            return "invalid policy operation";
        }
    }  // namespace

    /** @copydoc ReplicationDeclarationAssessment::Admitted */
    bool ReplicationDeclarationAssessment::Admitted() const noexcept {
        return use_ != ReplicationDeclarationUse::EditorInspection && use_ < ReplicationDeclarationUse::Count && !diagnosticsOverflowed_ &&
               std::ranges::none_of(diagnostics_, &ReplicationDeclarationDiagnostic::blocking);
    }

    /** @copydoc ReplicationDeclarationAssessment::Diagnostics */
    std::span<const ReplicationDeclarationDiagnostic> ReplicationDeclarationAssessment::Diagnostics() const noexcept {
        return diagnostics_;
    }

    /** @copydoc ReplicationDeclarationAssessment::DiagnosticsOverflowed */
    bool ReplicationDeclarationAssessment::DiagnosticsOverflowed() const noexcept {
        return diagnosticsOverflowed_;
    }

    /** @copydoc ReplicationDeclarationAssessment::AdmittedSchemas */
    std::span<const ReplicationSchemaId> ReplicationDeclarationAssessment::AdmittedSchemas() const noexcept {
        return admittedSchemas_;
    }

    /** @copydoc ReplicationDeclarationAssessment::OpaqueDeclarations */
    std::span<const ReplicationDeclaration> ReplicationDeclarationAssessment::OpaqueDeclarations() const noexcept {
        return opaqueDeclarations_;
    }

    /** @copydoc ReplicationDeclarationAssessment::RequireAdmission */
    Result<void> ReplicationDeclarationAssessment::RequireAdmission() const {
        if (Admitted())
            return Result<void>::Success();
        Error error = MakeError(NetworkErrors::ReplicationDescriptorIncompatible);
        if (diagnosticsOverflowed_)
            error.diagnostics.push_back({.code = DiagnosticCode{"replication.declaration.diagnostic_overflow"},
                                         .severity = DiagnosticSeverity::Error,
                                         .message = "Replication declaration diagnostic capacity was exceeded; admission is denied."});
        for (const auto &diagnostic : diagnostics_) {
            if (!diagnostic.blocking)
                continue;
            error.diagnostics.push_back(
                {.code = DiagnosticCode{"replication.declaration.incompatible"},
                 .severity = DiagnosticSeverity::Error,
                 .message = std::format("Module '{}', schema {}: {} (expected {}.{}, registered {}).", diagnostic.owner.value,
                                        diagnostic.schema.Value(), ProblemText(diagnostic.problem), diagnostic.expectedVersion.major,
                                        diagnostic.expectedVersion.minor,
                                        diagnostic.registeredVersion
                                            ? std::format("{}.{}", diagnostic.registeredVersion->major, diagnostic.registeredVersion->minor)
                                            : "unavailable"),
                 .path = diagnostic.field
                             ? std::format("replication[{}].fields[{}]", diagnostic.declarationIndex, diagnostic.field->Value())
                             : std::format("replication[{}]", diagnostic.declarationIndex)});
        }
        return Result<void>::Failure(std::move(error));
    }

    /** @copydoc AssessReplicationDeclarations */
    Result<ReplicationDeclarationAssessment> AssessReplicationDeclarations(const std::span<const ReplicationDeclaration> declarations,
                                                                           const ReplicationDescriptorSnapshotPtr &registered,
                                                                           const std::span<const ModuleId> availableModules,
                                                                           const ReplicationDeclarationUse use,
                                                                           const ReplicationDeclarationPolicyLimits &limits) {
        if (use >= ReplicationDeclarationUse::Count || limits.maximumModules == 0 || limits.maximumDiagnostics == 0 ||
            limits.descriptors.maximumSchemas == 0 || limits.descriptors.maximumFieldsPerSchema == 0 ||
            limits.descriptors.maximumOwnerIdentityBytes == 0 || limits.descriptors.maximumDefaultBytesPerField == 0 ||
            limits.descriptors.maximumTotalDefaultBytes == 0)
            return Result<ReplicationDeclarationAssessment>::Failure(MakeError(NetworkErrors::ReplicationDescriptorInvalid));
        if (declarations.size() > limits.descriptors.maximumSchemas || availableModules.size() > limits.maximumModules ||
            (registered != nullptr && registered->Schemas().size() > limits.descriptors.maximumSchemas))
            return Result<ReplicationDeclarationAssessment>::Failure(MakeError(NetworkErrors::ReplicationCapacityExceeded));
        if (std::ranges::any_of(availableModules, [&](const ModuleId &module) {
            return module.value.empty() || module.value.size() > limits.descriptors.maximumOwnerIdentityBytes;
        }))
            return Result<ReplicationDeclarationAssessment>::Failure(MakeError(NetworkErrors::ReplicationDescriptorInvalid));
        try {
            ReplicationDeclarationAssessment assessment;
            assessment.use_ = use;
            std::size_t defaultBytes{};
            for (std::size_t index = 0; index < declarations.size(); ++index) {
                const auto &declaration = declarations[index];
                const auto remaining = limits.maximumDiagnostics - assessment.diagnostics_.size();
                auto problems = Inspect(declaration, index, registered, availableModules, limits.descriptors, remaining);
                const auto duplicate = std::ranges::count(declarations, declaration.schema.id, [](const auto &value) {
                    return value.schema.id;
                }) > 1;
                if (duplicate) {
                    problems.Add(declaration, index, ReplicationDeclarationProblem::DuplicateSchema);
                    problems.malformed = true;
                }
                if (!problems.malformed) {
                    for (const auto &field : declaration.schema.fields) {
                        const auto bytes = field.canonicalDefault ? field.canonicalDefault->canonicalBytes.size() : 0;
                        if (bytes > limits.descriptors.maximumTotalDefaultBytes - defaultBytes)
                            return Result<ReplicationDeclarationAssessment>::Failure(MakeError(NetworkErrors::ReplicationCapacityExceeded));
                        defaultBytes += bytes;
                    }
                    if (declaration.requirement == ReplicationDeclarationRequirement::Optional)
                        for (auto &diagnostic : problems.diagnostics)
                            diagnostic.blocking = false;
                    if (!problems.found)
                        assessment.admittedSchemas_.push_back(declaration.schema.id);
                    else if (use == ReplicationDeclarationUse::EditorInspection)
                        assessment.opaqueDeclarations_.push_back(declaration);
                }
                assessment.diagnosticsOverflowed_ |= problems.overflowed;
                assessment.diagnostics_.insert(assessment.diagnostics_.end(), problems.diagnostics.begin(), problems.diagnostics.end());
            }
            if (!assessment.Admitted())
                assessment.admittedSchemas_.clear();
            else
                std::ranges::sort(assessment.admittedSchemas_);
            return Result<ReplicationDeclarationAssessment>::Success(std::move(assessment));
        } catch (const std::bad_alloc &) {
            return Result<ReplicationDeclarationAssessment>::Failure(MakeError(NetworkErrors::ReplicationCapacityExceeded));
        }
    }
}  // namespace Horo::Network
