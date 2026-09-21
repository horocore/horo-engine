#include "ScriptExportDescriptorInternal.h"

#include <iterator>
#include <ranges>
#include <set>

namespace Horo::Extensions::Detail {
    class ScriptExportTypeGraphValidator final {
    public:
        ScriptExportTypeGraphValidator(const ScriptExportDescriptor &descriptor, const ScriptExportDescriptorLimits &limits)
            : descriptor_(descriptor), limits_(limits), state_(descriptor.types.size()) {}

        [[nodiscard]] Result<void> Validate() {
            for (std::size_t index = 0; index < descriptor_.types.size(); ++index) {
                if (const Result<void> result = Visit(index, 1); result.HasError())
                    return result;
            }
            return Result<void>::Success();
        }

    private:
        [[nodiscard]] Result<void> VisitReference(const ScriptExportTypeReference &reference, const std::size_t depth) {
            if (reference.primitive != ScriptExportPrimitiveKind::Count)
                return Result<void>::Success();
            const auto found = std::ranges::find(descriptor_.types, reference.namedType, &ScriptExportTypeDescriptor::id);
            if (found == descriptor_.types.end())
                return ScriptExportDescriptorInvalid<void>("Named type graph references an unknown type.");
            return Visit(static_cast<std::size_t>(std::distance(descriptor_.types.begin(), found)), depth + 1U);
        }

