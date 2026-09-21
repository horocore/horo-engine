#include "../ExtensionAuthorityIdentityValidation.h"
#include "ScriptExportDescriptorInternal.h"

#include <iterator>
#include <memory>
#include <ranges>

namespace Horo::Extensions::Detail {
    bool IsCanonicalScriptExportIdentity(const std::string_view value, const std::size_t maximumBytes) noexcept {
        return value.size() <= maximumBytes && IsCanonicalExtensionAuthorityId(value);
    }

    bool IsKnownPrimitive(const ScriptExportPrimitiveKind primitive) noexcept {
        return primitive < ScriptExportPrimitiveKind::Count;
    }

    bool IsKnownNullability(const ScriptExportNullability nullability) noexcept {
        return nullability < ScriptExportNullability::Count;
    }

    bool IsKnownNamedTypeKind(const ScriptExportNamedTypeKind kind) noexcept {
        return kind < ScriptExportNamedTypeKind::Count;
    }

    bool IsKnownRequirement(const ScriptExportParameterRequirement requirement) noexcept {
        return requirement < ScriptExportParameterRequirement::Count;
    }

    bool IsKnownInvocationMode(const ScriptExportInvocationMode mode) noexcept {
        return mode < ScriptExportInvocationMode::Count;
    }

    bool IsValidScriptExportVersion(const ScriptExportVersion &version) noexcept {
        return version.IsValid();
    }

    bool IsValidScriptExportLimits(const ScriptExportDescriptorLimits &limits) noexcept {
        return limits.maximumIdentityBytes > 0 && limits.maximumDocumentationBytes > 0 && limits.maximumDefaultBytes > 0 &&
               limits.maximumConstantBytes > 0 && limits.maximumTypeNestingDepth > 0 && limits.maximumDescriptorBytes > 0;
    }

    bool AddScriptExportBytes(std::size_t &total, const std::size_t bytes, const std::size_t maximum) noexcept {
        if (bytes > maximum || total > maximum - bytes)
            return false;
        total += bytes;
        return true;
    }

    std::string MakeScopedScriptExportIdentity(const std::string_view parent, const std::string_view child) {
        std::string result;
        result.reserve(parent.size() + child.size() + 1U);
        result.append(parent);
        result.push_back('.');
        result.append(child);
        return result;
    }

    bool IsBoundedScriptExportScopedIdentity(const std::string_view parent, const std::string_view child,
                                             const std::size_t maximumBytes) noexcept {
        return maximumBytes > 0 && parent.size() < maximumBytes && child.size() <= maximumBytes - parent.size() - 1U;
    }

    Result<void> ValidateScriptExportText(const std::string_view value, const std::size_t maximumBytes, std::size_t &totalBytes,
                                          const ScriptExportDescriptorLimits &limits, const bool required, const std::string_view field) {
        if (required && value.empty())
            return ScriptExportDescriptorInvalid<void>(field);
        if (!AddScriptExportBytes(totalBytes, value.size(), limits.maximumDescriptorBytes) || value.size() > maximumBytes)
            return ScriptExportDescriptorCapacity<void>(field);
        return Result<void>::Success();
    }

    Result<void> ValidateScriptExportDocumentation(const ScriptExportDocumentation &documentation, std::size_t &totalBytes,
                                                   const ScriptExportDescriptorLimits &limits) {
        if (const Result<void> summary = ValidateScriptExportText(documentation.summary, limits.maximumDocumentationBytes, totalBytes,
                                                                  limits, false, "Documentation summary exceeds its bound.");
            summary.HasError())
            return summary;
        if (const Result<void> description =
                ValidateScriptExportText(documentation.description, limits.maximumDocumentationBytes, totalBytes, limits, false,
                                         "Documentation description exceeds its bound");
            description.HasError())
            return description;
        if (const Result<void> replacement =
                ValidateScriptExportText(documentation.replacementId, limits.maximumIdentityBytes, totalBytes, limits, false,
                                         "Documentation replacement identity exceeds its bound.");
            replacement.HasError())
            return replacement;
        if (!documentation.replacementId.empty() &&
            !IsCanonicalScriptExportIdentity(documentation.replacementId, limits.maximumIdentityBytes))
            return ScriptExportDescriptorInvalid<void>("Documentation replacement identity is not canonical.");
        if (!documentation.replacementId.empty() && !documentation.deprecated)
            return ScriptExportDescriptorInvalid<void>("A replacement identity requires deprecated documentation metadata.");
        return Result<void>::Success();
    }

