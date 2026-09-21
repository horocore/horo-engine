#include "Horo/Extensions/ScriptValue.h"

#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace Horo::Extensions {
    namespace {
        using namespace ExtensionErrors;

        constexpr std::uint8_t ValueMagic0 = 0x53;
        constexpr std::uint8_t ValueMagic1 = 0x56;
        constexpr std::uint8_t ValueCodecVersion = 1;
        constexpr std::uint8_t ResultMagic0 = 0x53;
        constexpr std::uint8_t ResultMagic1 = 0x52;
        constexpr std::uint8_t ResultCodecVersion = 1;

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

        [[nodiscard]] Result<void> ValidateValueImpl(const ScriptValue &value, ValidationContext &context, std::size_t depth) {
            if (depth > context.limits.maximumDepth)
                return Result<void>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, "Script value nesting depth was exceeded."));
            auto work = ConsumeWork(context);
            if (work.HasError())
                return work;

            using enum ScriptValue::Kind;
            switch (value.GetKind()) {
                case Null:
                    return Result<void>::Success();
                case Boolean:
                case SignedInteger:
                case UnsignedInteger:
                    return Result<void>::Success();
                case Number:
                    if (value.AsNumber() == nullptr || !std::isfinite(*value.AsNumber()))
                        return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script number must be finite."));
                    return Result<void>::Success();
                case String: {
                    const auto string = value.AsString();
                    if (!IsValidUtf8ScalarSequence(string))
                        return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script string is not valid UTF-8."));
                    if (string.size() > context.limits.maximumStringBytes)
                        return Result<void>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, "Script string bound was exceeded."));
                    return AddRawBytes(context, string.size());
                }
                case Bytes: {
                    const auto bytes = value.AsBytes();
                    return AddRawBytes(context, bytes.size());
                }
                case Handle: {
                    const auto *handle = value.AsHandle();
                    if (handle == nullptr || !handle->IsValid() || handle->type.empty() || !IsValidUtf8ScalarSequence(handle->type))
                        return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script handle identity is malformed."));
                    if (handle->type.size() > context.limits.maximumTypeIdentityBytes)
                        return Result<void>::Failure(
                            MakeBoundaryError(ScriptValueCapacityExceeded, "Script handle type identity is too large."));
                    return AddRawBytes(context, handle->type.size());
                }
                case Enum: {
                    const auto *enumerator = value.AsEnum();
                    if (enumerator == nullptr || !IsValidIdentity(enumerator->type, context.limits.maximumTypeIdentityBytes))
                        return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script enum identity is malformed."));
                    return AddRawBytes(context, enumerator->type.size());
                }
                case Array: {
                    const auto elements = value.AsArray();
                    if (elements.size() > context.limits.maximumArrayElements)
                        return Result<void>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, "Script array bound was exceeded."));
                    auto count = AddElements(context, elements.size());
                    if (count.HasError())
                        return count;
                    for (const auto &element : elements) {
                        auto result = ValidateValueImpl(element, context, depth + 1);
                        if (result.HasError())
                            return result;
                    }
                    return Result<void>::Success();
                }
                case Map: {
                    const auto entries = value.AsMap();
                    if (entries.size() > context.limits.maximumMapEntries)
                        return Result<void>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, "Script map bound was exceeded."));
                    auto count = AddElements(context, entries.size());
                    if (count.HasError())
                        return count;
                    for (std::size_t index = 0; index < entries.size(); ++index) {
                        if (!IsScalarMapKey(entries[index].first.GetKind()))
                            return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script map key is not a bounded scalar."));
                        for (std::size_t prior = 0; prior < index; ++prior) {
                            if (entries[index].first == entries[prior].first)
                                return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script map contains duplicate keys."));
                        }
                        auto key = ValidateValueImpl(entries[index].first, context, depth + 1);
                        if (key.HasError())
                            return key;
                        auto mapped = ValidateValueImpl(entries[index].second, context, depth + 1);
                        if (mapped.HasError())
                            return mapped;
                    }
                    return Result<void>::Success();
                }
                case Struct: {
                    const auto type = value.StructType();
                    if (!IsValidIdentity(type, context.limits.maximumTypeIdentityBytes))
                        return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script struct type identity is malformed."));
                    auto typeBytes = AddRawBytes(context, type.size());
                    if (typeBytes.HasError())
                        return typeBytes;
                    const auto fields = value.AsStruct();
                    if (fields.size() > context.limits.maximumStructFields)
                        return Result<void>::Failure(
                            MakeBoundaryError(ScriptValueCapacityExceeded, "Script struct field bound was exceeded."));
                    auto count = AddElements(context, fields.size());
                    if (count.HasError())
                        return count;
                    for (std::size_t index = 0; index < fields.size(); ++index) {
                        if (!IsValidIdentity(fields[index].first, context.limits.maximumTypeIdentityBytes))
                            return Result<void>::Failure(
                                MakeBoundaryError(ScriptValueInvalid, "Script struct field identity is malformed."));
                        auto fieldBytes = AddRawBytes(context, fields[index].first.size());
                        if (fieldBytes.HasError())
                            return fieldBytes;
                        for (std::size_t prior = 0; prior < index; ++prior) {
                            if (fields[index].first == fields[prior].first)
                                return Result<void>::Failure(
                                    MakeBoundaryError(ScriptValueInvalid, "Script struct contains duplicate fields."));
                        }
                        auto field = ValidateValueImpl(fields[index].second, context, depth + 1);
                        if (field.HasError())
                            return field;
                    }
                    return Result<void>::Success();
                }
                case Count:
                    return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Unknown script value kind."));
            }
            return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Unknown script value kind."));
        }

        [[nodiscard]] Result<void> ValidateErrorImpl(const ScriptError &error, ValidationContext &context) {
            auto work = ConsumeWork(context);
            if (work.HasError())
                return work;
            if (!IsValidIdentity(error.domain, context.limits.maximumTypeIdentityBytes) ||
                !IsValidIdentity(error.code, context.limits.maximumTypeIdentityBytes))
                return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script error identity is malformed."));
            if (!IsValidUtf8ScalarSequence(error.message))
                return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script error message is not valid UTF-8."));
            if (error.message.size() > context.limits.maximumStringBytes)
                return Result<void>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, "Script error message bound was exceeded."));
            auto identityBytes = AddRawBytes(context, error.domain.size() + error.code.size() + error.message.size());
            if (identityBytes.HasError())
                return identityBytes;
            if (error.details.size() > context.limits.maximumErrorDetails)
                return Result<void>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, "Script error detail bound was exceeded."));
            auto count = AddElements(context, error.details.size());
            if (count.HasError())
                return count;
            for (std::size_t index = 0; index < error.details.size(); ++index) {
                const auto &detail = error.details[index];
                if (!IsValidIdentity(detail.id, context.limits.maximumTypeIdentityBytes))
                    return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script error detail identity is malformed."));
                auto detailBytes = AddRawBytes(context, detail.id.size());
                if (detailBytes.HasError())
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

        [[nodiscard]] Result<void> ValidateNamedValue(const ScriptValue &value, std::string_view typeId,
                                                      const ScriptExportDescriptor &descriptor, const ScriptValueLimits &limits,
                                                      std::size_t depth, std::vector<std::string> &activeTypes) {
            if (depth > limits.maximumDepth)
                return Result<void>::Failure(MakeBoundaryError(ScriptValueCapacityExceeded, "Script type nesting depth was exceeded."));
            const auto *named = FindType(descriptor, typeId);
            if (named == nullptr)
                return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script named type is not declared."));
            if (std::find(activeTypes.begin(), activeTypes.end(), named->id) != activeTypes.end())
                return Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script named type reference is cyclic."));
            activeTypes.emplace_back(named->id);
            auto removeType = [&activeTypes]() noexcept {
                activeTypes.pop_back();
            };

            Result<void> result = Result<void>::Success();
            switch (named->kind) {
                case ScriptExportNamedTypeKind::Enum: {
                    const auto *enumerator = value.AsEnum();
                    if (enumerator == nullptr || enumerator->type != named->id || FindEnumValue(*named, enumerator->value) == nullptr)
                        result = TypeMismatch<void>("Script enum value does not match its declared type.");
                    break;
                }
                case ScriptExportNamedTypeKind::Struct: {
                    if (value.GetKind() != ScriptValue::Kind::Struct || value.StructType() != named->id) {
                        result = TypeMismatch<void>("Script struct value does not match its declared type.");
                        break;
                    }
                    const auto fields = value.AsStruct();
                    for (const auto &field : fields) {
                        const auto *declaration = FindField(*named, field.first);
                        if (declaration == nullptr) {
                            result = TypeMismatch<void>("Script struct contains an undeclared field.");
                            break;
                        }
                        result = ValidateValueForTypeImpl(field.second, declaration->type, descriptor, limits, depth + 1, activeTypes);
                        if (result.HasError())
                            break;
                    }
                    if (result.HasValue()) {
                        for (const auto &declaration : named->fields) {
                            bool present = false;
                            for (const auto &field : fields) {
                                if (field.first == declaration.id) {
                                    present = true;
                                    break;
                                }
                            }
                            if (!present && declaration.requirement == ScriptExportParameterRequirement::Required) {
                                result = TypeMismatch<void>("Script struct is missing a required field.");
                                break;
                            }
                        }
                    }
                    break;
                }
                case ScriptExportNamedTypeKind::Array: {
                    if (value.GetKind() != ScriptValue::Kind::Array || value.AsArray().size() > named->maximumElements) {
                        result = TypeMismatch<void>("Script array value does not match its declared bound.");
                        break;
                    }
                    for (const auto &element : value.AsArray()) {
                        result = ValidateValueForTypeImpl(element, named->elementType, descriptor, limits, depth + 1, activeTypes);
                        if (result.HasError())
                            break;
                    }
                    break;
                }
                case ScriptExportNamedTypeKind::Map: {
                    if (value.GetKind() != ScriptValue::Kind::Map || value.AsMap().size() > named->maximumElements) {
                        result = TypeMismatch<void>("Script map value does not match its declared bound.");
                        break;
                    }
                    for (const auto &entry : value.AsMap()) {
                        result = ValidateValueForTypeImpl(entry.first, named->keyType, descriptor, limits, depth + 1, activeTypes);
                        if (result.HasError())
                            break;
                        result = ValidateValueForTypeImpl(entry.second, named->valueType, descriptor, limits, depth + 1, activeTypes);
                        if (result.HasError())
                            break;
                    }
                    break;
                }
                case ScriptExportNamedTypeKind::Count:
                    result = Result<void>::Failure(MakeBoundaryError(ScriptValueInvalid, "Script named type kind is invalid."));
                    break;
            }
            removeType();
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

        struct Encoder final {
            std::vector<std::byte> &output;
            const ScriptValueLimits &limits;
            std::size_t work{};

            [[nodiscard]] bool CanAppend(std::size_t count) const noexcept {
                return count <= limits.maximumEncodedBytes - std::min(output.size(), limits.maximumEncodedBytes);
            }

            [[nodiscard]] bool Consume(std::size_t count = 1) noexcept {
                if (count > limits.maximumWorkUnits - std::min(work, limits.maximumWorkUnits))
                    return false;
                work += count;
                return true;
            }

            [[nodiscard]] bool AppendByte(std::uint8_t value) {
                if (!CanAppend(1))
                    return false;
                output.push_back(static_cast<std::byte>(value));
                return true;
            }

            [[nodiscard]] bool AppendBytes(std::span<const std::byte> bytes) {
                if (!CanAppend(bytes.size()))
                    return false;
                output.insert(output.end(), bytes.begin(), bytes.end());
                return true;
            }

            [[nodiscard]] bool AppendU64(std::uint64_t value) {
                if (!CanAppend(sizeof(value)))
                    return false;
                for (std::size_t index = 0; index < sizeof(value); ++index)
                    output.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
                return true;
            }

            [[nodiscard]] bool AppendString(std::string_view value) {
                return value.size() <= limits.maximumStringBytes && AppendU64(value.size()) &&
                       AppendBytes(std::as_bytes(std::span{value.data(), value.size()}));
            }

            [[nodiscard]] bool EncodeValue(const ScriptValue &value);
        };

        [[nodiscard]] bool Encoder::EncodeValue(const ScriptValue &value) {
            if (!Consume())
                return false;
            using enum ScriptValue::Kind;
            switch (value.GetKind()) {
                case Null:
                    return AppendByte(0);
                case Boolean:
                    return AppendByte(*value.AsBoolean() ? 2 : 1);
                case SignedInteger:
                    return AppendByte(3) && AppendU64(static_cast<std::uint64_t>(*value.AsSignedInteger()));
                case UnsignedInteger:
                    return AppendByte(4) && AppendU64(*value.AsUnsignedInteger());
                case Number:
                    return AppendByte(5) && AppendU64(std::bit_cast<std::uint64_t>(*value.AsNumber()));
                case String:
                    return AppendByte(6) && AppendString(value.AsString());
                case Bytes:
                    return AppendByte(7) && value.AsBytes().size() <= limits.maximumBytes && AppendU64(value.AsBytes().size()) &&
                           AppendBytes(value.AsBytes());
                case Handle: {
                    const auto &handle = *value.AsHandle();
                    return AppendByte(8) && AppendU64(handle.context.value) && AppendU64(handle.providerGeneration) &&
                           AppendU64(handle.value) && AppendU64(handle.generation) && AppendString(handle.type);
                }
                case Enum: {
                    const auto &enumerator = *value.AsEnum();
                    return AppendByte(9) && AppendString(enumerator.type) && AppendU64(static_cast<std::uint64_t>(enumerator.value));
                }
                case Array: {
                    const auto elements = value.AsArray();
                    if (!AppendByte(10) || !AppendU64(elements.size()))
                        return false;
                    for (const auto &element : elements) {
                        if (!EncodeValue(element))
                            return false;
                    }
                    return true;
                }
                case Map: {
                    struct EncodedEntry final {
                        std::vector<std::byte> key;
                        std::vector<std::byte> value;
                    };

                    std::vector<EncodedEntry> encoded;
                    encoded.reserve(value.AsMap().size());
                    for (const auto &entry : value.AsMap()) {
                        encoded.push_back({});
                        Encoder keyEncoder{encoded.back().key, limits, work};
                        if (!keyEncoder.EncodeValue(entry.first))
                            return false;
                        work = keyEncoder.work;
                        Encoder valueEncoder{encoded.back().value, limits, work};
                        if (!valueEncoder.EncodeValue(entry.second))
                            return false;
                        work = valueEncoder.work;
                    }
                    std::sort(encoded.begin(), encoded.end(), [](const EncodedEntry &left, const EncodedEntry &right) {
                        return std::lexicographical_compare(left.key.begin(), left.key.end(), right.key.begin(), right.key.end());
                    });
                    for (std::size_t index = 1; index < encoded.size(); ++index) {
                        if (encoded[index - 1].key == encoded[index].key)
                            return false;
                    }
                    if (!AppendByte(11) || !AppendU64(encoded.size()))
                        return false;
                    for (const auto &entry : encoded) {
                        if (!AppendBytes(entry.key) || !AppendBytes(entry.value))
                            return false;
                    }
                    return true;
                }
                case Struct: {
                    std::vector<std::size_t> order(value.AsStruct().size());
                    for (std::size_t index = 0; index < order.size(); ++index)
                        order[index] = index;
                    std::sort(order.begin(), order.end(), [&value](std::size_t left, std::size_t right) {
                        return value.AsStruct()[left].first < value.AsStruct()[right].first;
                    });
                    for (std::size_t index = 1; index < order.size(); ++index) {
                        if (value.AsStruct()[order[index - 1]].first == value.AsStruct()[order[index]].first)
                            return false;
                    }
                    if (!AppendByte(12) || !AppendString(value.StructType()) || !AppendU64(order.size()))
                        return false;
                    for (const auto index : order) {
                        if (!AppendString(value.AsStruct()[index].first) || !EncodeValue(value.AsStruct()[index].second))
                            return false;
                    }
                    return true;
                }
                case Count:
                    return false;
            }
            return false;
        }

        struct Decoder final {
            std::span<const std::byte> input;
            const ScriptValueLimits &limits;
            std::size_t position{};
            std::size_t work{};
            std::size_t elements{};
            std::size_t rawBytes{};

            [[nodiscard]] bool Consume(std::size_t count = 1) noexcept {
                if (count > limits.maximumWorkUnits - std::min(work, limits.maximumWorkUnits))
                    return false;
                work += count;
                return true;
            }

            [[nodiscard]] bool CanRead(std::size_t count) const noexcept {
                return count <= input.size() - std::min(position, input.size());
            }

            [[nodiscard]] bool ReadByte(std::uint8_t &value) noexcept {
                if (!CanRead(1))
                    return false;
                value = std::to_integer<std::uint8_t>(input[position++]);
                return true;
            }

            [[nodiscard]] bool ReadU64(std::uint64_t &value) noexcept {
                if (!CanRead(sizeof(value)))
                    return false;
                value = 0;
                for (std::size_t index = 0; index < sizeof(value); ++index)
                    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input[position++])) << (index * 8U);
                return true;
            }

            [[nodiscard]] bool ReadLength(std::uint64_t encoded, std::size_t maximum, std::size_t &length) const noexcept {
                if (encoded > maximum || encoded > std::numeric_limits<std::size_t>::max())
                    return false;
                length = static_cast<std::size_t>(encoded);
                return true;
            }

            [[nodiscard]] bool ReadString(std::string &value) {
                std::uint64_t encodedLength{};
                if (!ReadU64(encodedLength))
                    return false;
                std::size_t length{};
                if (!ReadLength(encodedLength, limits.maximumStringBytes, length) || !CanRead(length))
                    return false;
                value.assign(reinterpret_cast<const char *>(input.data() + position), length);
                position += length;
                if (length > limits.maximumBytes - std::min(rawBytes, limits.maximumBytes))
                    return false;
                rawBytes += length;
                return true;
            }

            [[nodiscard]] bool ReadBytes(ScriptValue::Bytes &value) {
                std::uint64_t encodedLength{};
                if (!ReadU64(encodedLength))
                    return false;
                std::size_t length{};
                if (!ReadLength(encodedLength, limits.maximumBytes, length) || !CanRead(length))
                    return false;
                value.assign(input.begin() + static_cast<std::ptrdiff_t>(position),
                             input.begin() + static_cast<std::ptrdiff_t>(position + length));
                position += length;
                if (length > limits.maximumBytes - std::min(rawBytes, limits.maximumBytes))
                    return false;
                rawBytes += length;
                return true;
            }

            [[nodiscard]] bool AddElements(std::size_t count) noexcept {
                if (count > limits.maximumElements - std::min(elements, limits.maximumElements))
                    return false;
                elements += count;
                return true;
            }

            [[nodiscard]] Result<ScriptValue> DecodeValue(std::size_t depth);
        };

        [[nodiscard]] Result<ScriptValue> Decoder::DecodeValue(std::size_t depth) {
            if (depth > limits.maximumDepth || !Consume())
                return CapacityExceeded<ScriptValue>("Script value decode depth or work bound was exceeded.");
            std::uint8_t tag{};
            if (!ReadByte(tag))
                return InvalidEncoding<ScriptValue>("Script value payload ended before its tag.");
            switch (tag) {
                case 0:
                    return Result<ScriptValue>::Success(ScriptValue::Null());
                case 1:
                    return Result<ScriptValue>::Success(ScriptValue{false});
                case 2:
                    return Result<ScriptValue>::Success(ScriptValue{true});
                case 3: {
                    std::uint64_t encoded{};
                    if (!ReadU64(encoded))
                        return InvalidEncoding<ScriptValue>("Script signed integer payload is truncated.");
                    return Result<ScriptValue>::Success(ScriptValue{static_cast<std::int64_t>(encoded)});
                }
                case 4: {
                    std::uint64_t encoded{};
                    if (!ReadU64(encoded))
                        return InvalidEncoding<ScriptValue>("Script unsigned integer payload is truncated.");
                    return Result<ScriptValue>::Success(ScriptValue{encoded});
                }
                case 5: {
                    std::uint64_t encoded{};
                    if (!ReadU64(encoded))
                        return InvalidEncoding<ScriptValue>("Script number payload is truncated.");
                    const double number = std::bit_cast<double>(encoded);
                    if (!std::isfinite(number))
                        return InvalidEncoding<ScriptValue>("Script number payload is not finite.");
                    return Result<ScriptValue>::Success(ScriptValue{number});
                }
                case 6: {
                    std::string value;
                    if (!ReadString(value))
                        return InvalidEncoding<ScriptValue>("Script string payload is truncated or oversized.");
                    return Result<ScriptValue>::Success(ScriptValue::String(value));
                }
                case 7: {
                    ScriptValue::Bytes value;
                    if (!ReadBytes(value))
                        return InvalidEncoding<ScriptValue>("Script bytes payload is truncated or oversized.");
                    return Result<ScriptValue>::Success(ScriptValue{std::move(value)});
                }
                case 8: {
                    ScriptHandle handle;
                    if (!ReadU64(handle.context.value) || !ReadU64(handle.providerGeneration) || !ReadU64(handle.value) ||
                        !ReadU64(handle.generation) || !ReadString(handle.type))
                        return InvalidEncoding<ScriptValue>("Script handle payload is truncated or malformed.");
                    return Result<ScriptValue>::Success(ScriptValue{std::move(handle)});
                }
                case 9: {
                    ScriptEnumValue enumerator;
                    std::uint64_t value{};
                    if (!ReadString(enumerator.type) || !ReadU64(value))
                        return InvalidEncoding<ScriptValue>("Script enum payload is truncated or malformed.");
                    enumerator.value = static_cast<std::int64_t>(value);
                    return Result<ScriptValue>::Success(ScriptValue{std::move(enumerator)});
                }
                case 10: {
                    std::uint64_t encodedCount{};
                    std::size_t count{};
                    if (!ReadU64(encodedCount) || !ReadLength(encodedCount, limits.maximumArrayElements, count) || !AddElements(count))
                        return CapacityExceeded<ScriptValue>("Script array element bound was exceeded.");
                    ScriptValue::ArrayElements values;
                    values.reserve(count);
                    for (std::size_t index = 0; index < count; ++index) {
                        auto element = DecodeValue(depth + 1);
                        if (element.HasError())
                            return element;
                        values.emplace_back(std::move(element).Value());
                    }
                    return Result<ScriptValue>::Success(ScriptValue::Array(std::move(values)));
                }
                case 11: {
                    std::uint64_t encodedCount{};
                    std::size_t count{};
                    if (!ReadU64(encodedCount) || !ReadLength(encodedCount, limits.maximumMapEntries, count) || !AddElements(count))
                        return CapacityExceeded<ScriptValue>("Script map entry bound was exceeded.");
                    ScriptValue::MapEntries values;
                    values.reserve(count);
                    std::size_t previousKeyStart = 0;
                    std::size_t previousKeyEnd = 0;
                    for (std::size_t index = 0; index < count; ++index) {
                        const std::size_t keyStart = position;
                        auto key = DecodeValue(depth + 1);
                        const std::size_t keyEnd = position;
                        if (key.HasError())
                            return key;
                        if (!IsScalarMapKey(key.Value().GetKind()))
                            return InvalidEncoding<ScriptValue>("Script map key is not a bounded scalar.");
                        if (index > 0) {
                            const auto previous = input.subspan(previousKeyStart, previousKeyEnd - previousKeyStart);
                            const auto current = input.subspan(keyStart, keyEnd - keyStart);
                            if (!std::lexicographical_compare(previous.begin(), previous.end(), current.begin(), current.end()))
                                return InvalidEncoding<ScriptValue>("Script map keys are not in canonical order.");
                        }
                        auto mapped = DecodeValue(depth + 1);
                        if (mapped.HasError())
                            return mapped;
                        values.emplace_back(std::move(key).Value(), std::move(mapped).Value());
                        previousKeyStart = keyStart;
                        previousKeyEnd = keyEnd;
                    }
                    return Result<ScriptValue>::Success(ScriptValue::Map(std::move(values)));
                }
                case 12: {
                    std::string type;
                    std::uint64_t encodedCount{};
                    std::size_t count{};
                    if (!ReadString(type) || !ReadU64(encodedCount) || !ReadLength(encodedCount, limits.maximumStructFields, count) ||
                        !AddElements(count))
                        return CapacityExceeded<ScriptValue>("Script struct field bound was exceeded.");
                    ScriptValue::StructFields fields;
                    fields.reserve(count);
                    std::string previousId;
                    for (std::size_t index = 0; index < count; ++index) {
                        std::string id;
                        if (!ReadString(id) || (index > 0 && !(previousId < id)))
                            return InvalidEncoding<ScriptValue>("Script struct fields are not in canonical order.");
                        auto field = DecodeValue(depth + 1);
                        if (field.HasError())
                            return field;
                        previousId = id;
                        fields.emplace_back(std::move(id), std::move(field).Value());
                    }
                    return Result<ScriptValue>::Success(ScriptValue::Struct(std::move(type), std::move(fields)));
                }
                default:
                    return InvalidEncoding<ScriptValue>("Script value tag is unknown.");
            }
        }

        [[nodiscard]] Result<void> ValidateValueForCodec(const ScriptValue &value, const ScriptValueLimits &limits) {
            ValidationContext context{limits};
            return ValidateValueImpl(value, context, 0);
        }

        [[nodiscard]] bool AppendResultString(Encoder &encoder, std::string_view value) {
            return encoder.AppendString(value);
        }

        [[nodiscard]] bool EncodeError(Encoder &encoder, const ScriptError &error) {
            if (!AppendResultString(encoder, error.domain) || !AppendResultString(encoder, error.code) ||
                !AppendResultString(encoder, error.message) || !encoder.AppendByte(error.retryable ? 1 : 0) ||
                !encoder.AppendByte(error.cancelled ? 1 : 0) || !encoder.AppendU64(error.details.size()))
                return false;
            for (const auto &detail : error.details) {
                if (!AppendResultString(encoder, detail.id) || !encoder.EncodeValue(detail.value))
                    return false;
            }
            return true;
        }

        [[nodiscard]] Result<ScriptError> DecodeError(Decoder &decoder) {
            ScriptError error;
            std::uint8_t retryable{};
            std::uint8_t cancelled{};
            std::uint64_t encodedCount{};
            std::size_t count{};
            if (!decoder.ReadString(error.domain) || !decoder.ReadString(error.code) || !decoder.ReadString(error.message) ||
                !decoder.ReadByte(retryable) || !decoder.ReadByte(cancelled) || !decoder.ReadU64(encodedCount) ||
                !decoder.ReadLength(encodedCount, decoder.limits.maximumErrorDetails, count) || !decoder.AddElements(count) ||
                retryable > 1 || cancelled > 1)
                return InvalidEncoding<ScriptError>("Script error payload is truncated or malformed.");
            error.retryable = retryable != 0;
            error.cancelled = cancelled != 0;
            error.details.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                ScriptErrorDetail detail;
                if (!decoder.ReadString(detail.id))
                    return InvalidEncoding<ScriptError>("Script error detail identity is truncated or malformed.");
                auto value = decoder.DecodeValue(0);
                if (value.HasError())
                    return Result<ScriptError>::Failure(value.ErrorValue());
                detail.value = std::move(value).Value();
                error.details.emplace_back(std::move(detail));
            }
            return Result<ScriptError>::Success(std::move(error));
        }
    }  // namespace

    /** @copydoc ScriptHandle::IsValid */
    bool ScriptHandle::IsValid() const noexcept {
        return context.IsValid() && providerGeneration != 0 && value != 0 && generation != 0 && !type.empty();
    }

    ScriptValue::ScriptValue() noexcept : value_(std::monostate{}) {}

    ScriptValue::ScriptValue(bool value) noexcept : value_(value) {}

    ScriptValue::ScriptValue(std::int64_t value) noexcept : value_(value) {}

    ScriptValue::ScriptValue(std::uint64_t value) noexcept : value_(value) {}

    ScriptValue::ScriptValue(double value) noexcept : value_(value) {}

    ScriptValue::ScriptValue(std::string value) : value_(std::move(value)) {}

    ScriptValue::ScriptValue(Bytes value) : value_(std::move(value)) {}

    ScriptValue::ScriptValue(ScriptHandle value) : value_(std::move(value)) {}

    ScriptValue::ScriptValue(ScriptEnumValue value) : value_(std::move(value)) {}

    ScriptValue::ScriptValue(Storage value) noexcept : value_(std::move(value)) {}

    /** @copydoc ScriptValue::Null */
    ScriptValue ScriptValue::Null() noexcept {
        return ScriptValue{};
    }

    /** @copydoc ScriptValue::String */
    ScriptValue ScriptValue::String(std::string_view value) {
        return ScriptValue{std::string{value}};
    }

    /** @copydoc ScriptValue::BytesValue */
    ScriptValue ScriptValue::BytesValue(std::span<const std::byte> value) {
        return ScriptValue{Bytes{value.begin(), value.end()}};
    }

    /** @copydoc ScriptValue::Array */
    ScriptValue ScriptValue::Array(ArrayElements values) {
        return ScriptValue{std::make_shared<const ScriptArray>(ScriptArray{std::move(values)})};
    }

    /** @copydoc ScriptValue::Map */
    ScriptValue ScriptValue::Map(MapEntries values) {
        return ScriptValue{std::make_shared<const ScriptMap>(ScriptMap{std::move(values)})};
    }

    /** @copydoc ScriptValue::Struct */
    ScriptValue ScriptValue::Struct(std::string type, StructFields fields) {
        return ScriptValue{std::make_shared<const ScriptStruct>(ScriptStruct{std::move(type), std::move(fields)})};
    }

    /** @copydoc ScriptValue::GetKind */
    ScriptValue::Kind ScriptValue::GetKind() const noexcept {
        switch (value_.index()) {
            case 0:
                return Kind::Null;
            case 1:
                return Kind::Boolean;
            case 2:
                return Kind::SignedInteger;
            case 3:
                return Kind::UnsignedInteger;
            case 4:
                return Kind::Number;
            case 5:
                return Kind::String;
            case 6:
                return Kind::Bytes;
            case 7:
                return Kind::Handle;
            case 8:
                return Kind::Enum;
            case 9:
                return Kind::Array;
            case 10:
                return Kind::Map;
            case 11:
                return Kind::Struct;
            default:
                return Kind::Count;
        }
    }

    /** @copydoc ScriptValue::AsBoolean */
    const bool *ScriptValue::AsBoolean() const noexcept {
        return std::get_if<bool>(&value_);
    }

    /** @copydoc ScriptValue::AsSignedInteger */
    const std::int64_t *ScriptValue::AsSignedInteger() const noexcept {
        return std::get_if<std::int64_t>(&value_);
    }

    /** @copydoc ScriptValue::AsUnsignedInteger */
    const std::uint64_t *ScriptValue::AsUnsignedInteger() const noexcept {
        return std::get_if<std::uint64_t>(&value_);
    }

    /** @copydoc ScriptValue::AsNumber */
    const double *ScriptValue::AsNumber() const noexcept {
        return std::get_if<double>(&value_);
    }

    /** @copydoc ScriptValue::AsString */
    std::string_view ScriptValue::AsString() const noexcept {
        if (const auto *value = std::get_if<std::string>(&value_))
            return *value;
        return {};
    }

    /** @copydoc ScriptValue::AsBytes */
    std::span<const std::byte> ScriptValue::AsBytes() const noexcept {
        if (const auto *value = std::get_if<Bytes>(&value_))
            return *value;
        return {};
    }

    /** @copydoc ScriptValue::AsHandle */
    const ScriptHandle *ScriptValue::AsHandle() const noexcept {
        return std::get_if<ScriptHandle>(&value_);
    }

    /** @copydoc ScriptValue::AsEnum */
    const ScriptEnumValue *ScriptValue::AsEnum() const noexcept {
        return std::get_if<ScriptEnumValue>(&value_);
    }

    /** @copydoc ScriptValue::AsArray */
    std::span<const ScriptValue> ScriptValue::AsArray() const noexcept {
        if (const auto *value = std::get_if<ArrayStorage>(&value_); value != nullptr && *value != nullptr)
            return (*value)->values;
        return {};
    }

    /** @copydoc ScriptValue::AsMap */
    std::span<const ScriptValue::MapEntry> ScriptValue::AsMap() const noexcept {
        if (const auto *value = std::get_if<MapStorage>(&value_); value != nullptr && *value != nullptr)
            return (*value)->values;
        return {};
    }

    /** @copydoc ScriptValue::StructType */
    std::string_view ScriptValue::StructType() const noexcept {
        if (const auto *value = std::get_if<StructStorage>(&value_); value != nullptr && *value != nullptr)
            return (*value)->type;
        return {};
    }

    /** @copydoc ScriptValue::AsStruct */
    std::span<const ScriptValue::StructField> ScriptValue::AsStruct() const noexcept {
        if (const auto *value = std::get_if<StructStorage>(&value_); value != nullptr && *value != nullptr)
            return (*value)->fields;
        return {};
    }

    /** @copydoc ScriptValue::IsNull */
    bool ScriptValue::IsNull() const noexcept {
        return GetKind() == Kind::Null;
    }

    /** @copydoc ScriptValue::operator== */
    bool ScriptValue::operator==(const ScriptValue &other) const noexcept {
        if (GetKind() != other.GetKind())
            return false;
        using enum Kind;
        switch (GetKind()) {
            case Null:
                return true;
            case Boolean:
                return *AsBoolean() == *other.AsBoolean();
            case SignedInteger:
                return *AsSignedInteger() == *other.AsSignedInteger();
            case UnsignedInteger:
                return *AsUnsignedInteger() == *other.AsUnsignedInteger();
            case Number:
                return *AsNumber() == *other.AsNumber();
            case String:
                return AsString() == other.AsString();
            case Bytes:
                return std::equal(AsBytes().begin(), AsBytes().end(), other.AsBytes().begin(), other.AsBytes().end());
            case Handle:
                return *AsHandle() == *other.AsHandle();
            case Enum:
                return *AsEnum() == *other.AsEnum();
            case Array:
                return std::equal(AsArray().begin(), AsArray().end(), other.AsArray().begin(), other.AsArray().end());
            case Map:
                if (AsMap().size() != other.AsMap().size())
                    return false;
                for (const auto &left : AsMap()) {
                    const auto found = std::find_if(other.AsMap().begin(), other.AsMap().end(), [&left](const MapEntry &right) {
                        return left.first == right.first;
                    });
                    if (found == other.AsMap().end() || left.second != found->second)
                        return false;
                }
                return true;
            case Struct:
                if (StructType() != other.StructType() || AsStruct().size() != other.AsStruct().size())
                    return false;
                for (const auto &left : AsStruct()) {
                    const auto found = std::find_if(other.AsStruct().begin(), other.AsStruct().end(), [&left](const StructField &right) {
                        return left.first == right.first;
                    });
                    if (found == other.AsStruct().end() || left.second != found->second)
                        return false;
                }
                return true;
            case Count:
                return false;
        }
        return false;
    }

    /** @copydoc ScriptCallResult::Success */
    ScriptCallResult ScriptCallResult::Success(std::vector<ScriptValue> values) {
        return ScriptCallResult{.values = std::move(values), .error = std::nullopt};
    }

    /** @copydoc ScriptCallResult::Failure */
    ScriptCallResult ScriptCallResult::Failure(ScriptError error) {
        return ScriptCallResult{.values = {}, .error = std::move(error)};
    }

    /** @copydoc ScriptCallResult::IsSuccess */
    bool ScriptCallResult::IsSuccess() const noexcept {
        return !error.has_value();
    }

    /** @copydoc ScriptCallResult::IsFailure */
    bool ScriptCallResult::IsFailure() const noexcept {
        return error.has_value();
    }

    /** @copydoc ValidateScriptValue */
    Result<void> ValidateScriptValue(const ScriptValue &value, const ScriptValueLimits &limits) {
        return ValidateValueForCodec(value, limits);
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
        auto count = AddElements(context, result.values.size());
        if (count.HasError())
            return count;
        for (const auto &value : result.values) {
            auto valid = ValidateValueImpl(value, context, 0);
            if (valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidateScriptValueForType */
    Result<void> ValidateScriptValueForType(const ScriptValue &value, const ScriptExportTypeReference &type,
                                            const ScriptExportDescriptor &descriptor, const ScriptValueLimits &limits) {
        auto structural = ValidateScriptValue(value, limits);
        if (structural.HasError())
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
            auto valid = ValidateScriptValueForType(arguments[index], function.parameters[index].type, descriptor, limits);
            if (valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidateScriptResult */
    Result<void> ValidateScriptResult(const ScriptExportFunctionDescriptor &function, const ScriptCallResult &result,
                                      const ScriptExportDescriptor &descriptor, const ScriptValueLimits &limits) {
        auto structural = ValidateScriptCallResult(result, limits);
        if (structural.HasError())
            return structural;
        if (result.IsFailure())
            return Result<void>::Success();
        if (result.values.size() != function.results.size())
            return TypeMismatch<void>("Script call result count does not match the declaration.");
        for (std::size_t index = 0; index < result.values.size(); ++index) {
            auto valid = ValidateScriptValueForType(result.values[index], function.results[index].type, descriptor, limits);
            if (valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }

    /** @copydoc EncodeScriptValue */
    Result<std::vector<std::byte>> EncodeScriptValue(const ScriptValue &value, const ScriptValueLimits &limits) {
        auto valid = ValidateScriptValue(value, limits);
        if (valid.HasError())
            return Result<std::vector<std::byte>>::Failure(valid.ErrorValue());
        std::vector<std::byte> output;
        output.reserve(std::min<std::size_t>(limits.maximumEncodedBytes, 256));
        Encoder encoder{output, limits};
        if (!encoder.AppendByte(ValueMagic0) || !encoder.AppendByte(ValueMagic1) || !encoder.AppendByte(ValueCodecVersion) ||
            !encoder.EncodeValue(value))
            return CapacityExceeded<std::vector<std::byte>>("Script value encoded payload bound was exceeded.");
        return Result<std::vector<std::byte>>::Success(std::move(output));
    }

    /** @copydoc DecodeScriptValue */
    Result<ScriptValue> DecodeScriptValue(std::span<const std::byte> bytes, const ScriptValueLimits &limits) {
        if (bytes.size() > limits.maximumEncodedBytes)
            return CapacityExceeded<ScriptValue>("Script value encoded payload bound was exceeded.");
        Decoder decoder{bytes, limits};
        std::uint8_t magic0{};
        std::uint8_t magic1{};
        std::uint8_t version{};
        if (!decoder.ReadByte(magic0) || !decoder.ReadByte(magic1) || !decoder.ReadByte(version) || magic0 != ValueMagic0 ||
            magic1 != ValueMagic1 || version != ValueCodecVersion)
            return InvalidEncoding<ScriptValue>("Script value codec header is invalid.");
        auto value = decoder.DecodeValue(0);
        if (value.HasError())
            return value;
        if (decoder.position != bytes.size())
            return InvalidEncoding<ScriptValue>("Script value payload has trailing bytes.");
        auto valid = ValidateScriptValue(value.Value(), limits);
        if (valid.HasError())
            return Result<ScriptValue>::Failure(valid.ErrorValue());
        return value;
    }

    /** @copydoc EncodeScriptCallResult */
    Result<std::vector<std::byte>> EncodeScriptCallResult(const ScriptCallResult &result, const ScriptValueLimits &limits) {
        auto valid = ValidateScriptCallResult(result, limits);
        if (valid.HasError())
            return Result<std::vector<std::byte>>::Failure(valid.ErrorValue());
        std::vector<std::byte> output;
        output.reserve(std::min<std::size_t>(limits.maximumEncodedBytes, 256));
        Encoder encoder{output, limits};
        if (!encoder.AppendByte(ResultMagic0) || !encoder.AppendByte(ResultMagic1) || !encoder.AppendByte(ResultCodecVersion) ||
            !encoder.AppendByte(result.IsSuccess() ? 0 : 1))
            return CapacityExceeded<std::vector<std::byte>>("Script result encoded payload bound was exceeded.");
        if (result.IsSuccess()) {
            if (!encoder.AppendU64(result.values.size()))
                return CapacityExceeded<std::vector<std::byte>>("Script result value count exceeded its encoded bound.");
            for (const auto &value : result.values) {
                if (!encoder.EncodeValue(value))
                    return CapacityExceeded<std::vector<std::byte>>("Script result encoded payload bound was exceeded.");
            }
        } else if (!EncodeError(encoder, *result.error)) {
            return CapacityExceeded<std::vector<std::byte>>("Script result error payload exceeded its encoded bound.");
        }
        return Result<std::vector<std::byte>>::Success(std::move(output));
    }

    /** @copydoc DecodeScriptCallResult */
    Result<ScriptCallResult> DecodeScriptCallResult(std::span<const std::byte> bytes, const ScriptValueLimits &limits) {
        if (bytes.size() > limits.maximumEncodedBytes)
            return CapacityExceeded<ScriptCallResult>("Script result encoded payload bound was exceeded.");
        Decoder decoder{bytes, limits};
        std::uint8_t magic0{};
        std::uint8_t magic1{};
        std::uint8_t version{};
        std::uint8_t kind{};
        if (!decoder.ReadByte(magic0) || !decoder.ReadByte(magic1) || !decoder.ReadByte(version) || !decoder.ReadByte(kind) ||
            magic0 != ResultMagic0 || magic1 != ResultMagic1 || version != ResultCodecVersion || kind > 1)
            return InvalidEncoding<ScriptCallResult>("Script result codec header is invalid.");
        ScriptCallResult result;
        if (kind == 0) {
            std::uint64_t encodedCount{};
            std::size_t count{};
            if (!decoder.ReadU64(encodedCount) || !decoder.ReadLength(encodedCount, limits.maximumElements, count) ||
                !decoder.AddElements(count))
                return CapacityExceeded<ScriptCallResult>("Script result value count exceeded its bound.");
            result.values.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                auto value = decoder.DecodeValue(0);
                if (value.HasError())
                    return Result<ScriptCallResult>::Failure(value.ErrorValue());
                result.values.emplace_back(std::move(value).Value());
            }
        } else {
            auto error = DecodeError(decoder);
            if (error.HasError())
                return Result<ScriptCallResult>::Failure(error.ErrorValue());
            result.error = std::move(error).Value();
        }
        if (decoder.position != bytes.size())
            return InvalidEncoding<ScriptCallResult>("Script result payload has trailing bytes.");
        auto valid = ValidateScriptCallResult(result, limits);
        if (valid.HasError())
            return Result<ScriptCallResult>::Failure(valid.ErrorValue());
        return Result<ScriptCallResult>::Success(std::move(result));
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
        auto valid = ValidateScriptError(result, limits);
        if (valid.HasError())
            return Result<ScriptError>::Failure(valid.ErrorValue());
        return Result<ScriptError>::Success(std::move(result));
    }
}  // namespace Horo::Extensions
