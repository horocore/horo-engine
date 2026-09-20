#include "Horo/Prefab/PrefabDiagnostics.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <string_view>
#include <utility>

namespace Horo::Prefab {
    namespace {
        using enum PrefabDiagnosticOrigin;

        [[nodiscard]] std::optional<PrefabDiagnosticOrigin> OriginForDomain(const std::string_view domain) noexcept {
            if (domain == "horo.prefab")
                return Prefab;
            if (domain == "horo.asset")
                return Asset;
            if (domain == "horo.scene")
                return Scene;
            if (domain == "horo.gameplay")
                return Gameplay;
            if (domain == "horo.application.project" || domain == "horo.project" || domain == "horo.migration")
                return Migration;
            return std::nullopt;
        }

        [[nodiscard]] bool IsKnownPrefabCode(const std::string_view code) noexcept {
            return std::ranges::any_of(PrefabDiagnosticErrorDescriptors(), [code](const ErrorCodeDescriptor *descriptor) {
                return descriptor != nullptr && descriptor->code.Value() == code;
            });
        }

        [[nodiscard]] bool HasExpectedForeignPrefix(const PrefabDiagnosticOrigin origin, const std::string_view code) noexcept {
            switch (origin) {
                case Asset:
                    return code.starts_with("asset.");
                case Scene:
                    return code.starts_with("scene.");
                case Gameplay:
                    return code.starts_with("gameplay.");
                case Migration:
                    return code.starts_with("project.") || code.starts_with("migration.");
                case Prefab:
                case Count:
                    return false;
            }
            return false;
        }

        [[nodiscard]] bool IsValidErrorIdentity(const Error &error, const PrefabDiagnosticOrigin origin) noexcept {
            const std::string_view domain = error.domain.Value();
            const std::string_view code = error.code.Value();
            if (domain.empty() || code.empty() || domain.size() > MaximumPrefabDiagnosticIdentityBytes ||
                code.size() > MaximumPrefabDiagnosticIdentityBytes)
                return false;
            if (origin == Prefab)
                return IsKnownPrefabCode(code);
            return HasExpectedForeignPrefix(origin, code);
        }

        [[nodiscard]] bool IsValidSourceRevision(const PrefabSourceRevision &revision) noexcept {
            const bool hasVersion = revision.projectVersion.major != 0 || revision.projectVersion.minor != 0 ||
                                    revision.projectVersion.patch != 0 || !revision.projectVersion.prerelease.empty();
            const bool hasDigest = std::ranges::any_of(revision.contentDigest.bytes, [](const std::uint8_t byte) {
                return byte != 0;
            });
            return hasVersion && hasDigest;
        }

        [[nodiscard]] bool IsValidSourceLocation(const std::optional<DiagnosticSourceLocation> &source) noexcept {
            if (!source.has_value())
                return true;
            if (source->absolutePath.empty() || source->absolutePath.size() > MaximumPrefabDiagnosticSourcePathBytes)
                return false;
            return source->line != 0 || source->column == 0;
        }

        [[nodiscard]] bool IsValidObjectAddress(const PrefabObjectAddress &address) noexcept {
            const auto scope = address.NestedInstanceScope();
            return scope.size() <= MaximumPrefabObjectScopeDepth && std::ranges::all_of(scope, [](const LocalObjectId object) {
                return !object.IsRoot();
            });
        }

        [[nodiscard]] bool IsValidContext(const PrefabDiagnosticContext &context) noexcept {
            if (!context.prefabAsset.IsValid() || !IsValidSourceRevision(context.revision))
                return false;
            if (context.instance.has_value() && !context.instance->IsValid())
                return false;
            if (context.localMember.has_value() && !IsValidObjectAddress(*context.localMember))
                return false;
            if (context.property.has_value()) {
                if (!context.localMember.has_value() || context.property->Object() != *context.localMember ||
                    !context.property->ComponentType().IsValid() || !context.property->ComponentInstance().IsValid() ||
                    !context.property->Property().IsValid())
                    return false;
            }
            if (context.operationId.has_value() && *context.operationId == 0)
                return false;
            return IsValidSourceLocation(context.source);
        }

