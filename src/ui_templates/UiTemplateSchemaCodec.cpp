#include "UiTemplateCodec.h"

namespace Horo::UiTemplates::Detail::Codec {
    namespace {
        /** @brief Includes complete style default, effects and numeric/inheritance constraints in schema evidence. */
        Json Style(const Runtime::Ui::UiStylePropertyDescriptor &value) {
            return Json::array({Hex(value.id.Bytes()), static_cast<std::uint8_t>(value.category), WriteStyle(value.defaultValue),
                                value.effects.measure, value.effects.paint, value.effects.hitTest, value.effects.accessibility,
                                value.inheritsToChildren, value.allowsSignedDimension, value.minimumScalar, value.maximumScalar});
        }

        /** @brief Includes the immutable provider's complete public property signatures and capability envelope. */
        Json Provider(const Runtime::Ui::UiBindingProviderSchema &value) {
            Json properties = Json::array();
            for (const auto &property : value.Properties()) {
                const auto &limits = property.limits;
                properties.push_back(Json::array(
                    {property.id.Value(), static_cast<std::uint8_t>(property.type), static_cast<std::uint8_t>(property.access),
                     static_cast<std::uint8_t>(property.update), static_cast<std::uint8_t>(property.privacy),
                     static_cast<std::uint16_t>(property.flags), property.signatureFingerprint, limits.maximumBytes, limits.maximumElements,
                     Optional(limits.minimumSigned), Optional(limits.maximumSigned), Optional(limits.minimumUnsigned),
                     Optional(limits.maximumUnsigned), Optional(limits.minimumScalar), Optional(limits.maximumScalar)}));
            }
            const auto version = value.Version();
            return Json::array({value.Type().Value(), value.OwnerModule().value, version.major, version.minor, version.fingerprint,
                                value.AllowedScopes(), static_cast<std::uint16_t>(value.Flags()), std::move(properties)});
        }
    }  // namespace

    /** @copydoc WriteSchemas */
    Json WriteSchemas(const UiTemplateSchemaSnapshot &schemas) {
        Json elements = Json::array(), properties = Json::array(), providers = Json::array();
        for (const auto *element : Ordered(std::span{schemas.elementTypes}, [](const auto &value) {
            return value;
        }))
            elements.push_back(element->Value());
        for (const auto *property : Ordered(std::span{schemas.properties}, [](const auto &value) {
            return std::pair{value.elementType, value.property};
        }))
            properties.push_back(
                Json::array({property->elementType.Value(), Hex(property->property.Bytes()), static_cast<std::uint8_t>(property->kind),
                             property->documentKey, property->style ? Style(*property->style) : Json(nullptr)}));
        for (const auto *provider : Ordered(std::span{schemas.bindingProviders}, &Runtime::Ui::UiBindingProviderSchema::Type))
            providers.push_back(Provider(*provider));
        return {{"elements", std::move(elements)}, {"properties", std::move(properties)}, {"providers", std::move(providers)}};
    }
}  // namespace Horo::UiTemplates::Detail::Codec