        [[nodiscard]] Result<void> VisitChildren(const ScriptExportTypeDescriptor &type, const std::size_t depth) {
            using enum ScriptExportNamedTypeKind;
            if (type.kind == Struct) {
                for (const ScriptExportFieldDescriptor &field : type.fields) {
                    if (const Result<void> result = VisitReference(field.type, depth); result.HasError())
                        return result;
                }
            } else if (type.kind == Array) {
                if (const Result<void> result = VisitReference(type.elementType, depth); result.HasError())
                    return result;
            } else if (type.kind == Map) {
                if (const Result<void> result = VisitReference(type.keyType, depth); result.HasError())
                    return result;
                if (const Result<void> result = VisitReference(type.valueType, depth); result.HasError())
                    return result;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> Visit(const std::size_t index, const std::size_t depth) {
            if (depth > limits_.maximumTypeNestingDepth)
                return ScriptExportDescriptorCapacity<void>("Named type nesting exceeds the configured bound.");
            if (state_[index] == 1U)
                return ScriptExportDescriptorInvalid<void>("Named type declarations must not contain recursive cycles.");
            if (state_[index] == 2U)
                return Result<void>::Success();

            state_[index] = 1U;
            if (const Result<void> children = VisitChildren(descriptor_.types[index], depth); children.HasError())
                return children;
            state_[index] = 2U;
            return Result<void>::Success();
        }

        const ScriptExportDescriptor &descriptor_;
        const ScriptExportDescriptorLimits &limits_;
        std::vector<std::uint8_t> state_;
    };

    namespace {
        Result<void> ValidateTypeIdentityAndKind(const ScriptExportTypeDescriptor &type, const ScriptExportVersion &current,
                                                 const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes) {
            if (!IsCanonicalScriptExportIdentity(type.id, limits.maximumIdentityBytes))
                return ScriptExportDescriptorInvalid<void>("Named type identity is not canonical.");
            if (!AddScriptExportBytes(totalBytes, type.id.size(), limits.maximumDescriptorBytes))
                return ScriptExportDescriptorCapacity<void>("Named type identities exceed the descriptor bound.");
            if (const Result<void> introduction = ValidateScriptExportVersionIntroduction(type.introducedVersion, current);
                introduction.HasError())
                return introduction;
            if (!IsKnownNamedTypeKind(type.kind))
                return ScriptExportDescriptorInvalid<void>("Named type kind is unsupported.");
            return Result<void>::Success();
        }

        Result<void> ValidateTypeElementShape(const ScriptExportTypeDescriptor &type) {
            if (type.kind != ScriptExportNamedTypeKind::Array && type.elementType != ScriptExportTypeReference{})
                return ScriptExportDescriptorInvalid<void>("Only array declarations may carry an element type.");
            if (type.kind != ScriptExportNamedTypeKind::Map &&
                (type.keyType != ScriptExportTypeReference{} || type.valueType != ScriptExportTypeReference{}))
                return ScriptExportDescriptorInvalid<void>("Only map declarations may carry key and value types.");
            return Result<void>::Success();
        }

        Result<void> ValidateTypeElementBound(const ScriptExportTypeDescriptor &type, const ScriptExportDescriptorLimits &limits) {
            using enum ScriptExportNamedTypeKind;
            if (type.kind != Array && type.kind != Map && type.maximumElements != 0)
                return ScriptExportDescriptorInvalid<void>("Only array and map declarations may carry element bounds.");
            if ((type.kind == Array || type.kind == Map) &&
                (type.maximumElements == 0 || type.maximumElements > limits.maximumContainerElements))
                return ScriptExportDescriptorCapacity<void>("Named container element bound exceeds the configured limit.");
            return Result<void>::Success();
        }

        Result<void> ValidateTypeContainerShape(const ScriptExportTypeDescriptor &type, const ScriptExportDescriptorLimits &limits) {
            if (const Result<void> element = ValidateTypeElementShape(type); element.HasError())
                return element;
            return ValidateTypeElementBound(type, limits);
        }

        Result<void> ValidateTypeMemberKinds(const ScriptExportTypeDescriptor &type) {
            if (type.kind != ScriptExportNamedTypeKind::Struct && !type.fields.empty())
                return ScriptExportDescriptorInvalid<void>("Only struct declarations may contain fields.");
            if (type.kind != ScriptExportNamedTypeKind::Enum && !type.enumValues.empty())
                return ScriptExportDescriptorInvalid<void>("Only enum declarations may contain enum values.");
            return Result<void>::Success();
        }

        Result<void> ValidateTypeCollectionLimits(const ScriptExportTypeDescriptor &type, const ScriptExportDescriptorLimits &limits) {
            using enum ScriptExportNamedTypeKind;
            if (type.kind == Enum && type.enumValues.empty())
                return ScriptExportDescriptorInvalid<void>("Enum declarations must contain at least one value.");
            if (type.kind == Struct && type.fields.size() > limits.maximumFieldsPerStruct)
                return ScriptExportDescriptorCapacity<void>("Struct field count exceeds the configured limit.");
            if (type.kind == Enum && type.enumValues.size() > limits.maximumEnumValues)
                return ScriptExportDescriptorCapacity<void>("Enum value count exceeds the configured limit.");
            return Result<void>::Success();
        }

        Result<void> ValidateTypeCollectionShape(const ScriptExportTypeDescriptor &type, const ScriptExportDescriptorLimits &limits) {
            if (const Result<void> kinds = ValidateTypeMemberKinds(type); kinds.HasError())
                return kinds;
            return ValidateTypeCollectionLimits(type, limits);
        }

        struct TypeValidationContext final {
            const ScriptExportTypeDescriptor &type;
            const ScriptExportVersion &current;
            std::set<std::string, std::less<>> &activeSymbols;
            const ScriptExportDescriptorLimits &limits;
            std::size_t &totalBytes;
        };

        Result<void> ValidateStructField(const TypeValidationContext &context, const ScriptExportFieldDescriptor &field,
                                         const std::set<std::string, std::less<>> &namedTypes,
                                         std::set<std::string, std::less<>> &localIds) {
            if (!IsCanonicalScriptExportIdentity(field.id, context.limits.maximumIdentityBytes))
                return ScriptExportDescriptorInvalid<void>("Struct field identity is not canonical.");
            if (!IsBoundedScriptExportScopedIdentity(context.type.id, field.id, context.limits.maximumIdentityBytes))
                return ScriptExportDescriptorCapacity<void>("Qualified struct field identity exceeds the configured bound.");
            if (!localIds.insert(field.id).second)
                return ScriptExportDescriptorConflict<void>("Struct field identities must be unique.");
            if (!context.activeSymbols.insert(MakeScopedScriptExportIdentity(context.type.id, field.id)).second)
                return ScriptExportDescriptorConflict<void>("Script export symbol identities must be unique.");
            return ValidateScriptExportField(field, context.current, namedTypes, context.limits, context.totalBytes);
        }

        Result<void> ValidateStructFields(const ScriptExportTypeDescriptor &type, const ScriptExportVersion &current,
                                          const std::set<std::string, std::less<>> &namedTypes,
                                          std::set<std::string, std::less<>> &activeSymbols, const ScriptExportDescriptorLimits &limits,
                                          std::size_t &totalBytes) {
            std::set<std::string, std::less<>> localIds;
            const TypeValidationContext context{type, current, activeSymbols, limits, totalBytes};
            for (const ScriptExportFieldDescriptor &field : type.fields) {
                if (const Result<void> valid = ValidateStructField(context, field, namedTypes, localIds); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }

        Result<void> ValidateEnumValue(const TypeValidationContext &context, const ScriptExportEnumValueDescriptor &value,
                                       std::set<std::string, std::less<>> &localIds, std::set<std::int64_t> &numericValues) {
            if (!IsCanonicalScriptExportIdentity(value.id, context.limits.maximumIdentityBytes))
                return ScriptExportDescriptorInvalid<void>("Enum value identity is not canonical.");
            if (!IsBoundedScriptExportScopedIdentity(context.type.id, value.id, context.limits.maximumIdentityBytes))
                return ScriptExportDescriptorCapacity<void>("Qualified enum value identity exceeds the configured bound.");
            if (!localIds.insert(value.id).second)
                return ScriptExportDescriptorConflict<void>("Enum identities and numeric values must be unique.");
            if (!numericValues.insert(value.value).second)
                return ScriptExportDescriptorConflict<void>("Enum identities and numeric values must be unique.");
            if (!context.activeSymbols.insert(MakeScopedScriptExportIdentity(context.type.id, value.id)).second)
                return ScriptExportDescriptorConflict<void>("Script export symbol identities must be unique.");
            if (!AddScriptExportBytes(context.totalBytes, value.id.size(), context.limits.maximumDescriptorBytes))
                return ScriptExportDescriptorCapacity<void>("Enum value identities exceed the descriptor bound.");
            if (const Result<void> introduction = ValidateScriptExportVersionIntroduction(value.introducedVersion, context.current);
                introduction.HasError())
                return introduction;
            return ValidateScriptExportDocumentation(value.documentation, context.totalBytes, context.limits);
        }

        Result<void> ValidateEnumValues(const ScriptExportTypeDescriptor &type, const ScriptExportVersion &current,
                                        std::set<std::string, std::less<>> &activeSymbols, const ScriptExportDescriptorLimits &limits,
                                        std::size_t &totalBytes) {
            std::set<std::string, std::less<>> localIds;
            std::set<std::int64_t> numericValues;
            const TypeValidationContext context{type, current, activeSymbols, limits, totalBytes};
            for (const ScriptExportEnumValueDescriptor &value : type.enumValues) {
                if (const Result<void> valid = ValidateEnumValue(context, value, localIds, numericValues); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }

        Result<void> ValidateArrayType(const ScriptExportTypeDescriptor &type, const std::set<std::string, std::less<>> &namedTypes,
                                       const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes) {
            if (const Result<void> element = ValidateScriptExportTypeReference(type.elementType, namedTypes, limits, totalBytes);
                element.HasError())
                return element;
            return Result<void>::Success();
        }

        Result<void> ValidateMapType(const ScriptExportTypeDescriptor &type, const std::set<std::string, std::less<>> &namedTypes,
                                     const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes) {
            using enum ScriptExportPrimitiveKind;
            if (const Result<void> key = ValidateScriptExportTypeReference(type.keyType, namedTypes, limits, totalBytes); key.HasError())
                return key;
            if (type.keyType.primitive != String && type.keyType.primitive != SignedInteger && type.keyType.primitive != UnsignedInteger)
                return ScriptExportDescriptorInvalid<void>("Map keys must be non-null primitive strings or integers.");
            if (type.keyType.nullability != ScriptExportNullability::NonNull)
                return ScriptExportDescriptorInvalid<void>("Map keys cannot be nullable.");
            if (const Result<void> value = ValidateScriptExportTypeReference(type.valueType, namedTypes, limits, totalBytes);
                value.HasError())
                return value;
            return Result<void>::Success();
        }

        Result<void> ValidateTypeMembers(const ScriptExportTypeDescriptor &type, const ScriptExportVersion &current,
                                         const std::set<std::string, std::less<>> &namedTypes,
                                         std::set<std::string, std::less<>> &activeSymbols, const ScriptExportDescriptorLimits &limits,
                                         std::size_t &totalBytes) {
            using enum ScriptExportNamedTypeKind;
            if (type.kind == Struct)
                return ValidateStructFields(type, current, namedTypes, activeSymbols, limits, totalBytes);
            if (type.kind == Enum)
                return ValidateEnumValues(type, current, activeSymbols, limits, totalBytes);
            if (type.kind == Array)
                return ValidateArrayType(type, namedTypes, limits, totalBytes);
            return ValidateMapType(type, namedTypes, limits, totalBytes);
        }
    }  // namespace

    Result<void> ValidateScriptExportType(const ScriptExportTypeDescriptor &type, const ScriptExportVersion &current,
                                          const std::set<std::string, std::less<>> &namedTypes,
                                          std::set<std::string, std::less<>> &activeSymbols, const ScriptExportDescriptorLimits &limits,
                                          std::size_t &totalBytes) {
        if (const Result<void> identity = ValidateTypeIdentityAndKind(type, current, limits, totalBytes); identity.HasError())
            return identity;
        if (const Result<void> container = ValidateTypeContainerShape(type, limits); container.HasError())
            return container;
        if (const Result<void> collection = ValidateTypeCollectionShape(type, limits); collection.HasError())
            return collection;
        if (const Result<void> documentation = ValidateScriptExportDocumentation(type.documentation, totalBytes, limits);
            documentation.HasError())
            return documentation;
        return ValidateTypeMembers(type, current, namedTypes, activeSymbols, limits, totalBytes);
    }

    Result<void> ValidateScriptExportError(const ScriptExportErrorDescriptor &error, const ScriptExportVersion &current,
                                           const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes) {
        if (!IsCanonicalScriptExportIdentity(error.id, limits.maximumIdentityBytes))
            return ScriptExportDescriptorInvalid<void>("Error identity is not canonical.");
        if (!AddScriptExportBytes(totalBytes, error.id.size(), limits.maximumDescriptorBytes))
            return ScriptExportDescriptorCapacity<void>("Error identities exceed the descriptor bound.");
        if (const Result<void> introduction = ValidateScriptExportVersionIntroduction(error.introducedVersion, current);
            introduction.HasError())
            return introduction;
        return ValidateScriptExportDocumentation(error.documentation, totalBytes, limits);
    }

    Result<void> ValidateScriptExportConstant(const ScriptExportConstantDescriptor &constant, const ScriptExportVersion &current,
                                              const std::set<std::string, std::less<>> &namedTypes,
                                              const ScriptExportDescriptorLimits &limits, std::size_t &totalBytes) {
        if (!IsCanonicalScriptExportIdentity(constant.id, limits.maximumIdentityBytes))
            return ScriptExportDescriptorInvalid<void>("Constant identity is not canonical.");
        if (!AddScriptExportBytes(totalBytes, constant.id.size(), limits.maximumDescriptorBytes))
            return ScriptExportDescriptorCapacity<void>("Constant identities exceed the descriptor bound.");
        if (const Result<void> introduction = ValidateScriptExportVersionIntroduction(constant.introducedVersion, current);
            introduction.HasError())
            return introduction;
        if (const Result<void> type = ValidateScriptExportTypeReference(constant.type, namedTypes, limits, totalBytes); type.HasError())
            return type;
        if (constant.canonicalValue.empty())
            return ScriptExportDescriptorInvalid<void>("Constants require a canonical value encoding.");
        if (!AddScriptExportBytes(totalBytes, constant.canonicalValue.size(), limits.maximumDescriptorBytes) ||
            constant.canonicalValue.size() > limits.maximumConstantBytes)
            return ScriptExportDescriptorCapacity<void>("Constant value exceeds the configured bound.");
        return ValidateScriptExportDocumentation(constant.documentation, totalBytes, limits);
    }

    Result<void> ValidateScriptExportTypeGraph(const ScriptExportDescriptor &descriptor, const ScriptExportDescriptorLimits &limits) {
        return ScriptExportTypeGraphValidator{descriptor, limits}.Validate();
    }
}  // namespace Horo::Extensions::Detail