        [[nodiscard]] bool IsValidDependencyKind(const PrefabDependencyKind kind) noexcept {
            switch (kind) {
                case PrefabDependencyKind::Resource:
                case PrefabDependencyKind::NestedPrefab:
                case PrefabDependencyKind::VariantParent:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsValidDependencyChain(const std::span<const PrefabDiagnosticDependency> chain) noexcept {
            if (chain.size() > MaximumPrefabDiagnosticDependencyDepth)
                return false;
            for (const PrefabDiagnosticDependency &dependency : chain) {
                if (!dependency.asset.IsValid() || !IsValidDependencyKind(dependency.kind) ||
                    (dependency.revision.has_value() && !IsValidSourceRevision(*dependency.revision)))
                    return false;
            }
            return true;
        }

        [[nodiscard]] Result<PrefabDiagnosticRecord> InvalidDiagnostic(const ErrorCodeDescriptor &descriptor, const char *message) {
            return Result<PrefabDiagnosticRecord>::Failure(MakeError(descriptor, message));
        }

        [[nodiscard]] Result<PrefabDiagnosticRecord> ValidateErrorAndFillRecord(const Error &error, PrefabDiagnosticRecord &record) {
            if (error.message.empty())
                return InvalidDiagnostic(PrefabErrors::DiagnosticInvalid, "Prefab diagnostic message is empty.");
            if (error.message.size() > MaximumPrefabDiagnosticMessageBytes)
                return InvalidDiagnostic(PrefabErrors::DiagnosticBudgetExceeded, "Prefab diagnostic message exceeds its bound.");

            const auto severity = DiagnosticSeverityForError(error.severity);
            if (!severity.has_value())
                return InvalidDiagnostic(PrefabErrors::DiagnosticInvalid, "Prefab diagnostic severity is unknown.");

            const Error *current = &error;
            bool first = true;
            while (current != nullptr) {
                const auto origin = OriginForDomain(current->domain.Value());
                if (!origin.has_value())
                    return InvalidDiagnostic(PrefabErrors::DiagnosticUnsupported,
                                             "Prefab diagnostic contains an unsupported originating error domain.");
                if (!IsValidErrorIdentity(*current, *origin))
                    return InvalidDiagnostic(PrefabErrors::DiagnosticUnsupported,
                                             "Prefab diagnostic contains an unknown or malformed originating error identity.");
                const auto currentSeverity = DiagnosticSeverityForError(current->severity);
                if (!currentSeverity.has_value())
                    return InvalidDiagnostic(PrefabErrors::DiagnosticInvalid, "Prefab diagnostic cause severity is unknown.");

                if (first) {
                    record.origin = *origin;
                    record.domain = ErrorDomainId{std::string{current->domain.Value()}};
                    record.code = DiagnosticCode{std::string{current->code.Value()}};
                    record.severity = *severity;
                    record.message = error.message;
                    first = false;
                } else {
                    if (record.causeCount >= MaximumPrefabDiagnosticCauseDepth)
                        return InvalidDiagnostic(PrefabErrors::DiagnosticBudgetExceeded,
                                                 "Prefab diagnostic cause chain exceeds its bound.");
                    record.causes[record.causeCount++] = {
                        .domain = ErrorDomainId{std::string{current->domain.Value()}},
                        .code = DiagnosticCode{std::string{current->code.Value()}},
                        .origin = *origin,
                        .severity = *currentSeverity,
                    };
                }
                current = current->cause.Get();
            }
            return Result<PrefabDiagnosticRecord>::Success(std::move(record));
        }

        [[nodiscard]] Result<PrefabDiagnosticRecord> MakeRecord(const PrefabDiagnosticRecordInput &input, const Error &error) {
            if (PrefabDiagnosticCategoryName(input.category).empty() || PrefabDiagnosticOperationName(input.operation).empty())
                return InvalidDiagnostic(PrefabErrors::DiagnosticUnsupported, "Prefab diagnostic category or operation is unknown.");
            if (!IsValidContext(input.context))
                return InvalidDiagnostic(PrefabErrors::DiagnosticInvalid, "Prefab diagnostic context is malformed.");
            if (input.dependencyChain.size() > MaximumPrefabDiagnosticDependencyDepth)
                return InvalidDiagnostic(PrefabErrors::DiagnosticBudgetExceeded, "Prefab diagnostic dependency chain exceeds its bound.");
            if (!IsValidDependencyChain(input.dependencyChain))
                return InvalidDiagnostic(PrefabErrors::DiagnosticInvalid, "Prefab diagnostic dependency chain is malformed.");

            PrefabDiagnosticRecord record;
            record.category = input.category;
            record.operation = input.operation;
            record.prefabAsset = input.context.prefabAsset;
            record.revision = input.context.revision;
            record.registryRevision = input.context.registryRevision;
            record.instance = input.context.instance;
            record.localMember = input.context.localMember;
            record.property = input.context.property;
            record.operationId = input.context.operationId;
            record.source = input.context.source;
            record.dependencyCount = static_cast<std::uint8_t>(input.dependencyChain.size());
            std::ranges::copy(input.dependencyChain, record.dependencyChain.begin());
            return ValidateErrorAndFillRecord(error, record);
        }
    }  // namespace

    /** @copydoc PrefabDiagnosticCategoryName */
    std::string_view PrefabDiagnosticCategoryName(const PrefabDiagnosticCategory category) noexcept {
        using enum PrefabDiagnosticCategory;
        switch (category) {
            case Source:
                return "prefab.source";
            case Graph:
                return "prefab.graph";
            case Expansion:
                return "prefab.expansion";
            case Override:
                return "prefab.override";
            case Cook:
                return "prefab.cook";
            case Migration:
                return "prefab.migration";
            case RuntimeSpawn:
                return "prefab.runtime_spawn";
            case Lifecycle:
                return "prefab.lifecycle";
            case Count:
                break;
        }
        return {};
    }

    /** @copydoc PrefabDiagnosticOperationName */
    std::string_view PrefabDiagnosticOperationName(const PrefabDiagnosticOperation operation) noexcept {
        using enum PrefabDiagnosticOperation;
        switch (operation) {
            case Parse:
                return "parse";
            case ValidateSource:
                return "validate_source";
            case BuildGraph:
                return "build_graph";
            case Expand:
                return "expand";
            case ApplyOverride:
                return "apply_override";
            case Cook:
                return "cook";
            case Migrate:
                return "migrate";
            case Load:
                return "load";
            case Spawn:
                return "spawn";
            case Commit:
                return "commit";
            case Publish:
                return "publish";
            case Count:
                break;
        }
        return {};
    }

    /** @copydoc PrefabDiagnosticErrorDescriptors */
    std::span<const ErrorCodeDescriptor *const> PrefabDiagnosticErrorDescriptors() noexcept {
        return PrefabErrors::Descriptors();
    }

    /** @copydoc MakePrefabDiagnosticRecord */
    Result<PrefabDiagnosticRecord> MakePrefabDiagnosticRecord(const PrefabDiagnosticRecordInput &input, const Error &error) {
        return MakeRecord(input, error);
    }

    /** @copydoc MakePrefabDiagnosticRecord */
    Result<PrefabDiagnosticRecord> MakePrefabDiagnosticRecord(const PrefabDiagnosticCategory category, const Error &error,
                                                              const PrefabDiagnosticContext &context,
                                                              const PrefabDiagnosticOperation operation,
                                                              const std::span<const PrefabDiagnosticDependency> dependencyChain) {
        return MakePrefabDiagnosticRecord(
            PrefabDiagnosticRecordInput{
                .category = category,
                .operation = operation,
                .context = context,
                .dependencyChain = dependencyChain,
            },
            error);
    }

    /** @copydoc MakePrefabDiagnosticRecord */
    Result<PrefabDiagnosticRecord> MakePrefabDiagnosticRecord(const PrefabDiagnosticCategory category,
                                                              const PrefabDiagnosticOperation operation, const Error &error,
                                                              const PrefabDiagnosticContext &context,
                                                              const std::span<const PrefabDiagnosticDependency> dependencyChain) {
        return MakePrefabDiagnosticRecord(category, error, context, operation, dependencyChain);
    }

    /** @copydoc MakePrefabBuildOutputRecord */
    BuildOutputRecord MakePrefabBuildOutputRecord(const PrefabDiagnosticRecord &record, const BuildOutputResult result) {
        return BuildOutputRecord{
            .timestampUtc = std::chrono::system_clock::now(),
            .operationId = record.operationId,
            .severity = record.severity,
            .result = result,
            .stage = std::string{PrefabDiagnosticOperationName(record.operation)},
            .code = record.code,
            .message = record.message,
            .source = record.source,
        };
    }
}  // namespace Horo::Prefab
