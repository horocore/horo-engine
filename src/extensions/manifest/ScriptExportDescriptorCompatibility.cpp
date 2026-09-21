#include "ScriptExportDescriptorInternal.h"

#include <algorithm>
#include <ranges>

namespace Horo::Extensions::Detail {
    namespace {
        bool CompatiblePreviousFields(const ScriptExportTypeDescriptor &previous, const ScriptExportTypeDescriptor &candidate,
                                      const bool sameMajor, const ScriptExportDescriptor &candidateDescriptor) {
            return std::ranges::all_of(previous.fields, [&](const ScriptExportFieldDescriptor &field) {
                if (const ScriptExportFieldDescriptor *replacement = FindScriptExportField(candidate, field.id); replacement != nullptr)
                    return SameFieldShape(field, *replacement);
                return !sameMajor && HasTombstone(candidateDescriptor, MakeScopedScriptExportIdentity(previous.id, field.id));
            });
        }

        bool CompatibleAddedFields(const ScriptExportTypeDescriptor &previous, const ScriptExportTypeDescriptor &candidate,
                                   const bool sameMajor, const ScriptExportVersion &previousVersion) {
            return std::ranges::all_of(candidate.fields, [&](const ScriptExportFieldDescriptor &field) {
                if (FindScriptExportField(previous, field.id) != nullptr)
                    return true;
                return field.introducedVersion > previousVersion &&
                       (!sameMajor ||
                        (field.requirement == ScriptExportParameterRequirement::Optional && field.canonicalDefault.has_value()));
            });
        }

        bool CompatibleFields(const ScriptExportTypeDescriptor &previous, const ScriptExportTypeDescriptor &candidate, const bool sameMajor,
                              const ScriptExportVersion &previousVersion, const ScriptExportDescriptor &candidateDescriptor) {
            return CompatiblePreviousFields(previous, candidate, sameMajor, candidateDescriptor) &&
                   CompatibleAddedFields(previous, candidate, sameMajor, previousVersion);
        }

        bool CompatibleEnumValues(const ScriptExportTypeDescriptor &previous, const ScriptExportTypeDescriptor &candidate,
                                  const bool sameMajor, const ScriptExportVersion &previousVersion,
                                  const ScriptExportDescriptor &candidateDescriptor) {
            if (!std::ranges::all_of(previous.enumValues, [&](const ScriptExportEnumValueDescriptor &value) {
                if (const ScriptExportEnumValueDescriptor *replacement = FindScriptExportEnumValue(candidate, value.id);
                    replacement != nullptr)
                    return SameEnumValueShape(value, *replacement);
                return !sameMajor && HasTombstone(candidateDescriptor, MakeScopedScriptExportIdentity(previous.id, value.id));
            }))
                return false;
            return std::ranges::all_of(candidate.enumValues, [&](const ScriptExportEnumValueDescriptor &value) {
                return FindScriptExportEnumValue(previous, value.id) != nullptr || value.introducedVersion > previousVersion;
            });
        }

        bool CompatibleType(const ScriptExportTypeDescriptor &previous, const ScriptExportTypeDescriptor &candidate, const bool sameMajor,
                            const ScriptExportVersion &previousVersion, const ScriptExportDescriptor &candidateDescriptor) {
            return SameTypeBase(previous, candidate) &&
                   (previous.kind != ScriptExportNamedTypeKind::Struct ||
                    CompatibleFields(previous, candidate, sameMajor, previousVersion, candidateDescriptor)) &&
                   (previous.kind != ScriptExportNamedTypeKind::Enum ||
                    CompatibleEnumValues(previous, candidate, sameMajor, previousVersion, candidateDescriptor));
        }

        bool CompatiblePreviousParameters(const ScriptExportFunctionDescriptor &previous, const ScriptExportFunctionDescriptor &candidate,
                                          const bool sameMajor, const ScriptExportDescriptor &candidateDescriptor) {
            return std::ranges::all_of(previous.parameters, [&](const ScriptExportParameterDescriptor &parameter) {
                if (const ScriptExportParameterDescriptor *replacement = FindScriptExportParameter(candidate, parameter.id);
                    replacement != nullptr)
                    return SameParameterShape(parameter, *replacement);
                return !sameMajor && HasTombstone(candidateDescriptor, MakeScopedScriptExportIdentity(previous.id, parameter.id));
            });
        }

