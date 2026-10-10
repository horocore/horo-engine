#include "UiTemplateCodec.h"

namespace Horo::UiTemplates::Detail::Codec {
    /** @copydoc WriteReference */
    Json WriteReference(const Runtime::Ui::UiReference &value) {
        return {{"kind", static_cast<std::uint8_t>(value.kind)},
                {"element", Hex(value.element.Bytes())},
                {"canvas", Hex(value.canvas.Bytes())},
                {"document", Hex(value.document.Bytes())},
                {"asset", Hex(value.asset.Bytes())},
                {"type", value.expectedAssetType.Value()}};
    }

    /** @copydoc ReadReference */
    Runtime::Ui::UiReference ReadReference(const Json &value) {
        using namespace Runtime::Ui;
        Fields(value, {"kind", "element", "canvas", "document", "asset", "type"});
        const auto type = Text(value.at("type"));
        UiReference output{Enum<UiReferenceKind>(value.at("kind"), 4),
                           Identity<UiElementId>(value.at("element"), true),
                           Identity<UiCanvasId>(value.at("canvas"), true),
                           Identity<UiDocumentId>(value.at("document"), true),
                           Asset(value.at("asset")),
                           type.empty() ? Assets::AssetTypeId{} : Take(Assets::AssetTypeId::Parse(type))};
        const bool valid = output.kind == UiReferenceKind::Element    ? output.element.IsValid()
                           : output.kind == UiReferenceKind::Canvas   ? output.canvas.IsValid()
                           : output.kind == UiReferenceKind::Document ? output.document.IsValid()
                                                                      : output.asset.IsValid() && !output.expectedAssetType.Value().empty();
        if (!valid)
            Reject();
        return output;
    }

    /** @copydoc WriteValue */
    Json WriteValue(const UiTemplateValue &value) {
        using namespace Runtime::Ui;
        if (value.valueless_by_exception())
            Reject();
        Json payload = std::visit([]<typename T>(const T &item) -> Json {
            if constexpr (std::is_same_v<T, UiReference>)
                return WriteReference(item);
            else if constexpr (std::is_same_v<T, UiStyleValue>)
                return WriteStyle(item);
            else if constexpr (std::is_same_v<T, LocalizedMessageRef>)
                return WriteLocalized(item);
            else if constexpr (std::is_same_v<T, UiBindingDescriptor>)
                return WriteBinding(item);
            else if constexpr (std::is_same_v<T, double>) {
                if (!std::isfinite(item))
                    Reject();
                return item;
            } else if constexpr (std::is_same_v<T, std::string>) {
                if (!IsValidUtf8ScalarSequence(item))
                    Reject();
                return item;
            } else
                return item;
        }, value);
        return {{"kind", value.index()}, {"value", std::move(payload)}};
    }

    /** @copydoc ReadValue */
    UiTemplateValue ReadValue(const Json &value) {
        Fields(value, {"kind", "value"});
        const auto &p = value.at("value");
        switch (Integer<std::uint8_t>(value.at("kind"))) {
            case 0:
                return Boolean(p);
            case 1:
                return Integer<std::int64_t>(p);
            case 2:
                return Scalar<double>(p);
            case 3:
                return Text(p);
            case 4:
                return ReadReference(p);
            case 5:
                return ReadStyle(p);
            case 6:
                return ReadLocalized(p);
            case 7:
                return ReadBinding(p);
            default:
                Reject();
        }
    }
}  // namespace Horo::UiTemplates::Detail::Codec
