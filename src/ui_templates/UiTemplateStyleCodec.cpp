#include "UiTemplateCodec.h"

namespace Horo::UiTemplates::Detail::Codec {
    namespace {
        using namespace Runtime::Ui;

        /** @brief Encodes exact linear color and semantic role fields. */
        Json Color(const UiStyleColor &value) {
            if (!value.IsValid())
                Reject();
            return Json::array({value.red, value.green, value.blue, value.alpha, static_cast<std::uint8_t>(value.role)});
        }

        /** @brief Validates and restores one typed linear color. */
        UiStyleColor Color(const Json &value) {
            const auto &tuple = Tuple(value, 5);
            UiStyleColor color{Scalar<float>(tuple[0]), Scalar<float>(tuple[1]), Scalar<float>(tuple[2]), Scalar<float>(tuple[3]),
                               Enum<UiStyleColorRole>(tuple[4], 6)};
            if (!color.IsValid())
                Reject();
            return color;
        }

        /** @brief Encodes one finite literal through its owning style schema. */
        Json Literal(const UiStyleDimension &value) {
            return value.value;
        }

        Json Literal(const UiStyleColor &value) {
            return Color(value);
        }

        Json Literal(const UiStyleTypography &value) {
            if (!value.IsValid())
                Reject();
            return Json::array(
                {Hex(value.family.Bytes()), value.weight, value.stretch, value.style, value.size, value.lineHeight, value.letterSpacing});
        }

        Json Literal(const UiStyleImage &value) {
            if (!value.IsValid())
                Reject();
            return Json::array({Hex(value.asset.Bytes()), static_cast<std::uint8_t>(value.fit), value.nineSlice, Color(value.tint)});
        }

        Json Literal(const UiStyleShape &value) {
            if (!value.IsValid())
                Reject();
            return Json::array(
                {value.radius, value.borderWidth, value.outlineWidth, value.shadowOffsetX, value.shadowOffsetY, value.shadowBlur});
        }

        Json Literal(const UiStyleScalar &value) {
            if (!value.IsValid())
                Reject();
            return value.value;
        }

        Json Literal(const UiStyleEnumValue &value) {
            return value.value;
        }

        Json Literal(const UiStyleMotionReference &value) {
            return Json::array({value.durationMicroseconds, value.easing});
        }

        /** @brief Restores the exact stable family and bounded typography request. */
        UiStyleTypography Typography(const Json &value) {
            const auto &v = Tuple(value, 7);
            UiStyleTypography output{Asset(v[0]),
                                     Integer<std::uint16_t>(v[1]),
                                     Integer<std::uint8_t>(v[2]),
                                     Integer<std::uint8_t>(v[3]),
                                     Integer<std::int32_t>(v[4]),
                                     Integer<std::int32_t>(v[5]),
                                     Integer<std::int32_t>(v[6])};
            if (!output.IsValid())
                Reject();
            return output;
        }

        /** @brief Restores stable image identity, logical fit, insets and tint without a renderer dependency. */
        UiStyleImage Image(const Json &value) {
            const auto &v = Tuple(value, 4);
            const auto &insets = Tuple(v[2], 4);
            UiStyleImage output{Asset(v[0]),
                                Enum<UiStyleImageFit>(v[1], 4),
                                {Integer<std::int32_t>(insets[0]), Integer<std::int32_t>(insets[1]), Integer<std::int32_t>(insets[2]),
                                 Integer<std::int32_t>(insets[3])},
                                Color(v[3])};
            if (!output.IsValid())
                Reject();
            return output;
        }

        /** @brief Restores signed logical shape fields through the style validator. */
        UiStyleShape Shape(const Json &value) {
            const auto &v = Tuple(value, 6);
            UiStyleShape output{Integer<std::int32_t>(v[0]), Integer<std::int32_t>(v[1]), Integer<std::int32_t>(v[2]),
                                Integer<std::int32_t>(v[3]), Integer<std::int32_t>(v[4]), Integer<std::int32_t>(v[5])};
            if (!output.IsValid())
                Reject();
            return output;
        }
    }  // namespace

    /** @copydoc WriteStyle */
    Json WriteStyle(const Runtime::Ui::UiStyleValue &value) {
        if (value.valueless_by_exception())
            Reject();
        return {{"kind", value.index()}, {"value", std::visit([](const auto &literal) {
            return Literal(literal);
        }, value)}};
    }

    /** @copydoc ReadStyle */
    Runtime::Ui::UiStyleValue ReadStyle(const Json &value) {
        using namespace Runtime::Ui;
        Fields(value, {"kind", "value"});
        const auto &payload = value.at("value");
        switch (Integer<std::uint8_t>(value.at("kind"))) {
            case 0:
                return Color(payload);
            case 1:
                return UiStyleDimension{Integer<std::int32_t>(payload)};
            case 2:
                return Typography(payload);
            case 3:
                return Image(payload);
            case 4:
                return Shape(payload);
            case 5:
                return UiStyleScalar{Scalar<float>(payload)};
            case 6:
                return UiStyleEnumValue{Integer<std::uint32_t>(payload)};
            case 7: {
                const auto &v = Tuple(payload, 2);
                return UiStyleMotionReference{Integer<std::uint32_t>(v[0]), Integer<std::uint16_t>(v[1])};
            }
            default:
                Reject();
        }
    }
}  // namespace Horo::UiTemplates::Detail::Codec
