#include "UiTemplateCodec.h"

namespace Horo::UiTemplates::Detail::Codec {
    namespace {
        using namespace Runtime::Ui;

        /** @brief Persists the full typed copy/range envelope, including absent bounds. */
        Json WriteLimits(const UiBindingValueLimits &value) {
            return Json::array({value.maximumBytes, value.maximumElements, Optional(value.minimumSigned), Optional(value.maximumSigned),
                                Optional(value.minimumUnsigned), Optional(value.maximumUnsigned), Optional(value.minimumScalar),
                                Optional(value.maximumScalar)});
        }

        /** @brief Checks directional ranges without converting absent bounds to zero. */
        template <typename T> bool Ordered(const std::optional<T> minimum, const std::optional<T> maximum) {
            return !minimum || !maximum || *minimum <= *maximum;
        }

        /** @brief Restores a finite binding envelope before allocating a fallback payload. */
        UiBindingValueLimits ReadLimits(const Json &value) {
            const auto &v = Tuple(value, 8);
            UiBindingValueLimits limits{Integer<std::uint32_t>(v[0]),
                                        Integer<std::uint32_t>(v[1]),
                                        OptionalInteger<std::int64_t>(v[2]),
                                        OptionalInteger<std::int64_t>(v[3]),
                                        OptionalInteger<std::uint64_t>(v[4]),
                                        OptionalInteger<std::uint64_t>(v[5]),
                                        OptionalScalar(v[6]),
                                        OptionalScalar(v[7])};
            if (limits.maximumBytes == 0 || limits.maximumBytes > MaximumUiBindingValueBytes || limits.maximumElements == 0 ||
                limits.maximumElements > MaximumUiBindingValueElements || !Ordered(limits.minimumSigned, limits.maximumSigned) ||
                !Ordered(limits.minimumUnsigned, limits.maximumUnsigned) || !Ordered(limits.minimumScalar, limits.maximumScalar))
                Reject();
            return limits;
        }

        /** @brief Encodes closed scalar/reference fallback metadata; no provider instance or callbacks are representable. */
        Json WriteFallback(const UiBindingValue &value) {
            if (value.valueless_by_exception())
                Reject();
            const Json payload = std::visit([]<typename T>(const T &item) -> Json {
                if constexpr (std::is_same_v<T, UiBindingNullValue>)
                    return nullptr;
                else if constexpr (std::is_same_v<T, UiBindingLocalizedMessage>)
                    return item.key;
                else if constexpr (std::is_same_v<T, UiBindingEnumValue> || std::is_same_v<T, UiBindingFlagsValue>)
                    return item.value;
                else if constexpr (std::is_same_v<T, UiBindingVector2>)
                    return Json::array({item.x, item.y});
                else if constexpr (std::is_same_v<T, UiBindingVector3>)
                    return Json::array({item.x, item.y, item.z});
                else if constexpr (std::is_same_v<T, UiBindingColor>)
                    return Json::array({item.r, item.g, item.b, item.a});
                else if constexpr (std::is_same_v<T, UiBindingReference>)
                    return Json::array({static_cast<std::uint8_t>(item.kind), item.value});
                else
                    return item;
            }, value);
            return {{"kind", value.index()}, {"value", payload}};
        }

        /** @brief Restores complete closed fallback alternatives without scalar/reference coercion. */
        UiBindingValue ReadFallback(const Json &value) {
            const auto envelope = ReadTaggedPayload(value);
            const auto &p = envelope.payload;
            switch (envelope.kind) {
                case 0:
                    if (!p.is_null())
                        Reject();
                    return UiBindingNullValue{};
                case 1:
                    return Boolean(p);
                case 2:
                    return Integer<std::int64_t>(p);
                case 3:
                    return Integer<std::uint64_t>(p);
                case 4:
                    return Scalar<double>(p);
                case 5:
                    return Text(p, MaximumUiBindingValueBytes);
                case 6:
                    return UiBindingLocalizedMessage{Text(p)};
                case 7:
                    return UiBindingEnumValue{Integer<std::uint64_t>(p)};
                case 8:
                    return UiBindingFlagsValue{Integer<std::uint64_t>(p)};
                case 9: {
                    const auto &v = Tuple(p, 2);
                    return UiBindingVector2{Scalar<float>(v[0]), Scalar<float>(v[1])};
                }
                case 10: {
                    const auto &v = Tuple(p, 3);
                    return UiBindingVector3{Scalar<float>(v[0]), Scalar<float>(v[1]), Scalar<float>(v[2])};
                }
                case 11: {
                    const auto &v = Tuple(p, 4);
                    return UiBindingColor{Scalar<float>(v[0]), Scalar<float>(v[1]), Scalar<float>(v[2]), Scalar<float>(v[3])};
                }
                case 12: {
                    const auto &v = Tuple(p, 2);
                    return UiBindingReference{Enum<UiBindingReferenceKind>(v[0], 3), Text(v[1])};
                }
                default:
                    Reject();
            }
        }