    Result<void> ValidateScriptExportVersionIntroduction(const ScriptExportVersion &introduced, const ScriptExportVersion &current) {
        if (!IsValidScriptExportVersion(introduced) || introduced > current)
            return ScriptExportDescriptorInvalid<void>("A declaration introduction version is outside the API version.");
        return Result<void>::Success();
    }

    Result<void> ValidateScriptExportTypeReference(const ScriptExportTypeReference &reference,
                                                   const std::set<std::string, std::less<>> &namedTypes,
                                                   const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes) {
        if (!IsKnownNullability(reference.nullability))
            return ScriptExportDescriptorInvalid<void>("Type nullability is unsupported.");
        const bool named = reference.primitive == ScriptExportPrimitiveKind::Count;
        if (!named && !IsKnownPrimitive(reference.primitive))
            return ScriptExportDescriptorInvalid<void>("Type primitive is unsupported.");
        if (named) {
            if (!IsCanonicalScriptExportIdentity(reference.namedType, limits.maximumIdentityBytes))
                return ScriptExportDescriptorInvalid<void>("Named type identity is not canonical.");
            if (!namedTypes.contains(reference.namedType))
                return ScriptExportDescriptorInvalid<void>("Type reference targets an unknown named type.");
            if (!AddScriptExportBytes(totalBytes, reference.namedType.size(), limits.maximumDescriptorBytes))
                return ScriptExportDescriptorCapacity<void>("Named type references exceed the descriptor bound.");
        } else if (!reference.namedType.empty()) {
            return ScriptExportDescriptorInvalid<void>("Primitive type references cannot carry a named type identity.");
        }
        return Result<void>::Success();
    }

    Result<void> ValidateScriptExportDefault(const ScriptExportParameterRequirement requirement,
                                             const std::optional<std::vector<std::byte>> &canonicalDefault,
                                             const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes) {
        if (canonicalDefault.has_value()) {
            if (requirement != ScriptExportParameterRequirement::Optional)
                return ScriptExportDescriptorInvalid<void>("Only optional declarations may carry canonical defaults.");
            if (!AddScriptExportBytes(totalBytes, canonicalDefault->size(), limits.maximumDescriptorBytes) ||
                canonicalDefault->size() > limits.maximumDefaultBytes)
                return ScriptExportDescriptorCapacity<void>("Canonical default exceeds the descriptor bound.");
        } else if (requirement == ScriptExportParameterRequirement::Optional) {
            return ScriptExportDescriptorInvalid<void>("Optional declarations require a canonical default.");
        }
        return Result<void>::Success();
    }

    Result<void> ValidateScriptExportField(const ScriptExportFieldDescriptor &field, const ScriptExportVersion &current,
                                           const std::set<std::string, std::less<>> &namedTypes, const ScriptExportDescriptorLimits &limits,
                                           std::size_t &totalBytes) {
        if (!IsCanonicalScriptExportIdentity(field.id, limits.maximumIdentityBytes))
            return ScriptExportDescriptorInvalid<void>("Struct field identity is not canonical.");
        if (!AddScriptExportBytes(totalBytes, field.id.size(), limits.maximumDescriptorBytes))
            return ScriptExportDescriptorCapacity<void>("Struct field identities exceed the descriptor bound.");
        if (const Result<void> introduction = ValidateScriptExportVersionIntroduction(field.introducedVersion, current);
            introduction.HasError())
            return introduction;
        if (!IsKnownRequirement(field.requirement))
            return ScriptExportDescriptorInvalid<void>("Struct field requirement is unsupported.");
        if (const Result<void> type = ValidateScriptExportTypeReference(field.type, namedTypes, limits, totalBytes); type.HasError())
            return type;
        if (const Result<void> defaultValue = ValidateScriptExportDefault(field.requirement, field.canonicalDefault, limits, totalBytes);
            defaultValue.HasError())
            return defaultValue;
        return ValidateScriptExportDocumentation(field.documentation, totalBytes, limits);
    }

