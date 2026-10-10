#include "UiTemplateCodec.h"

namespace Horo::UiTemplates::Detail::Codec {
    /** @copydoc WriteRich */
    Json WriteRich(const UiTemplateAuthoredDocument &source) {
        Json styles = Json::array(), texts = Json::array(), bindings = Json::array();
        for (const auto &style : source.styles) {
            const auto &assignment = style.assignment;
            Json value = assignment.value.referencesToken
                             ? Json{{"token", Hex(assignment.value.token.id.Bytes())}, {"asset", Hex(assignment.value.token.asset.Bytes())}}
                             : WriteStyle(assignment.value.literal);
            styles.push_back({{"element", Hex(style.element.Bytes())},
                              {"property", Hex(assignment.property.Bytes())},
                              {"sealed", assignment.sealed},
                              {"token", assignment.value.referencesToken},
                              {"value", std::move(value)}});
        }
        for (const auto &text : source.localizedTexts)
            texts.push_back(
                {{"element", Hex(text.element.Bytes())}, {"property", Hex(text.property.Bytes())}, {"value", WriteLocalized(text.text)}});
        for (const auto &binding : source.bindings)
            bindings.push_back(WriteBinding(binding));
        return {{"styles", std::move(styles)}, {"texts", std::move(texts)}, {"bindings", std::move(bindings)}};
    }

    /** @copydoc ReadRich */
    void ReadRich(const Json &value, UiTemplateAuthoredDocument &source, const UiTemplateLimits &limits) {
        using namespace Runtime::Ui;
        Fields(value, {"styles", "texts", "bindings"});
        const auto &styles = Array(value.at("styles"), limits.maximumProperties);
        const auto &texts = Array(value.at("texts"), limits.maximumProperties);
        const auto &bindings = Array(value.at("bindings"), MaximumUiBindingDescriptors);
        if (styles.size() > limits.maximumProperties - texts.size() ||
            bindings.size() > limits.maximumProperties - styles.size() - texts.size())
            Reject();
        source.styles.reserve(styles.size());
        source.localizedTexts.reserve(texts.size());
        source.bindings.reserve(bindings.size());
        for (const auto &style : styles) {
            Fields(style, {"element", "property", "sealed", "token", "value"});
            UiStyleValueSource literal;
            if (Boolean(style.at("token"))) {
                const auto &token = style.at("value");
                Fields(token, {"asset", "token"});
                literal = UiStyleValueSource::Token(
                    {Identity<RuntimeStyleAssetId>(token.at("asset")), Identity<UiStyleTokenId>(token.at("token"))});
            } else
                literal = UiStyleValueSource::Literal(ReadStyle(style.at("value")));
            source.styles.push_back({Identity<UiElementId>(style.at("element")),
                                     {Identity<UiStylePropertyId>(style.at("property")), std::move(literal), Boolean(style.at("sealed"))}});
        }
        for (const auto &text : texts) {
            Fields(text, {"element", "property", "value"});
            source.localizedTexts.push_back({Identity<UiElementId>(text.at("element")), ReadLocalized(text.at("value")),
                                             Identity<UiStylePropertyId>(text.at("property"))});
        }
        for (const auto &binding : bindings)
            source.bindings.push_back(ReadBinding(binding));
    }
}  // namespace Horo::UiTemplates::Detail::Codec