        /** @brief Restores exact converter identity and its directional capability. */
        UiBindingConverterDescriptor ReadConverter(const Json &value) {
            const auto &v = Tuple(value, 4);
            return {Take(UiBindingConverterId::Parse(Text(v[0]))), Enum<UiBindingValueType>(v[1], 17), Enum<UiBindingValueType>(v[2], 17),
                    Enum<UiBindingConverterMode>(v[3], 2)};
        }
    }  // namespace

    /** @copydoc WriteBinding */
    Json WriteBinding(const Runtime::Ui::UiBindingDescriptor &value) {
        Json converter = nullptr;
        if (value.converter) {
            const auto &c = *value.converter;
            converter = Json::array({c.id.Value(), static_cast<std::uint8_t>(c.sourceType), static_cast<std::uint8_t>(c.targetType),
                                     static_cast<std::uint8_t>(c.mode)});
        }
        return {{"id", Hex(value.id.Bytes())},
                {"source", Json::array({value.source.providerType.Value(), value.source.property.Value(), value.source.schema.major,
                                        value.source.schema.minimumMinor, value.source.propertySignatureFingerprint})},
                {"target", Json::array({Hex(value.target.element.Bytes()), static_cast<std::uint8_t>(value.target.property),
                                        WriteLimits(value.target.limits)})},
                {"direction", static_cast<std::uint8_t>(value.direction)},
                {"converter", std::move(converter)},
                {"fallback", value.fallback ? WriteFallback(*value.fallback) : Json(nullptr)},
                {"update", static_cast<std::uint8_t>(value.updatePolicy)},
                {"requirement", static_cast<std::uint8_t>(value.requirement)}};
    }

    /** @copydoc ReadBinding */
    Runtime::Ui::UiBindingDescriptor ReadBinding(const Json &value) {
        using namespace Runtime::Ui;
        Fields(value, {"id", "source", "target", "direction", "converter", "fallback", "update", "requirement"});
        const auto &s = Tuple(value.at("source"), 5);
        const auto &t = Tuple(value.at("target"), 3);
        UiBindingDescriptor output;
        output.id = Identity<UiBindingId>(value.at("id"));
        output.source = {Take(UiBindingProviderTypeId::Parse(Text(s[0]))),
                         Take(UiBindingPropertyId::Parse(Text(s[1]))),
                         {Integer<std::uint32_t>(s[2]), Integer<std::uint32_t>(s[3])},
                         Integer<std::uint64_t>(s[4])};
        output.target = {Identity<UiElementId>(t[0]), Enum<UiBindingTargetProperty>(t[1], 8), ReadLimits(t[2])};
        output.direction = Enum<UiBindingDirection>(value.at("direction"), 3);
        if (!value.at("converter").is_null())
            output.converter = ReadConverter(value.at("converter"));
        if (!value.at("fallback").is_null())
            output.fallback = ReadFallback(value.at("fallback"));
        output.updatePolicy = Enum<UiBindingUpdatePolicy>(value.at("update"), 3);
        output.requirement = Enum<UiBindingRequirement>(value.at("requirement"), 2);
        if (!output.source.IsValid() || !output.target.IsValid())
            Reject();
        return output;
    }
}  // namespace Horo::UiTemplates::Detail::Codec
