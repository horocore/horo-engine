#include "ScriptExportDescriptorInternal.h"

#include <array>
#include <new>
#include <set>

namespace Horo::Extensions::Detail {
    namespace {
        Result<void> ValidateFunctionHeader(const ScriptExportFunctionDescriptor &function, const ScriptExportDescriptor &descriptor,
                                            const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes) {
            if (!IsCanonicalScriptExportIdentity(function.id, limits.maximumIdentityBytes))
                return ScriptExportDescriptorInvalid<void>("Function identity is not canonical.");
            if (!AddScriptExportBytes(totalBytes, function.id.size(), limits.maximumDescriptorBytes))
                return ScriptExportDescriptorCapacity<void>("Function identities exceed the descriptor bound.");
            if (const Result<void> introduction = ValidateScriptExportVersionIntroduction(function.introducedVersion, descriptor.version);
                introduction.HasError())
                return introduction;
            if (!IsKnownInvocationMode(function.invocation))
                return ScriptExportDescriptorInvalid<void>("Function invocation mode is unsupported.");
            if (function.parameters.size() > limits.maximumParametersPerFunction ||
                function.results.size() > limits.maximumResultsPerFunction || function.errorIds.size() > limits.maximumErrorsPerFunction)
                return ScriptExportDescriptorCapacity<void>("Function signature exceeds the configured bound.");
            return ValidateScriptExportDocumentation(function.documentation, totalBytes, limits);
        }

        Result<void> ValidateFunctionElementIdentity(const std::string_view functionId, const std::string_view elementId,
                                                     const std::string_view identityMessage, const std::string_view scopeMessage,
                                                     std::set<std::string, std::less<>> &localIds,
                                                     std::set<std::string, std::less<>> &activeSymbols,
                                                     const ScriptExportDescriptorLimits &limits) {
            if (!IsCanonicalScriptExportIdentity(elementId, limits.maximumIdentityBytes))
                return ScriptExportDescriptorInvalid<void>(identityMessage);
            if (!IsBoundedScriptExportScopedIdentity(functionId, elementId, limits.maximumIdentityBytes))
                return ScriptExportDescriptorCapacity<void>(scopeMessage);
            if (!localIds.emplace(elementId).second)
                return ScriptExportDescriptorConflict<void>("Function parameter and result identities must be unique.");
            if (!activeSymbols.insert(MakeScopedScriptExportIdentity(functionId, elementId)).second)
                return ScriptExportDescriptorConflict<void>("Script export symbol identities must be unique.");
            return Result<void>::Success();
        }

        struct FunctionValidationContext final {
            const ScriptExportFunctionDescriptor &function;
            const ScriptExportDescriptor &descriptor;
            const std::set<std::string, std::less<>> &namedTypes;
            std::set<std::string, std::less<>> &localIds;
            std::set<std::string, std::less<>> &activeSymbols;
            const ScriptExportDescriptorLimits &limits;
            std::size_t &totalBytes;
        };