    Result<void> ValidateScriptExportParameter(const ScriptExportParameterDescriptor &parameter, const ScriptExportVersion &current,
                                               const std::set<std::string, std::less<>> &namedTypes,
                                               const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes) {
        if (!IsCanonicalScriptExportIdentity(parameter.id, limits.maximumIdentityBytes))
            return ScriptExportDescriptorInvalid<void>("Function parameter identity is not canonical.");
        if (!AddScriptExportBytes(totalBytes, parameter.id.size(), limits.maximumDescriptorBytes))
            return ScriptExportDescriptorCapacity<void>("Function parameter identities exceed the descriptor bound.");
        if (const Result<void> introduction = ValidateScriptExportVersionIntroduction(parameter.introducedVersion, current);
            introduction.HasError())
            return introduction;
        if (!IsKnownRequirement(parameter.requirement))
            return ScriptExportDescriptorInvalid<void>("Function parameter requirement is unsupported.");
        if (const Result<void> type = ValidateScriptExportTypeReference(parameter.type, namedTypes, limits, totalBytes); type.HasError())
            return type;
        if (const Result<void> defaultValue =
                ValidateScriptExportDefault(parameter.requirement, parameter.canonicalDefault, limits, totalBytes);
            defaultValue.HasError())
            return defaultValue;
        return ValidateScriptExportDocumentation(parameter.documentation, totalBytes, limits);
    }

    Result<void> ValidateScriptExportResult(const ScriptExportResultDescriptor &result, const ScriptExportVersion &current,
                                            const std::set<std::string, std::less<>> &namedTypes,
                                            const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes) {
        if (!IsCanonicalScriptExportIdentity(result.id, limits.maximumIdentityBytes))
            return ScriptExportDescriptorInvalid<void>("Function result identity is not canonical.");
        if (!AddScriptExportBytes(totalBytes, result.id.size(), limits.maximumDescriptorBytes))
            return ScriptExportDescriptorCapacity<void>("Function result identities exceed the descriptor bound.");
        if (const Result<void> introduction = ValidateScriptExportVersionIntroduction(result.introducedVersion, current);
            introduction.HasError())
            return introduction;
        if (const Result<void> type = ValidateScriptExportTypeReference(result.type, namedTypes, limits, totalBytes); type.HasError())
            return type;
        return ValidateScriptExportDocumentation(result.documentation, totalBytes, limits);
    }

    const ScriptExportTypeDescriptor *FindScriptExportType(const ScriptExportDescriptor &descriptor, const std::string_view id) noexcept {
        const auto found = std::ranges::lower_bound(descriptor.types, id, {}, &ScriptExportTypeDescriptor::id);
        return found != descriptor.types.end() && found->id == id ? std::to_address(found) : nullptr;
    }

    const ScriptExportFunctionDescriptor *FindScriptExportFunction(const ScriptExportDescriptor &descriptor,
                                                                   const std::string_view id) noexcept {
        const auto found = std::ranges::lower_bound(descriptor.functions, id, {}, &ScriptExportFunctionDescriptor::id);
        return found != descriptor.functions.end() && found->id == id ? std::to_address(found) : nullptr;
    }

    const ScriptExportConstantDescriptor *FindScriptExportConstant(const ScriptExportDescriptor &descriptor,
                                                                   const std::string_view id) noexcept {
        const auto found = std::ranges::lower_bound(descriptor.constants, id, {}, &ScriptExportConstantDescriptor::id);
        return found != descriptor.constants.end() && found->id == id ? std::to_address(found) : nullptr;
    }

    const ScriptExportErrorDescriptor *FindScriptExportError(const ScriptExportDescriptor &descriptor, const std::string_view id) noexcept {
        const auto found = std::ranges::lower_bound(descriptor.errors, id, {}, &ScriptExportErrorDescriptor::id);
        return found != descriptor.errors.end() && found->id == id ? std::to_address(found) : nullptr;
    }

    const ScriptExportParameterDescriptor *FindScriptExportParameter(const ScriptExportFunctionDescriptor &function,
                                                                     const std::string_view id) noexcept {
        const auto found = std::ranges::lower_bound(function.parameters, id, {}, &ScriptExportParameterDescriptor::id);
        return found != function.parameters.end() && found->id == id ? std::to_address(found) : nullptr;
    }

    const ScriptExportResultDescriptor *FindScriptExportResult(const ScriptExportFunctionDescriptor &function,
                                                               const std::string_view id) noexcept {
        const auto found = std::ranges::lower_bound(function.results, id, {}, &ScriptExportResultDescriptor::id);
        return found != function.results.end() && found->id == id ? std::to_address(found) : nullptr;
    }

