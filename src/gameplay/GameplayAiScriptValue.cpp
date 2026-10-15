#include "GameplayAiScriptValue.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace Horo::Gameplay::Detail {
    using Extensions::ScriptValue;

    namespace {
        /** @brief Finds one required logical field in an already ABI-validated struct. */
        const ScriptValue *Field(const ScriptValue &value, std::string_view id) {
            for (const auto &[name, field] : value.AsStruct())
                if (name == id)
                    return &field;
            return nullptr;
        }

        /** @brief Encodes one closed scalar using explicit exact-width logical fields. */
        ScriptValue Scalar(const AI::BlackboardScalarValue &value) {
            bool boolean{};
            std::int64_t integer{}, x{}, y{}, z{};
            double number{};
            std::uint64_t incarnation{}, slot{}, generation{};
            ScriptValue::Bytes asset;
            std::visit([&](const auto &input) {
                using T = std::decay_t<decltype(input)>;
                if constexpr (std::is_same_v<T, bool>)
                    boolean = input;
                else if constexpr (std::is_same_v<T, std::int64_t>)
                    integer = input;
                else if constexpr (std::is_same_v<T, double>)
                    number = input;
                else if constexpr (std::is_same_v<T, AI::BlackboardStoredEntityReference>) {
                    incarnation = input.sceneIncarnation;
                    slot = input.slot;
                    generation = input.generation;
                } else if constexpr (std::is_same_v<T, AI::BlackboardStoredAssetReference>) {
                    for (auto byte : input.canonicalBytes)
                        asset.push_back(static_cast<std::byte>(byte));
                } else {
                    const auto mm = input.Millimeters();
                    x = mm[0];
                    y = mm[1];
                    z = mm[2];
                }
            }, value);
            return ScriptValue::Struct("ai_scalar", {{"kind", ScriptValue{static_cast<std::uint64_t>(value.index())}},
                                                     {"boolean", ScriptValue{boolean}},
                                                     {"integer", ScriptValue{integer}},
                                                     {"number", ScriptValue{number}},
                                                     {"incarnation", ScriptValue{incarnation}},
                                                     {"slot", ScriptValue{slot}},
                                                     {"generation", ScriptValue{generation}},
                                                     {"asset", ScriptValue{std::move(asset)}},
                                                     {"x", ScriptValue{x}},
                                                     {"y", ScriptValue{y}},
                                                     {"z", ScriptValue{z}}});
        }

        /** @brief Validates and reconstructs the exact Entity logical payload. */
        Result<AI::BlackboardScalarValue> ParseEntity(const ScriptValue &value) {
            const auto bad = [] {
                return Result<AI::BlackboardScalarValue>::Failure(MakeError(AI::AIErrors::BlackboardValueInvalid));
            };

            auto *i = Field(value, "incarnation"), *s = Field(value, "slot"), *g = Field(value, "generation");
            if (!i || !s || !g || !i->AsUnsignedInteger() || !s->AsUnsignedInteger() || !g->AsUnsignedInteger() ||
                *s->AsUnsignedInteger() > std::numeric_limits<std::uint32_t>::max() ||
                *g->AsUnsignedInteger() > std::numeric_limits<std::uint32_t>::max())
                return bad();
            AI::BlackboardStoredEntityReference entity{*i->AsUnsignedInteger(), static_cast<std::uint32_t>(*s->AsUnsignedInteger()),
                                                       static_cast<std::uint32_t>(*g->AsUnsignedInteger())};
            if (!entity.IsValid())
                return bad();
            return Result<AI::BlackboardScalarValue>::Success(entity);
        }

        /** @brief Validates and reconstructs the exact Asset logical payload. */
        Result<AI::BlackboardScalarValue> ParseAsset(const ScriptValue &value) {
            const auto bad = [] {
                return Result<AI::BlackboardScalarValue>::Failure(MakeError(AI::AIErrors::BlackboardValueInvalid));
            };

            auto *v = Field(value, "asset");
            if (!v || v->GetKind() != ScriptValue::Kind::Bytes || v->AsBytes().size() != 16)
                return bad();
            AI::BlackboardStoredAssetReference asset;
            for (std::size_t i = 0; i < 16; ++i)
                asset.canonicalBytes[i] = std::to_integer<std::uint8_t>(v->AsBytes()[i]);
            if (!asset.IsValid())
                return bad();
            return Result<AI::BlackboardScalarValue>::Success(asset);
        }

        /** @brief Validates and reconstructs the exact WorldCoordinate logical payload. */
        Result<AI::BlackboardScalarValue> ParseWorldCoordinate(const ScriptValue &value) {
            const auto bad = [] {
                return Result<AI::BlackboardScalarValue>::Failure(MakeError(AI::AIErrors::BlackboardValueInvalid));
            };

            auto *x = Field(value, "x"), *y = Field(value, "y"), *z = Field(value, "z");
            if (!x || !y || !z || !x->AsSignedInteger() || !y->AsSignedInteger() || !z->AsSignedInteger())
                return bad();
            return Result<AI::BlackboardScalarValue>::Success(
                Math::WorldCoordinate64::FromMillimeters(*x->AsSignedInteger(), *y->AsSignedInteger(), *z->AsSignedInteger()));
        }

        /** @brief Validates required scalar field types before exact reconstruction. */
        Result<AI::BlackboardScalarValue> ParseScalar(const ScriptValue &value) {
            const auto bad = [] {
                return Result<AI::BlackboardScalarValue>::Failure(MakeError(AI::AIErrors::BlackboardValueInvalid));
            };
            if (value.StructType() != "ai_scalar")
                return bad();
            const auto *kind = Field(value, "kind");
            if (!kind || !kind->AsUnsignedInteger())
                return bad();
            const auto payload = [&](std::string_view id) {
                return Field(value, id);
            };
            switch (*kind->AsUnsignedInteger()) {
                case 0: {
                    auto *v = payload("boolean");
                    if (!v || !v->AsBoolean())
                        return bad();
                    return Result<AI::BlackboardScalarValue>::Success(*v->AsBoolean());
                }
                case 1: {
                    auto *v = payload("integer");
                    if (!v || !v->AsSignedInteger())
                        return bad();
                    return Result<AI::BlackboardScalarValue>::Success(*v->AsSignedInteger());
                }
                case 2: {
                    auto *v = payload("number");
                    if (!v || !v->AsNumber() || !std::isfinite(*v->AsNumber()))
                        return bad();
                    return Result<AI::BlackboardScalarValue>::Success(*v->AsNumber());
                }
                case 3:
                    return ParseEntity(value);
                case 4:
                    return ParseAsset(value);
                case 5:
                    return ParseWorldCoordinate(value);
                default:
                    return bad();
            }
        }

        /** @brief Decodes a finite homogeneous collection before schema-specific admission. */
        Result<AI::BlackboardValue> ParseCollection(const ScriptValue &value) {
            const auto bad = [] {
                return Result<AI::BlackboardValue>::Failure(MakeError(AI::AIErrors::BlackboardValueInvalid));
            };

            const auto *type = Field(value, "element_kind"), *elements = Field(value, "elements");
            if (!type || !elements || !type->AsUnsignedInteger() ||
                *type->AsUnsignedInteger() >= static_cast<std::uint64_t>(AI::BlackboardValueKind::Count) ||
                elements->GetKind() != ScriptValue::Kind::Array || elements->AsArray().size() > AI::MaximumBlackboardCollectionElements)
                return bad();
            AI::BlackboardCollectionValue collection;
            collection.elementKind = static_cast<AI::BlackboardValueKind>(*type->AsUnsignedInteger());
            for (const auto &item : elements->AsArray()) {
                auto scalar = ParseScalar(item);
                if (scalar.HasError())
                    return Result<AI::BlackboardValue>::Failure(scalar.ErrorValue());
                if (scalar.Value().index() != static_cast<std::size_t>(collection.elementKind))
                    return bad();
                collection.elements[collection.count++] = std::move(scalar).Value();
            }
            return Result<AI::BlackboardValue>::Success(collection);
        }

        /** @brief Preserves bounded unknown canonical bytes as explicit opaque logical fields. */
        Result<AI::BlackboardValue> ParseOpaque(const ScriptValue &value) {
            const auto bad = [] {
                return Result<AI::BlackboardValue>::Failure(MakeError(AI::AIErrors::BlackboardValueInvalid));
            };
            const auto *version = Field(value, "opaque_version"), *type = Field(value, "opaque_type"), *bytes = Field(value, "opaque");
            if (!version || !type || !bytes || !version->AsUnsignedInteger() || !type->AsUnsignedInteger() ||
                *version->AsUnsignedInteger() == 0 || *version->AsUnsignedInteger() > std::numeric_limits<std::uint32_t>::max() ||
                *type->AsUnsignedInteger() == 0 || *type->AsUnsignedInteger() > std::numeric_limits<std::uint32_t>::max() ||
                bytes->GetKind() != ScriptValue::Kind::Bytes || bytes->AsBytes().size() > AI::MaximumBlackboardOpaqueBytes)
                return bad();
            AI::BlackboardOpaqueValue opaque;
            opaque.schemaVersion = static_cast<std::uint32_t>(*version->AsUnsignedInteger());
            opaque.serializedType = static_cast<std::uint32_t>(*type->AsUnsignedInteger());
            opaque.size = bytes->AsBytes().size();
            std::copy(bytes->AsBytes().begin(), bytes->AsBytes().end(), opaque.bytes.begin());
            return Result<AI::BlackboardValue>::Success(opaque);
        }
    }  // namespace

    /** @copydoc ToScriptBlackboardValue */
    ScriptValue ToScriptBlackboardValue(const AI::BlackboardValue &value) {
        AI::BlackboardScalarValue scalar{false};
        ScriptValue::ArrayElements elements;
        ScriptValue::Bytes opaque;
        std::uint64_t elementKind{}, version{}, type{};
        if (const auto *v = std::get_if<AI::BlackboardScalarValue>(&value))
            scalar = *v;
        else if (const auto *v = std::get_if<AI::BlackboardCollectionValue>(&value)) {
            elementKind = static_cast<std::uint64_t>(v->elementKind);
            for (std::size_t i = 0; i < v->count; ++i)
                elements.push_back(Scalar(v->elements[i]));
        } else {
            const auto &opaqueValue = std::get<AI::BlackboardOpaqueValue>(value);
            version = opaqueValue.schemaVersion;
            type = opaqueValue.serializedType;
            opaque.assign(opaqueValue.bytes.begin(), opaqueValue.bytes.begin() + opaqueValue.size);
        }
        return ScriptValue::Struct("ai_value", {{"kind", ScriptValue{static_cast<std::uint64_t>(value.index())}},
                                                {"scalar", Scalar(scalar)},
                                                {"element_kind", ScriptValue{elementKind}},
                                                {"elements", ScriptValue::Array(std::move(elements))},
                                                {"opaque_version", ScriptValue{version}},
                                                {"opaque_type", ScriptValue{type}},
                                                {"opaque", ScriptValue{std::move(opaque)}}});
    }

    /** @copydoc FromScriptBlackboardValue */
    Result<AI::BlackboardValue> FromScriptBlackboardValue(const ScriptValue &value) {
        const auto bad = [] {
            return Result<AI::BlackboardValue>::Failure(MakeError(AI::AIErrors::BlackboardValueInvalid));
        };
        const auto *kind = Field(value, "kind");
        if (value.StructType() != "ai_value" || !kind || !kind->AsUnsignedInteger())
            return bad();
        if (*kind->AsUnsignedInteger() == 0) {
            const auto *v = Field(value, "scalar");
            if (!v)
                return bad();
            auto scalar = ParseScalar(*v);
            if (scalar.HasError())
                return Result<AI::BlackboardValue>::Failure(scalar.ErrorValue());
            return Result<AI::BlackboardValue>::Success(std::move(scalar).Value());
        }
        if (*kind->AsUnsignedInteger() == 1)
            return ParseCollection(value);
        if (*kind->AsUnsignedInteger() == 2)
            return ParseOpaque(value);
        return bad();
    }
}  // namespace Horo::Gameplay::Detail
