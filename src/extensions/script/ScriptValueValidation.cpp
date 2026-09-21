#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/ScriptValue.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace Horo::Extensions {
    namespace {
        using namespace ExtensionErrors;

        [[nodiscard]] Error MakeBoundaryError(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            return MakeError(descriptor, std::move(message));
        }

        template <typename T> [[nodiscard]] Result<T> CapacityExceeded(std::string message) {
            return Result<T>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, std::move(message)));
        }

        template <typename T> [[nodiscard]] Result<T> InvalidEncoding(std::string message) {
            return Result<T>::Failure(MakeBoundaryError(ScriptValueEncodingInvalid, std::move(message)));
        }

        template <typename T> [[nodiscard]] Result<T> TypeMismatch(std::string message) {
            return Result<T>::Failure(MakeBoundaryError(ScriptValueTypeMismatch, std::move(message)));
        }

        [[nodiscard]] bool IsValidIdentity(std::string_view value, std::size_t maximum) noexcept {
            return !value.empty() && value.size() <= maximum && IsValidUtf8ScalarSequence(value);
        }

        [[nodiscard]] bool IsScalarMapKey(ScriptValue::Kind kind) noexcept {
            using enum ScriptValue::Kind;
            switch (kind) {
                case Boolean:
                case SignedInteger:
                case UnsignedInteger:
                case Number:
                case String:
                case Bytes:
                case Enum:
                    return true;
                case Null:
                case Handle:
                case Array:
                case Map:
                case Struct:
                case Count:
                    return false;
            }
            return false;
        }

        struct ValidationContext final {
            const ScriptValueLimits &limits;
            std::size_t work{};
            std::size_t elements{};
            std::size_t rawBytes{};
        };

        [[nodiscard]] Result<void> ConsumeWork(ValidationContext &context, std::size_t amount = 1) {
            if (amount > context.limits.maximumWorkUnits - std::min(context.work, context.limits.maximumWorkUnits))
                return Result<void>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, "Script value work bound was exceeded."));
            context.work += amount;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AddElements(ValidationContext &context, std::size_t amount) {
            if (amount > context.limits.maximumElements - std::min(context.elements, context.limits.maximumElements))
                return Result<void>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, "Script value element bound was exceeded."));
            context.elements += amount;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AddRawBytes(ValidationContext &context, std::size_t amount) {
            if (amount > context.limits.maximumBytes - std::min(context.rawBytes, context.limits.maximumBytes))
                return Result<void>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, "Script value byte bound was exceeded."));
            context.rawBytes += amount;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateStringValue(const ScriptValue &value, ValidationContext &context) {
            const auto string = value.AsString();
            if (!IsValidUtf8ScalarSequence(string))
                return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script string is not valid UTF-8."));
            if (string.size() > context.limits.maximumStringBytes)
                return Result<void>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, "Script string bound was exceeded."));
            return AddRawBytes(context, string.size());
        }

        [[nodiscard]] Result<void> ValidateHandleValue(const ScriptValue &value, ValidationContext &context) {
            const auto *handle = value.AsHandle();
            if (handle == nullptr || !handle->IsValid() || handle->type.empty() || !IsValidUtf8ScalarSequence(handle->type))
                return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script handle identity is malformed."));
            if (handle->type.size() > context.limits.maximumTypeIdentityBytes)
                return Result<void>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, "Script handle type identity is too large."));
            return AddRawBytes(context, handle->type.size());
        }

        [[nodiscard]] Result<void> ValidateEnumValue(const ScriptValue &value, ValidationContext &context) {
            const auto *enumerator = value.AsEnum();
            if (enumerator == nullptr || !IsValidIdentity(enumerator->type, context.limits.maximumTypeIdentityBytes))
                return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script enum identity is malformed."));
            return AddRawBytes(context, enumerator->type.size());
        }

        [[nodiscard]] Result<void> ValidateValueImpl(const ScriptValue &value, ValidationContext &context, std::size_t depth);

        [[nodiscard]] Result<void> ValidateArrayValue(const ScriptValue &value, ValidationContext &context, std::size_t depth) {
            const auto elements = value.AsArray();
            if (elements.size() > context.limits.maximumArrayElements)
                return Result<void>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, "Script array bound was exceeded."));
            if (auto count = AddElements(context, elements.size()); count.HasError())
                return count;
            for (const auto &element : elements) {
                auto result = ValidateValueImpl(element, context, depth + 1);
                if (result.HasError())
                    return result;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateMapValue(const ScriptValue &value, ValidationContext &context, std::size_t depth) {
            const auto entries = value.AsMap();
            if (entries.size() > context.limits.maximumMapEntries)
                return Result<void>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, "Script map bound was exceeded."));
            if (auto count = AddElements(context, entries.size()); count.HasError())
                return count;
            for (std::size_t index = 0; index < entries.size(); ++index) {
                if (!IsScalarMapKey(entries[index].first.GetKind()))
                    return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script map key is not a bounded scalar."));
                for (std::size_t prior = 0; prior < index; ++prior) {
                    if (entries[index].first == entries[prior].first)
                        return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script map contains duplicate keys."));
                }
                if (auto key = ValidateValueImpl(entries[index].first, context, depth + 1); key.HasError())
                    return key;
                if (auto mapped = ValidateValueImpl(entries[index].second, context, depth + 1); mapped.HasError())
                    return mapped;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateStructValue(const ScriptValue &value, ValidationContext &context, std::size_t depth) {
            const auto type = value.StructType();
            if (!IsValidIdentity(type, context.limits.maximumTypeIdentityBytes))
                return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script struct type identity is malformed."));
            if (auto typeBytes = AddRawBytes(context, type.size()); typeBytes.HasError())
                return typeBytes;
            const auto fields = value.AsStruct();
            if (fields.size() > context.limits.maximumStructFields)
                return Result<void>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, "Script struct field bound was exceeded."));
            if (auto count = AddElements(context, fields.size()); count.HasError())
                return count;
            for (std::size_t index = 0; index < fields.size(); ++index) {
                if (!IsValidIdentity(fields[index].first, context.limits.maximumTypeIdentityBytes))
                    return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script struct field identity is malformed."));
                if (auto fieldBytes = AddRawBytes(context, fields[index].first.size()); fieldBytes.HasError())
                    return fieldBytes;
                for (std::size_t prior = 0; prior < index; ++prior) {
                    if (fields[index].first == fields[prior].first)
                        return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script struct contains duplicate fields."));
                }
                auto field = ValidateValueImpl(fields[index].second, context, depth + 1);
                if (field.HasError())
                    return field;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateValueImpl(const ScriptValue &value, ValidationContext &context, std::size_t depth) {
            if (depth > context.limits.maximumDepth)
                return Result<void>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, "Script value nesting depth was exceeded."));
            if (auto work = ConsumeWork(context); work.HasError())
                return work;

            using enum ScriptValue::Kind;
            switch (value.GetKind()) {
                case Null:
                case Boolean:
                case SignedInteger:
                case UnsignedInteger:
                    return Result<void>::Success();
                case Number:
                    if (value.AsNumber() == nullptr || !std::isfinite(*value.AsNumber()))
                        return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script number must be finite."));
                    return Result<void>::Success();
                case String:
                    return ValidateStringValue(value, context);
                case Bytes:
                    return AddRawBytes(context, value.AsBytes().size());
                case Handle:
                    return ValidateHandleValue(value, context);
                case Enum:
                    return ValidateEnumValue(value, context);
                case Array:
                    return ValidateArrayValue(value, context, depth);
                case Map:
                    return ValidateMapValue(value, context, depth);
                case Struct:
                    return ValidateStructValue(value, context, depth);
                case Count:
                    return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Unknown script value kind."));
            }
            return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Unknown script value kind."));
        }

        [[nodiscard]] Result<void> ValidateErrorImpl(const ScriptError &error, ValidationContext &context) {
            if (auto work = ConsumeWork(context); work.HasError())
                return work;
            if (!IsValidIdentity(error.domain, context.limits.maximumTypeIdentityBytes) ||
                !IsValidIdentity(error.code, context.limits.maximumTypeIdentityBytes))
                return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script error identity is malformed."));
            if (!IsValidUtf8ScalarSequence(error.message))
                return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script error message is not valid UTF-8."));
            if (error.message.size() > context.limits.maximumStringBytes)
                return Result<void>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, "Script error message bound was exceeded."));
            if (auto identityBytes = AddRawBytes(context, error.domain.size() + error.code.size() + error.message.size());
                identityBytes.HasError())
                return identityBytes;
            if (error.details.size() > context.limits.maximumErrorDetails)
                return Result<void>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, "Script error detail bound was exceeded."));
            if (auto count = AddElements(context, error.details.size()); count.HasError())
                return count;
            for (std::size_t index = 0; index < error.details.size(); ++index) {
                const auto &detail = error.details[index];
                if (!IsValidIdentity(detail.id, context.limits.maximumTypeIdentityBytes))
                    return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script error detail identity is malformed."));
                if (auto detailBytes = AddRawBytes(context, detail.id.size()); detailBytes.HasError())
                    return detailBytes;
                for (std::size_t prior = 0; prior < index; ++prior) {
                    if (detail.id == error.details[prior].id)
                        return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script error contains duplicate details."));
                }
                auto value = ValidateValueImpl(detail.value, context, 0);
                if (value.HasError())
                    return value;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] const ScriptExportTypeDescriptor *FindType(const ScriptExportDescriptor &descriptor, std::string_view id) noexcept {
            for (const auto &type : descriptor.types) {
                if (type.id == id)
                    return &type;
            }
            return nullptr;
        }

        [[nodiscard]] const ScriptExportFieldDescriptor *FindField(const ScriptExportTypeDescriptor &type, std::string_view id) noexcept {
            for (const auto &field : type.fields) {
                if (field.id == id)
                    return &field;
            }
            return nullptr;
        }

        [[nodiscard]] const ScriptExportEnumValueDescriptor *FindEnumValue(const ScriptExportTypeDescriptor &type,
                                                                           std::int64_t value) noexcept {
            for (const auto &enumerator : type.enumValues) {
                if (enumerator.value == value)
                    return &enumerator;
            }
            return nullptr;
        }

        [[nodiscard]] Result<void> ValidateValueForTypeImpl(const ScriptValue &value, const ScriptExportTypeReference &type,
                                                            const ScriptExportDescriptor &descriptor, const ScriptValueLimits &limits,
                                                            std::size_t depth, std::vector<std::string> &activeTypes);

        [[nodiscard]] Result<void> ValidateNamedEnum(const ScriptValue &value, const ScriptExportTypeDescriptor &named) {
            const auto *enumerator = value.AsEnum();
            return enumerator != nullptr && enumerator->type == named.id && FindEnumValue(named, enumerator->value) != nullptr
                       ? Result<void>::Success()
                       : TypeMismatch<void>("Script enum value does not match its declared type.");
        }

        [[nodiscard]] Result<void> ValidateNamedStruct(const ScriptValue &value, const ScriptExportTypeDescriptor &named,
                                                       const ScriptExportDescriptor &descriptor, const ScriptValueLimits &limits,
                                                       std::size_t depth, std::vector<std::string> &activeTypes) {
            if (value.GetKind() != ScriptValue::Kind::Struct || value.StructType() != named.id)
                return TypeMismatch<void>("Script struct value does not match its declared type.");
            const auto fields = value.AsStruct();
            for (const auto &[fieldId, fieldValue] : fields) {
                const auto *declaration = FindField(named, fieldId);
                if (declaration == nullptr)
                    return TypeMismatch<void>("Script struct contains an undeclared field.");
                auto result = ValidateValueForTypeImpl(fieldValue, declaration->type, descriptor, limits, depth + 1, activeTypes);
                if (result.HasError())
                    return result;
            }
            for (const auto &declaration : named.fields) {
                const auto found = std::ranges::find_if(fields, [&declaration](const auto &field) {
                    return field.first == declaration.id;
                });
                if (found == fields.end() && declaration.requirement == ScriptExportParameterRequirement::Required)
                    return TypeMismatch<void>("Script struct is missing a required field.");
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateNamedArray(const ScriptValue &value, const ScriptExportTypeDescriptor &named,
                                                      const ScriptExportDescriptor &descriptor, const ScriptValueLimits &limits,
                                                      std::size_t depth, std::vector<std::string> &activeTypes) {
            if (value.GetKind() != ScriptValue::Kind::Array || value.AsArray().size() > named.maximumElements)
                return TypeMismatch<void>("Script array value does not match its declared bound.");
            for (const auto &element : value.AsArray()) {
                auto result = ValidateValueForTypeImpl(element, named.elementType, descriptor, limits, depth + 1, activeTypes);
                if (result.HasError())
                    return result;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateNamedMap(const ScriptValue &value, const ScriptExportTypeDescriptor &named,
                                                    const ScriptExportDescriptor &descriptor, const ScriptValueLimits &limits,
                                                    std::size_t depth, std::vector<std::string> &activeTypes) {
            if (value.GetKind() != ScriptValue::Kind::Map || value.AsMap().size() > named.maximumElements)
                return TypeMismatch<void>("Script map value does not match its declared bound.");
            for (const auto &[keyValue, mappedValue] : value.AsMap()) {
                if (auto key = ValidateValueForTypeImpl(keyValue, named.keyType, descriptor, limits, depth + 1, activeTypes);
                    key.HasError())
                    return key;
                if (auto mapped = ValidateValueForTypeImpl(mappedValue, named.valueType, descriptor, limits, depth + 1, activeTypes);
                    mapped.HasError())
                    return mapped;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateNamedValue(const ScriptValue &value, std::string_view typeId,
                                                      const ScriptExportDescriptor &descriptor, const ScriptValueLimits &limits,
                                                      std::size_t depth, std::vector<std::string> &activeTypes) {
            if (depth > limits.maximumDepth)
                return Result<void>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, "Script type nesting depth was exceeded."));
            const auto *named = FindType(descriptor, typeId);
            if (named == nullptr)
                return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script named type is not declared."));
            if (std::ranges::find(activeTypes, named->id) != activeTypes.end())
                return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script named type reference is cyclic."));
            activeTypes.emplace_back(named->id);

            Result<void> result = Result<void>::Success();
            using enum ScriptExportNamedTypeKind;
            switch (named->kind) {
                case Enum:
                    result = ValidateNamedEnum(value, *named);
                    break;
                case Struct:
                    result = ValidateNamedStruct(value, *named, descriptor, limits, depth, activeTypes);
                    break;
                case Array:
                    result = ValidateNamedArray(value, *named, descriptor, limits, depth, activeTypes);
                    break;
                case Map:
                    result = ValidateNamedMap(value, *named, descriptor, limits, depth, activeTypes);
                    break;
                case Count:
                    result = Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script named type kind is invalid."));
                    break;
            }
            activeTypes.pop_back();
            return result;
        }

        [[nodiscard]] Result<void> ValidateValueForTypeImpl(const ScriptValue &value, const ScriptExportTypeReference &type,
                                                            const ScriptExportDescriptor &descriptor, const ScriptValueLimits &limits,
                                                            std::size_t depth, std::vector<std::string> &activeTypes) {
            if (value.GetKind() == ScriptValue::Kind::Null)
                return type.nullability == ScriptExportNullability::Nullable ? Result<void>::Success()
                                                                             : TypeMismatch<void>("A non-null script value was null.");
            if (type.nullability == ScriptExportNullability::Count)
                return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script type nullability is invalid."));
            if (type.primitive == ScriptExportPrimitiveKind::Count) {
                if (type.namedType.empty())
                    return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script named type identity is missing."));
                return ValidateNamedValue(value, type.namedType, descriptor, limits, depth, activeTypes);
            }
            if (!type.namedType.empty())
                return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Primitive script type carries a named identity."));

            const auto kind = value.GetKind();
            bool matches = false;
            switch (type.primitive) {
                case ScriptExportPrimitiveKind::Boolean:
                    matches = kind == ScriptValue::Kind::Boolean;
                    break;
                case ScriptExportPrimitiveKind::SignedInteger:
                    matches = kind == ScriptValue::Kind::SignedInteger;
                    break;
                case ScriptExportPrimitiveKind::UnsignedInteger:
                    matches = kind == ScriptValue::Kind::UnsignedInteger;
                    break;
                case ScriptExportPrimitiveKind::Number:
                    matches = kind == ScriptValue::Kind::Number;
                    break;
                case ScriptExportPrimitiveKind::String:
                    matches = kind == ScriptValue::Kind::String;
                    break;
                case ScriptExportPrimitiveKind::Bytes:
                    matches = kind == ScriptValue::Kind::Bytes;
                    break;
                case ScriptExportPrimitiveKind::Handle:
                    matches = kind == ScriptValue::Kind::Handle;
                    break;
                case ScriptExportPrimitiveKind::Count:
                    break;
            }
            return matches ? Result<void>::Success() : TypeMismatch<void>("Script value kind does not match its declared primitive.");
        }
    }  // namespace

    /** @copydoc ValidateScriptValue */
    Result<void> ValidateScriptValue(const ScriptValue &value, const ScriptValueLimits &limits) {
        ValidationContext context{limits};
        return ValidateValueImpl(value, context, 0);
    }

    /** @copydoc ValidateScriptError */
    Result<void> ValidateScriptError(const ScriptError &error, const ScriptValueLimits &limits) {
        ValidationContext context{limits};
        return ValidateErrorImpl(error, context);
    }

    /** @copydoc ValidateScriptCallResult */
    Result<void> ValidateScriptCallResult(const ScriptCallResult &result, const ScriptValueLimits &limits) {
        ValidationContext context{limits};
        if (result.error.has_value()) {
            if (!result.values.empty())
                return Result<void>::Failure(
                    MakeBoundaryError(ScriptCallResultInvalid, "A failed script result cannot carry success values."));
            return ValidateErrorImpl(*result.error, context);
        }
        if (auto count = AddElements(context, result.values.size()); count.HasError())
            return count;
        for (const auto &value : result.values) {
            if (auto valid = ValidateValueImpl(value, context, 0); valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidateScriptValueForType */
    Result<void> ValidateScriptValueForType(const ScriptValue &value, const ScriptExportTypeReference &type,
                                            const ScriptExportDescriptor &descriptor, const ScriptValueLimits &limits) {
        if (auto structural = ValidateScriptValue(value, limits); structural.HasError())
            return structural;
        std::vector<std::string> activeTypes;
        return ValidateValueForTypeImpl(value, type, descriptor, limits, 0, activeTypes);
    }

    /** @copydoc ValidateScriptArguments */
    Result<void> ValidateScriptArguments(const ScriptExportFunctionDescriptor &function, std::span<const ScriptValue> arguments,
                                         const ScriptExportDescriptor &descriptor, const ScriptValueLimits &limits) {
        if (arguments.size() > function.parameters.size())
            return TypeMismatch<void>("Script call has more arguments than declared parameters.");
        for (std::size_t index = 0; index < function.parameters.size(); ++index) {
            if (index >= arguments.size()) {
                if (function.parameters[index].requirement == ScriptExportParameterRequirement::Required)
                    return TypeMismatch<void>("Script call is missing a required argument.");
                continue;
            }
            if (auto valid = ValidateScriptValueForType(arguments[index], function.parameters[index].type, descriptor, limits);
                valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidateScriptResult */
    Result<void> ValidateScriptResult(const ScriptExportFunctionDescriptor &function, const ScriptCallResult &result,
                                      const ScriptExportDescriptor &descriptor, const ScriptValueLimits &limits) {
        if (auto structural = ValidateScriptCallResult(result, limits); structural.HasError())
            return structural;
        if (result.IsFailure())
            return Result<void>::Success();
        if (result.values.size() != function.results.size())
            return TypeMismatch<void>("Script call result count does not match the declaration.");
        for (std::size_t index = 0; index < result.values.size(); ++index) {
            if (auto valid = ValidateScriptValueForType(result.values[index], function.results[index].type, descriptor, limits);
                valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }

    /** @copydoc MakeScriptError */
    Result<ScriptError> MakeScriptError(const Error &error, bool retryable, const ScriptValueLimits &limits) {
        ScriptError result{
            .domain = error.domain.Value(),
            .code = error.code.Value(),
            .message = error.message,
            .retryable = retryable,
            .cancelled = false,
        };
        for (const Error *cause = error.cause.Get(); cause != nullptr; cause = cause->cause.Get()) {
            if (result.details.size() >= limits.maximumErrorDetails)
                return CapacityExceeded<ScriptError>("Script error cause bound was exceeded.");
            result.details.push_back({.id = "cause." + cause->domain.Value(), .value = ScriptValue::String(cause->code.Value())});
        }
        if (auto valid = ValidateScriptError(result, limits); valid.HasError())
            return Result<ScriptError>::Failure(valid.ErrorValue());
        return Result<ScriptError>::Success(std::move(result));
    }
}  // namespace Horo::Extensions
