#include "Horo/UiTemplates/UiTemplateSource.h"

#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateCodec.h"

namespace Horo::UiTemplates {
    namespace {
        using namespace Detail::Codec;

        /** @brief Encodes the complete closed target vocabulary; local and nested identities remain distinct. */
        Json Target(const UiTemplateParameterTarget &target) {
            return std::visit([&]<typename T>(const T &value) -> Json {
                if constexpr (std::is_same_v<T, UiTemplateNestedParameterTarget>) {
                    Json lineage = Json::array();
                    for (const auto id : value.lineage)
                        lineage.push_back(Hex(id.Bytes()));
                    return {{"kind", 3}, {"lineage", std::move(lineage)}, {"parameter", Hex(value.parameter.Bytes())}};
                } else {
                    Json result{{"kind", target.index()},
                                {"element", Hex(value.element.Bytes())},
                                {"property", Hex(value.property.Bytes())}};
                    if constexpr (std::is_same_v<T, UiTemplateAttachmentTarget>)
                        result["valueKind"] = static_cast<std::uint8_t>(value.kind);
                    return result;
                }
            }, target);
        }

        /** @brief Commits exact parameter type/constraints/defaults and target identities in canonical order. */
        Json Parameters(const UiTemplateDocument &source) {
            Json result = Json::array();
            for (const auto *parameter : Ordered(std::span{source.parameters}, &UiTemplateParameter::id)) {
                std::vector<std::string> ordered;
                for (const auto &target : parameter->targets)
                    ordered.push_back(Target(target).dump());
                std::ranges::sort(ordered);
                Json targets = Json::array();
                for (const auto &bytes : ordered)
                    targets.push_back(ParseBoundedTemplateJson(bytes));
                result.push_back({{"id", Hex(parameter->id.Bytes())},
                                  {"kind", static_cast<std::uint8_t>(parameter->kind)},
                                  {"required", parameter->required},
                                  {"default", parameter->defaultValue ? WriteValue(*parameter->defaultValue) : Json(nullptr)},
                                  {"minimum", Optional(parameter->minimum)},
                                  {"maximum", Optional(parameter->maximum)},
                                  {"maximumTextBytes", parameter->maximumTextBytes},
                                  {"targets", std::move(targets)}});
            }
            return result;
        }

        /** @brief Commits authored local hierarchy and all eight closed property value alternatives. */
        Json Elements(const UiTemplateDocument &source) {
            Json result = Json::array();
            for (const auto *element : Ordered(std::span{source.elements}, &UiTemplateElement::id)) {
                Json properties = Json::array();
                for (const auto *property : Ordered(std::span{element->properties}, &UiTemplateProperty::property))
                    properties.push_back({{"property", Hex(property->property.Bytes())}, {"value", WriteValue(property->value)}});
                result.push_back({{"id", Hex(element->id.Bytes())},
                                  {"parent", Hex(element->parent.Bytes())},
                                  {"type", element->type.Value()},
                                  {"properties", std::move(properties)}});
            }
            return result;
        }

        /** @brief Commits every slot admission policy and ordinary default fragment without remapping authored IDs. */
        Json Slots(const UiTemplateDocument &source) {
            Json result = Json::array();
            for (const auto *slot : Ordered(std::span{source.slots}, &UiTemplateSlot::id)) {
                std::vector<std::string> kinds;
                for (const auto &kind : slot->allowedRootKinds)
                    kinds.push_back(kind.Value());
                std::ranges::sort(kinds);
                result.push_back({{"id", Hex(slot->id.Bytes())},
                                  {"parent", Hex(slot->parent.Bytes())},
                                  {"minimum", slot->minimumChildren},
                                  {"maximum", slot->maximumChildren},
                                  {"kinds", std::move(kinds)},
                                  {"default", WriteTemplateOrdinaryElements(slot->defaultContent)}});
            }
            return result;
        }

        /** @brief Reuses durable linked-record encoding for nested local-parent records; conversion preserves exact identity bytes. */
        Json Nested(const UiTemplateDocument &source) {
            std::vector<UiTemplateInstanceRecord> records;
            records.reserve(source.nested.size());
            for (const auto &instance : source.nested)
                records.push_back({instance.id, Take(Runtime::Ui::UiElementId::Create(instance.parent.Bytes())), instance.accepted,
                                   instance.arguments, instance.slots});
            return WriteRecords(records);
        }
    }  // namespace

    /** @copydoc SerializeTemplateSource */
    Result<std::string> SerializeTemplateSource(const UiTemplateCatalog &catalog, const UiTemplateReference &reference) {
        using Output = Result<std::string>;
        return CodecResult<std::string>([&]() -> Output {
            const auto resolved = catalog.Resolve(reference);
            if (resolved.HasError())
                return Output::Failure(resolved.ErrorValue());
            const auto &source = *resolved.Value();
            std::vector<std::pair<std::string, std::string>> packages;
            for (const auto &package : source.dependency.packages)
                packages.emplace_back(package.package.Value(), package.versions.ToString());
            std::ranges::sort(packages);
            const Json result{{"schema", Json::array({source.dependency.schema.major, source.dependency.schema.minor})},
                              {"asset", Hex(source.dependency.asset.asset.Bytes())},
                              {"interface",
                               Json::array({source.dependency.interfaceVersion.major, source.dependency.interfaceVersion.minor})},
                              {"packages", packages},
                              {"root", Hex(source.root.Bytes())},
                              {"elements", Elements(source)},
                              {"parameters", Parameters(source)},
                              {"slots", Slots(source)},
                              {"nested", Nested(source)}};
            auto bytes = result.dump();
            if (bytes.size() > catalog.Limits().maximumPayloadBytes)
                return Output::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
            return Output::Success(std::move(bytes));
        });
    }
}  // namespace Horo::UiTemplates