        bool CompatibleAddedParameters(const ScriptExportFunctionDescriptor &previous, const ScriptExportFunctionDescriptor &candidate,
                                       const ScriptExportVersion &previousVersion) {
            return std::ranges::all_of(candidate.parameters, [&](const ScriptExportParameterDescriptor &parameter) {
                return FindScriptExportParameter(previous, parameter.id) != nullptr ||
                       (parameter.introducedVersion > previousVersion &&
                        parameter.requirement == ScriptExportParameterRequirement::Optional && parameter.canonicalDefault.has_value());
            });
        }

        bool CompatibleParameters(const ScriptExportFunctionDescriptor &previous, const ScriptExportFunctionDescriptor &candidate,
                                  const bool sameMajor, const ScriptExportVersion &previousVersion,
                                  const ScriptExportDescriptor &candidateDescriptor) {
            return CompatiblePreviousParameters(previous, candidate, sameMajor, candidateDescriptor) &&
                   CompatibleAddedParameters(previous, candidate, previousVersion);
        }

        bool CompatibleResults(const ScriptExportFunctionDescriptor &previous, const ScriptExportFunctionDescriptor &candidate,
                               const bool sameMajor, const ScriptExportVersion &previousVersion,
                               const ScriptExportDescriptor &candidateDescriptor) {
            if (!std::ranges::all_of(previous.results, [&](const ScriptExportResultDescriptor &result) {
                if (const ScriptExportResultDescriptor *replacement = FindScriptExportResult(candidate, result.id); replacement != nullptr)
                    return SameResultShape(result, *replacement);
                return !sameMajor && HasTombstone(candidateDescriptor, MakeScopedScriptExportIdentity(previous.id, result.id));
            }))
                return false;
            return std::ranges::all_of(candidate.results, [&](const ScriptExportResultDescriptor &result) {
                return FindScriptExportResult(previous, result.id) != nullptr || result.introducedVersion > previousVersion;
            });
        }

        bool CompatibleFunction(const ScriptExportFunctionDescriptor &previous, const ScriptExportFunctionDescriptor &candidate,
                                const bool sameMajor, const ScriptExportVersion &previousVersion,
                                const ScriptExportDescriptor &candidateDescriptor) {
            const bool retainsErrors = std::ranges::all_of(previous.errorIds, [&](const std::string &errorId) {
                return ContainsString(candidate.errorIds, errorId) || (!sameMajor && HasTombstone(candidateDescriptor, errorId));
            });
            return SameFunctionBase(previous, candidate) &&
                   CompatibleParameters(previous, candidate, sameMajor, previousVersion, candidateDescriptor) &&
                   CompatibleResults(previous, candidate, sameMajor, previousVersion, candidateDescriptor) && retainsErrors;
        }

        bool CompatibleReplacementIdentity(const ScriptExportDescriptor &previous, const ScriptExportDescriptor &candidate) {
            if (candidate.moduleId != previous.moduleId || candidate.id != previous.id || candidate.nameSpace != previous.nameSpace ||
                candidate.version < previous.version)
                return false;
            return true;
        }

        bool CompatibleReplacementServiceIdentity(const ScriptExportDescriptor &previous, const ScriptExportDescriptor &candidate) {
            return candidate.service.capability == previous.service.capability &&
                   candidate.service.serviceId == previous.service.serviceId && candidate.service.contractId == previous.service.contractId;
        }

        bool CompatibleSameMajorVersion(const ScriptExportDescriptor &previous, const ScriptExportDescriptor &candidate) {
            return candidate.compatibility.minimum <= previous.version && candidate.compatibility.minimum.major == previous.version.major &&
                   candidate.service.minimumVersion == previous.service.minimumVersion;
        }

        bool CompatibleMajorVersion(const ScriptExportDescriptor &previous, const ScriptExportDescriptor &candidate) {
            return candidate.version.major > previous.version.major && candidate.compatibility.minimum.major == candidate.version.major;
        }

        bool CompatibleReplacementServiceAndVersion(const ScriptExportDescriptor &previous, const ScriptExportDescriptor &candidate,
                                                    bool &sameMajor) {
            if (!CompatibleReplacementServiceIdentity(previous, candidate))
                return false;
            sameMajor = candidate.version.major == previous.version.major;
            return sameMajor ? CompatibleSameMajorVersion(previous, candidate) : CompatibleMajorVersion(previous, candidate);
        }