    const ScriptExportFieldDescriptor *FindScriptExportField(const ScriptExportTypeDescriptor &type, const std::string_view id) noexcept {
        const auto found = std::ranges::lower_bound(type.fields, id, {}, &ScriptExportFieldDescriptor::id);
        return found != type.fields.end() && found->id == id ? std::to_address(found) : nullptr;
    }

    const ScriptExportEnumValueDescriptor *FindScriptExportEnumValue(const ScriptExportTypeDescriptor &type,
                                                                     const std::string_view id) noexcept {
        const auto found = std::ranges::lower_bound(type.enumValues, id, {}, &ScriptExportEnumValueDescriptor::id);
        return found != type.enumValues.end() && found->id == id ? std::to_address(found) : nullptr;
    }

    const ScriptExportDescriptor *FindScriptExportDescriptor(const std::span<const ScriptExportDescriptor> descriptors,
                                                             const std::string_view id) noexcept {
        const auto found = std::ranges::lower_bound(descriptors, id, {}, &ScriptExportDescriptor::id);
        return found != descriptors.end() && found->id == id ? std::to_address(found) : nullptr;
    }

    bool ContainsString(const std::vector<std::string> &values, const std::string_view value) noexcept {
        return std::ranges::find(values, value) != values.end();
    }

    bool HasTombstone(const ScriptExportDescriptor &descriptor, const std::string_view id) noexcept {
        return ContainsString(descriptor.tombstonedSymbols, id);
    }

    bool SameTypeReference(const ScriptExportTypeReference &left, const ScriptExportTypeReference &right) noexcept {
        return left == right;
    }

    bool SameFieldShape(const ScriptExportFieldDescriptor &left, const ScriptExportFieldDescriptor &right) noexcept {
        return left.id == right.id && SameTypeReference(left.type, right.type) && left.introducedVersion == right.introducedVersion &&
               left.requirement == right.requirement && left.canonicalDefault == right.canonicalDefault;
    }

    bool SameParameterShape(const ScriptExportParameterDescriptor &left, const ScriptExportParameterDescriptor &right) noexcept {
        return left.id == right.id && SameTypeReference(left.type, right.type) && left.introducedVersion == right.introducedVersion &&
               left.requirement == right.requirement && left.canonicalDefault == right.canonicalDefault;
    }

    bool SameResultShape(const ScriptExportResultDescriptor &left, const ScriptExportResultDescriptor &right) noexcept {
        return left.id == right.id && SameTypeReference(left.type, right.type) && left.introducedVersion == right.introducedVersion;
    }

    bool SameEnumValueShape(const ScriptExportEnumValueDescriptor &left, const ScriptExportEnumValueDescriptor &right) noexcept {
        return left.id == right.id && left.value == right.value && left.introducedVersion == right.introducedVersion;
    }

    bool SameTypeBase(const ScriptExportTypeDescriptor &left, const ScriptExportTypeDescriptor &right) noexcept {
        return left.id == right.id && left.kind == right.kind && left.introducedVersion == right.introducedVersion &&
               SameTypeReference(left.elementType, right.elementType) && SameTypeReference(left.keyType, right.keyType) &&
               SameTypeReference(left.valueType, right.valueType) && left.maximumElements == right.maximumElements;
    }

    bool SameFunctionBase(const ScriptExportFunctionDescriptor &left, const ScriptExportFunctionDescriptor &right) noexcept {
        return left.id == right.id && left.introducedVersion == right.introducedVersion && left.invocation == right.invocation;
    }

    bool SameConstantShape(const ScriptExportConstantDescriptor &left, const ScriptExportConstantDescriptor &right) noexcept {
        return left.id == right.id && SameTypeReference(left.type, right.type) && left.introducedVersion == right.introducedVersion &&
               left.canonicalValue == right.canonicalValue;
    }

    bool SameErrorShape(const ScriptExportErrorDescriptor &left, const ScriptExportErrorDescriptor &right) noexcept {
        return left.id == right.id && left.introducedVersion == right.introducedVersion && left.retryable == right.retryable;
    }

    bool ContainsAllStrings(const std::vector<std::string> &required, const std::vector<std::string> &available) noexcept {
        return std::ranges::all_of(required, [&available](const std::string &id) {
            return ContainsString(available, id);
        });
    }
}  // namespace Horo::Extensions::Detail