        template <typename Element, typename Validator>
        Result<void> ValidateFunctionElements(const std::vector<Element> &elements, const Validator &validator) {
            for (const Element &element : elements) {
                if (const Result<void> valid = validator(element); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }

        Result<void> ValidateFunctionParameter(const FunctionValidationContext &context, const ScriptExportParameterDescriptor &parameter) {
            if (const Result<void> identity =
                    ValidateFunctionElementIdentity(context.function.id, parameter.id, "Function parameter identity is not canonical.",
                                                    "Qualified function parameter identity exceeds the configured bound.", context.localIds,
                                                    context.activeSymbols, context.limits);
                identity.HasError())
                return identity;
            return ValidateScriptExportParameter(parameter, context.descriptor.version, context.namedTypes, context.limits,
                                                 context.totalBytes);
        }

        Result<void> ValidateFunctionParameters(const FunctionValidationContext &context) {
            return ValidateFunctionElements(context.function.parameters, [&context](const ScriptExportParameterDescriptor &parameter) {
                return ValidateFunctionParameter(context, parameter);
            });
        }

        Result<void> ValidateFunctionResult(const FunctionValidationContext &context, const ScriptExportResultDescriptor &result) {
            if (const Result<void> identity =
                    ValidateFunctionElementIdentity(context.function.id, result.id, "Function result identity is not canonical.",
                                                    "Qualified function result identity exceeds the configured bound.", context.localIds,
                                                    context.activeSymbols, context.limits);
                identity.HasError())
                return identity;
            return ValidateScriptExportResult(result, context.descriptor.version, context.namedTypes, context.limits, context.totalBytes);
        }

        Result<void> ValidateFunctionResults(const FunctionValidationContext &context) {
            return ValidateFunctionElements(context.function.results, [&context](const ScriptExportResultDescriptor &result) {
                return ValidateFunctionResult(context, result);
            });
        }

        Result<void> ValidateFunctionErrors(const ScriptExportFunctionDescriptor &function,
                                            const std::set<std::string, std::less<>> &errorIds, const ScriptExportDescriptorLimits &limits,
                                            std::size_t &totalBytes) {
            std::set<std::string, std::less<>> referencedErrors;
            for (const std::string &errorId : function.errorIds) {
                if (!IsCanonicalScriptExportIdentity(errorId, limits.maximumIdentityBytes))
                    return ScriptExportDescriptorInvalid<void>("Function error identity is not canonical.");
                if (!referencedErrors.insert(errorId).second)
                    return ScriptExportDescriptorConflict<void>("Function error identities must be unique.");
                if (!errorIds.contains(errorId))
                    return ScriptExportDescriptorInvalid<void>("Function references an unknown script export error.");
                if (!AddScriptExportBytes(totalBytes, errorId.size(), limits.maximumDescriptorBytes))
                    return ScriptExportDescriptorCapacity<void>("Function error references exceed the descriptor bound.");
            }
            return Result<void>::Success();
        }

        bool AddScriptExportSymbolCount(std::size_t &symbolCount, const std::size_t count, const std::size_t maximum) noexcept {
            if (count > maximum || symbolCount > maximum - count)
                return false;
            symbolCount += count;
            return true;
        }

        Result<void> ValidateScriptExportFunctionBody(const ScriptExportFunctionDescriptor &function,
                                                      const ScriptExportDescriptor &descriptor,
                                                      const std::set<std::string, std::less<>> &namedTypes,
                                                      const std::set<std::string, std::less<>> &errorIds,
                                                      std::set<std::string, std::less<>> &activeSymbols,
                                                      const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes) {
            std::set<std::string, std::less<>> localIds;
            const FunctionValidationContext context{function, descriptor, namedTypes, localIds, activeSymbols, limits, totalBytes};
            if (const Result<void> parameters = ValidateFunctionParameters(context); parameters.HasError())
                return parameters;
            if (const Result<void> results = ValidateFunctionResults(context); results.HasError())
                return results;
            return ValidateFunctionErrors(function, errorIds, limits, totalBytes);
        }
    }  // namespace

    Result<void> ValidateScriptExportFunction(const ScriptExportFunctionDescriptor &function, const ScriptExportDescriptor &descriptor,
                                              const std::set<std::string, std::less<>> &namedTypes,
                                              const std::set<std::string, std::less<>> &errorIds,
                                              std::set<std::string, std::less<>> &activeSymbols, const ScriptExportDescriptorLimits &limits,
                                              std::size_t &totalBytes) {
        if (const Result<void> header = ValidateFunctionHeader(function, descriptor, limits, totalBytes); header.HasError())
            return header;
        return ValidateScriptExportFunctionBody(function, descriptor, namedTypes, errorIds, activeSymbols, limits, totalBytes);
    }

    Result<void> ValidateScriptExportSymbolCapacity(const ScriptExportDescriptor &descriptor, const ScriptExportDescriptorLimits &limits) {
        std::size_t symbolCount{};
        const std::array<std::size_t, 5> topLevelCounts{
            descriptor.types.size(),     descriptor.constants.size(),         descriptor.errors.size(),
            descriptor.functions.size(), descriptor.tombstonedSymbols.size(),
        };
        for (const std::size_t count : topLevelCounts) {
            if (!AddScriptExportSymbolCount(symbolCount, count, limits.maximumSymbols))
                return ScriptExportDescriptorCapacity<void>("Script API symbol count exceeds the configured bound.");
        }
        for (const ScriptExportTypeDescriptor &type : descriptor.types) {
            if (!AddScriptExportSymbolCount(symbolCount, type.fields.size(), limits.maximumSymbols) ||
                !AddScriptExportSymbolCount(symbolCount, type.enumValues.size(), limits.maximumSymbols))
                return ScriptExportDescriptorCapacity<void>("Script API symbol count exceeds the configured bound.");
        }
        for (const ScriptExportFunctionDescriptor &function : descriptor.functions) {
            if (!AddScriptExportSymbolCount(symbolCount, function.parameters.size(), limits.maximumSymbols) ||
                !AddScriptExportSymbolCount(symbolCount, function.results.size(), limits.maximumSymbols))
                return ScriptExportDescriptorCapacity<void>("Script API symbol count exceeds the configured bound.");
        }
        return Result<void>::Success();
    }

    namespace {
        Result<void> ValidateDescriptorIdentity(const std::string_view value, const std::string_view field,
                                                const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes) {
            if (!IsCanonicalScriptExportIdentity(value, limits.maximumIdentityBytes))
                return ScriptExportDescriptorInvalid<void>(field);
            if (!AddScriptExportBytes(totalBytes, value.size(), limits.maximumDescriptorBytes))
                return ScriptExportDescriptorCapacity<void>(field);
            return Result<void>::Success();
        }

        Result<void> ValidateDescriptorIdentities(const ScriptExportDescriptor &descriptor, const ScriptExportDescriptorLimits &limits,
                                                  std::size_t &totalBytes) {
            const std::array<std::pair<std::string_view, std::string_view>, 6> identities{
                std::pair{std::string_view{descriptor.moduleId}, std::string_view{"Module identity is not canonical."}},
                std::pair{std::string_view{descriptor.id}, std::string_view{"Script API identity is not canonical."}},
                std::pair{std::string_view{descriptor.nameSpace}, std::string_view{"Script API namespace is not canonical."}},
                std::pair{std::string_view{descriptor.service.capability.value}, std::string_view{"Service capability is not canonical."}},
                std::pair{std::string_view{descriptor.service.serviceId}, std::string_view{"Service identity is not canonical."}},
                std::pair{std::string_view{descriptor.service.contractId}, std::string_view{"Service contract identity is not canonical."}},
            };
            for (const auto &[value, field] : identities) {
                if (const Result<void> valid = ValidateDescriptorIdentity(value, field, limits, totalBytes); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }

        Result<void> ValidateDescriptorVersions(const ScriptExportDescriptor &descriptor) {
            if (!IsValidScriptExportVersion(descriptor.version) || !IsValidScriptExportVersion(descriptor.compatibility.minimum) ||
                !IsValidScriptExportVersion(descriptor.compatibility.maximum) || descriptor.compatibility.maximum != descriptor.version ||
                descriptor.compatibility.minimum.major != descriptor.version.major ||
                descriptor.compatibility.minimum > descriptor.compatibility.maximum)
                return ScriptExportDescriptorInvalid<void>("Script API version or compatibility range is malformed.");
            if (!IsValidScriptExportVersion(descriptor.service.minimumVersion))
                return ScriptExportDescriptorInvalid<void>("Service minimum version is malformed.");
            return Result<void>::Success();
        }

        Result<void> ValidateDescriptorHeader(const ScriptExportDescriptor &descriptor, const ScriptExportDescriptorLimits &limits,
                                              std::size_t &totalBytes) {
            if (const Result<void> identities = ValidateDescriptorIdentities(descriptor, limits, totalBytes); identities.HasError())
                return identities;
            if (const Result<void> versions = ValidateDescriptorVersions(descriptor); versions.HasError())
                return versions;
            return ValidateScriptExportDocumentation(descriptor.documentation, totalBytes, limits);
        }

        Result<void> ValidateDescriptorCounts(const ScriptExportDescriptor &descriptor, const ScriptExportDescriptorLimits &limits) {
            if (descriptor.types.size() > limits.maximumTypes || descriptor.functions.size() > limits.maximumFunctions ||
                descriptor.constants.size() > limits.maximumConstants || descriptor.errors.size() > limits.maximumErrors ||
                descriptor.tombstonedSymbols.size() > limits.maximumTombstones)
                return ScriptExportDescriptorCapacity<void>("Script API declaration count exceeds the configured bound.");
            return ValidateScriptExportSymbolCapacity(descriptor, limits);
        }

        Result<void> IndexTypeSymbols(const ScriptExportDescriptor &descriptor, std::set<std::string, std::less<>> &namedTypes,
                                      std::set<std::string, std::less<>> &activeSymbols) {
            for (const ScriptExportTypeDescriptor &type : descriptor.types) {
                if (!namedTypes.insert(type.id).second || !activeSymbols.insert(type.id).second)
                    return ScriptExportDescriptorConflict<void>("Script API type identities must be unique.");
            }
            return Result<void>::Success();
        }

        Result<void> IndexErrorSymbols(const ScriptExportDescriptor &descriptor, std::set<std::string, std::less<>> &errorIds,
                                       std::set<std::string, std::less<>> &activeSymbols) {
            for (const ScriptExportErrorDescriptor &error : descriptor.errors) {
                if (!errorIds.insert(error.id).second || !activeSymbols.insert(error.id).second)
                    return ScriptExportDescriptorConflict<void>("Script API error identities must be unique.");
            }
            return Result<void>::Success();
        }

        Result<void> IndexValueSymbols(const ScriptExportDescriptor &descriptor, std::set<std::string, std::less<>> &activeSymbols) {
            for (const ScriptExportConstantDescriptor &constant : descriptor.constants) {
                if (!activeSymbols.insert(constant.id).second)
                    return ScriptExportDescriptorConflict<void>("Script API constant identities must be unique.");
            }
            for (const ScriptExportFunctionDescriptor &function : descriptor.functions) {
                if (!activeSymbols.insert(function.id).second)
                    return ScriptExportDescriptorConflict<void>("Script API function identities must be unique.");
            }
            return Result<void>::Success();
        }

        Result<void> BuildDescriptorIndexes(const ScriptExportDescriptor &descriptor, std::set<std::string, std::less<>> &namedTypes,
                                            std::set<std::string, std::less<>> &errorIds,
                                            std::set<std::string, std::less<>> &activeSymbols) {
            if (const Result<void> types = IndexTypeSymbols(descriptor, namedTypes, activeSymbols); types.HasError())
                return types;
            if (const Result<void> errors = IndexErrorSymbols(descriptor, errorIds, activeSymbols); errors.HasError())
                return errors;
            return IndexValueSymbols(descriptor, activeSymbols);
        }

        Result<void> ValidateDescriptorDeclarations(const ScriptExportDescriptor &descriptor,
                                                    const std::set<std::string, std::less<>> &namedTypes,
                                                    const std::set<std::string, std::less<>> &errorIds,
                                                    std::set<std::string, std::less<>> &activeSymbols,
                                                    const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes) {
            for (const ScriptExportTypeDescriptor &type : descriptor.types) {
                if (const Result<void> valid =
                        ValidateScriptExportType(type, descriptor.version, namedTypes, activeSymbols, limits, totalBytes);
                    valid.HasError())
                    return valid;
            }
            for (const ScriptExportErrorDescriptor &error : descriptor.errors) {
                if (const Result<void> valid = ValidateScriptExportError(error, descriptor.version, limits, totalBytes); valid.HasError())
                    return valid;
            }
            for (const ScriptExportConstantDescriptor &constant : descriptor.constants) {
                if (const Result<void> valid = ValidateScriptExportConstant(constant, descriptor.version, namedTypes, limits, totalBytes);
                    valid.HasError())
                    return valid;
            }
            for (const ScriptExportFunctionDescriptor &function : descriptor.functions) {
                if (const Result<void> valid =
                        ValidateScriptExportFunction(function, descriptor, namedTypes, errorIds, activeSymbols, limits, totalBytes);
                    valid.HasError())
                    return valid;
            }
            return ValidateScriptExportTypeGraph(descriptor, limits);
        }

        Result<void> ValidateDescriptorTombstones(const ScriptExportDescriptor &descriptor,
                                                  const std::set<std::string, std::less<>> &activeSymbols,
                                                  const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes) {
            std::set<std::string, std::less<>> tombstones;
            for (const std::string &tombstone : descriptor.tombstonedSymbols) {
                if (!IsCanonicalScriptExportIdentity(tombstone, limits.maximumIdentityBytes))
                    return ScriptExportDescriptorInvalid<void>("Tombstoned symbol identity is not canonical.");
                if (!tombstones.insert(tombstone).second || activeSymbols.contains(tombstone))
                    return ScriptExportDescriptorConflict<void>("Tombstoned symbol identities must be unique and retired.");
                if (!AddScriptExportBytes(totalBytes, tombstone.size(), limits.maximumDescriptorBytes))
                    return ScriptExportDescriptorCapacity<void>("Tombstone identities exceed the descriptor bound.");
            }
            return Result<void>::Success();
        }
    }  // namespace

    Result<void> ValidateScriptExportDescriptorData(const ScriptExportDescriptor &descriptor, const ScriptExportDescriptorLimits &limits) {
        if (!IsValidScriptExportLimits(limits))
            return ScriptExportDescriptorInvalid<void>("Script export descriptor limits are malformed.");

        std::size_t totalBytes{};
        if (const Result<void> header = ValidateDescriptorHeader(descriptor, limits, totalBytes); header.HasError())
            return header;
        if (const Result<void> counts = ValidateDescriptorCounts(descriptor, limits); counts.HasError())
            return counts;

        std::set<std::string, std::less<>> namedTypes;
        std::set<std::string, std::less<>> errorIds;
        std::set<std::string, std::less<>> activeSymbols;
        if (const Result<void> indexes = BuildDescriptorIndexes(descriptor, namedTypes, errorIds, activeSymbols); indexes.HasError())
            return indexes;
        if (const Result<void> declarations =
                ValidateDescriptorDeclarations(descriptor, namedTypes, errorIds, activeSymbols, limits, totalBytes);
            declarations.HasError())
            return declarations;
        return ValidateDescriptorTombstones(descriptor, activeSymbols, limits, totalBytes);
    }
}  // namespace Horo::Extensions::Detail

namespace Horo::Extensions {
    /** @copydoc ValidateScriptExportDescriptor */
    Result<void> ValidateScriptExportDescriptor(const ScriptExportDescriptor &descriptor, const ScriptExportDescriptorLimits &limits) {
        try {
            return Detail::ValidateScriptExportDescriptorData(descriptor, limits);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptExportDescriptorCapacityExceeded));
        }
    }
}  // namespace Horo::Extensions