        bool CompatibleReplacementTypes(const ScriptExportDescriptor &previous, const ScriptExportDescriptor &candidate,
                                        const bool sameMajor) {
            if (!std::ranges::all_of(previous.types, [&](const ScriptExportTypeDescriptor &type) {
                if (const ScriptExportTypeDescriptor *replacement = FindScriptExportType(candidate, type.id); replacement != nullptr)
                    return CompatibleType(type, *replacement, sameMajor, previous.version, candidate);
                return !sameMajor && HasTombstone(candidate, type.id);
            }))
                return false;
            return std::ranges::all_of(candidate.types, [&](const ScriptExportTypeDescriptor &type) {
                return FindScriptExportType(previous, type.id) != nullptr || type.introducedVersion > previous.version;
            });
        }

        bool CompatibleReplacementFunctions(const ScriptExportDescriptor &previous, const ScriptExportDescriptor &candidate,
                                            const bool sameMajor) {
            if (!std::ranges::all_of(previous.functions, [&](const ScriptExportFunctionDescriptor &function) {
                if (const ScriptExportFunctionDescriptor *replacement = FindScriptExportFunction(candidate, function.id);
                    replacement != nullptr)
                    return CompatibleFunction(function, *replacement, sameMajor, previous.version, candidate);
                return !sameMajor && HasTombstone(candidate, function.id);
            }))
                return false;
            return std::ranges::all_of(candidate.functions, [&](const ScriptExportFunctionDescriptor &function) {
                return FindScriptExportFunction(previous, function.id) != nullptr || function.introducedVersion > previous.version;
            });
        }

        bool CompatibleReplacementConstants(const ScriptExportDescriptor &previous, const ScriptExportDescriptor &candidate,
                                            const bool sameMajor) {
            if (!std::ranges::all_of(previous.constants, [&](const ScriptExportConstantDescriptor &constant) {
                if (const ScriptExportConstantDescriptor *replacement = FindScriptExportConstant(candidate, constant.id);
                    replacement != nullptr)
                    return SameConstantShape(constant, *replacement);
                return !sameMajor && HasTombstone(candidate, constant.id);
            }))
                return false;
            return std::ranges::all_of(candidate.constants, [&](const ScriptExportConstantDescriptor &constant) {
                return FindScriptExportConstant(previous, constant.id) != nullptr || constant.introducedVersion > previous.version;
            });
        }

        bool CompatibleReplacementErrors(const ScriptExportDescriptor &previous, const ScriptExportDescriptor &candidate,
                                         const bool sameMajor) {
            if (!std::ranges::all_of(previous.errors, [&](const ScriptExportErrorDescriptor &error) {
                if (const ScriptExportErrorDescriptor *replacement = FindScriptExportError(candidate, error.id); replacement != nullptr)
                    return SameErrorShape(error, *replacement);
                return !sameMajor && HasTombstone(candidate, error.id);
            }))
                return false;
            return std::ranges::all_of(candidate.errors, [&](const ScriptExportErrorDescriptor &error) {
                return FindScriptExportError(previous, error.id) != nullptr || error.introducedVersion > previous.version;
            });
        }

        bool CompatibleReplacement(const ScriptExportDescriptor &previous, const ScriptExportDescriptor &candidate) {
            if (!CompatibleReplacementIdentity(previous, candidate))
                return false;
            if (candidate.version == previous.version)
                return candidate == previous;
            bool sameMajor{};
            if (!CompatibleReplacementServiceAndVersion(previous, candidate, sameMajor))
                return false;
            if (!ContainsAllStrings(previous.tombstonedSymbols, candidate.tombstonedSymbols))
                return false;
            return CompatibleReplacementTypes(previous, candidate, sameMajor) &&
                   CompatibleReplacementFunctions(previous, candidate, sameMajor) &&
                   CompatibleReplacementConstants(previous, candidate, sameMajor) &&
                   CompatibleReplacementErrors(previous, candidate, sameMajor);
        }
    }  // namespace

    Result<void> ValidateScriptExportReplacement(const ScriptExportDescriptorSnapshotPtr &previous,
                                                 const std::span<const ScriptExportDescriptor> candidate) {
        if (previous == nullptr)
            return ScriptExportDescriptorInvalid<void>("Script export descriptor replacement requires a prior generation.");
        for (const ScriptExportDescriptor &prior : previous->Descriptors()) {
            const ScriptExportDescriptor *replacement = FindScriptExportDescriptor(candidate, prior.id);
            if (replacement == nullptr || !CompatibleReplacement(prior, *replacement))
                return ScriptExportDescriptorIncompatible<void>("Script export replacement is not compatible with the prior generation.");
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Extensions::Detail
