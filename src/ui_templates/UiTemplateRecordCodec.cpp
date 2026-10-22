#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateCodec.h"

namespace Horo::UiTemplates::Detail::Codec {
    namespace {
        using namespace Runtime::Ui;

        /** @brief Cumulative decoding admission, checked before reserve or construction of authored containers. */
        struct DecodeBudget final {
            const UiTemplateLimits &limits;
            std::size_t instances{};
            std::size_t parameters{};
            std::size_t slots{};
            std::size_t elements{};
            std::size_t properties{};

            void Add(std::size_t &count, const std::size_t amount, const std::size_t maximum) const {
                if (count > maximum || amount > maximum - count)
                    throw Failure{MakeError(UiErrors::TemplateGraphBudgetExceeded)};
                count += amount;
            }
        };

        /** @brief Writes ordinary element data through the same closed first-five value categories. */
        Json WriteElement(const UiDocumentElement &element) {
            Json properties = Json::array();
            Json references = Json::array();
            for (const auto *property : Ordered(std::span{element.properties}, &UiTypedProperty::key))
                properties.push_back({{"key", property->key}, {"value", std::visit([](const auto &item) {
                    return WriteValue(UiTemplateValue{item});
                }, property->value)}});
            for (const auto *reference : Ordered(std::span{element.references}, [](const auto &value) {
                return value;
            }))
                references.push_back(WriteReference(*reference));
            Json encoded{{"id", Hex(element.id.Bytes())},
                         {"parent", Hex(element.parent.Bytes())},
                         {"type", element.type.Value()},
                         {"properties", std::move(properties)},
                         {"references", std::move(references)}};
            if (element.presentation)
                encoded["presentation"] = Json::parse(Take(SerializeUiElementPresentation(*element.presentation)));
            return encoded;
        }

        /** @brief Restores one ordinary property without accepting rich alternatives in an ordinary codec field. */
        UiPropertyValue ReadOrdinaryValue(const Json &source) {
            Fields(source, {"kind", "value"});
            if (Integer<std::uint8_t>(source.at("kind")) > 4)
                Reject();
            return std::visit([]<typename T>(T &&item) -> UiPropertyValue {
                using Value = std::remove_cvref_t<T>;
                if constexpr (std::is_same_v<Value, bool> || std::is_same_v<Value, std::int64_t> || std::is_same_v<Value, double> ||
                              std::is_same_v<Value, std::string> || std::is_same_v<Value, UiReference>)
                    return std::forward<T>(item);
                else
                    Reject();
            }, ReadValue(source));
        }

        /** @brief Restores bounded ordinary slot data; full connected hierarchy/reference admission occurs before expansion. */
        UiDocumentElement ReadElement(const Json &value, DecodeBudget &budget) {
            if (value.contains("presentation"))
                Fields(value, {"id", "parent", "type", "properties", "references", "presentation"});
            else
                Fields(value, {"id", "parent", "type", "properties", "references"});
            const auto &properties = Array(value.at("properties"), MaximumUiDocumentProperties);
            const auto &references = Array(value.at("references"), MaximumUiDocumentReferences);
            budget.Add(budget.properties, properties.size(), budget.limits.maximumProperties);
            UiDocumentElement output{Identity<UiElementId>(value.at("id")),
                                     Identity<UiElementId>(value.at("parent"), true),
                                     Take(Assets::AssetTypeId::Parse(Text(value.at("type")))),
                                     {},
                                     {}};
            output.properties.reserve(properties.size());
            output.references.reserve(references.size());
            for (const auto &property : properties) {
                Fields(property, {"key", "value"});
                output.properties.emplace_back(Text(property.at("key")), ReadOrdinaryValue(property.at("value")));
            }
            for (const auto &reference : references)
                output.references.push_back(ReadReference(reference));
            if (value.contains("presentation"))
                output.presentation = Take(DeserializeUiElementPresentation(value.at("presentation").dump()));
            return output;
        }

        Json WriteSlots(std::span<const UiTemplateSlotContent> slots);
        std::vector<UiTemplateSlotContent> ReadSlots(const Json &value, DecodeBudget &budget, std::size_t depth);

        /** @brief Writes a linked record with stable keys and owned customizations, without expanded template content. */
        template <typename Record> Json WriteRecord(const Record &record) {
            Json arguments = Json::array();
            for (const auto *argument : Ordered(std::span{record.arguments}, &UiTemplateArgument::parameter))
                arguments.push_back({{"parameter", Hex(argument->parameter.Bytes())}, {"value", WriteValue(argument->value)}});
            return {{"id", Hex(record.id.Bytes())},
                    {"parent", Hex(record.parent.Bytes())},
                    {"accepted", WriteAccepted(record.accepted)},
                    {"arguments", std::move(arguments)},
                    {"slots", WriteSlots(record.slots)}};
        }

        /** @brief Decodes complete linked customizations under cumulative count/depth bounds before allocating their candidates. */
        template <typename Record> Record ReadRecord(const Json &value, DecodeBudget &budget, const std::size_t depth) {
            if (depth >= budget.limits.maximumDepth)
                throw Failure{MakeError(UiErrors::TemplateGraphBudgetExceeded)};
            Fields(value, {"id", "parent", "accepted", "arguments", "slots"});
            budget.Add(budget.instances, 1, budget.limits.maximumInstances);
            const auto &arguments = Array(value.at("arguments"), budget.limits.maximumParameters);
            budget.Add(budget.parameters, arguments.size(), budget.limits.maximumParameters);
            Record output;
            output.id = Identity<UiTemplateInstanceId>(value.at("id"));
            output.parent = Identity<UiElementId>(value.at("parent"));
            output.accepted = ReadAccepted(value.at("accepted"));
            output.arguments.reserve(arguments.size());
            for (const auto &argument : arguments) {
                Fields(argument, {"parameter", "value"});
                output.arguments.push_back({Identity<UiTemplateParameterId>(argument.at("parameter")), ReadValue(argument.at("value"))});
            }
            output.slots = ReadSlots(value.at("slots"), budget, depth);
            return output;
        }

        /** @brief Canonicalizes ordinary slot fragments and nested linked records by stable identities. */
        Json WriteSlots(const std::span<const UiTemplateSlotContent> slots) {
            Json output = Json::array();
            for (const auto *slot : Ordered(slots, &UiTemplateSlotContent::slot)) {
                Json elements = Json::array();
                Json nested = Json::array();
                for (const auto *element : Ordered(std::span{slot->elements}, &UiDocumentElement::id))
                    elements.push_back(WriteElement(*element));
                for (const auto *instance : Ordered(std::span{slot->nested}, &UiTemplateSlotNestedInstance::id))
                    nested.push_back(WriteRecord(*instance));
                output.push_back({{"slot", Hex(slot->slot.Bytes())}, {"elements", std::move(elements)}, {"nested", std::move(nested)}});
            }
            return output;
        }

        /** @brief Restores owned slot authority with finite aggregate capacity before each vector allocation. */
        std::vector<UiTemplateSlotContent> ReadSlots(const Json &value, DecodeBudget &budget, const std::size_t depth) {
            const auto &slots = Array(value, budget.limits.maximumSlots);
            budget.Add(budget.slots, slots.size(), budget.limits.maximumSlots);
            std::vector<UiTemplateSlotContent> output;
            output.reserve(slots.size());
            for (const auto &slot : slots) {
                Fields(slot, {"slot", "elements", "nested"});
                const auto &elements = Array(slot.at("elements"), budget.limits.maximumElements);
                const auto &nested = Array(slot.at("nested"), budget.limits.maximumInstances);
                budget.Add(budget.elements, elements.size(), budget.limits.maximumElements);
                UiTemplateSlotContent content{Identity<UiTemplateSlotId>(slot.at("slot")), {}, {}};
                content.elements.reserve(elements.size());
                content.nested.reserve(nested.size());
                for (const auto &element : elements)
                    content.elements.push_back(ReadElement(element, budget));
                for (const auto &instance : nested)
                    content.nested.push_back(ReadRecord<UiTemplateSlotNestedInstance>(instance, budget, depth + 1));
                output.push_back(std::move(content));
            }
            return output;
        }
    }  // namespace

    /** @copydoc WriteTemplateOrdinaryElements */
    Json WriteTemplateOrdinaryElements(const std::span<const Runtime::Ui::UiDocumentElement> elements) {
        Json result = Json::array();
        for (const auto *element : Ordered(elements, &Runtime::Ui::UiDocumentElement::id))
            result.push_back(WriteElement(*element));
        return result;
    }

    /** @copydoc WriteAccepted */
    Json WriteAccepted(const UiTemplateReference &value) {
        return {{"asset", Hex(value.asset.asset.Bytes())},
                {"revision", Hex(value.revision.bytes)},
                {"major", value.minimumInterface.major},
                {"minor", value.minimumInterface.minor}};
    }

    /** @copydoc ReadAccepted */
    UiTemplateReference ReadAccepted(const Json &value) {
        Fields(value, {"asset", "revision", "major", "minor"});
        UiTemplateReference output{{Asset(value.at("asset"))},
                                   {Bytes<32>(value.at("revision"))},
                                   {Integer<std::uint16_t>(value.at("major")), Integer<std::uint16_t>(value.at("minor"))}};
        if (!output.asset.IsValid() || output.revision == Sha256Digest{} || output.minimumInterface.major == 0)
            Reject();
        return output;
    }

    /** @copydoc WriteRecords */
    Json WriteRecords(const std::span<const UiTemplateInstanceRecord> records) {
        Json output = Json::array();
        for (const auto *record : Ordered(records, &UiTemplateInstanceRecord::id))
            output.push_back(WriteRecord(*record));
        return output;
    }

    /** @copydoc ReadRecords */
    std::vector<UiTemplateInstanceRecord> ReadRecords(const Json &value, const UiTemplateLimits &limits) {
        const auto &records = Array(value, limits.maximumInstances);
        DecodeBudget budget{limits};
        std::vector<UiTemplateInstanceRecord> output;
        output.reserve(records.size());
        for (const auto &record : records)
            output.push_back(ReadRecord<UiTemplateInstanceRecord>(record, budget, 0));
        return output;
    }
}  // namespace Horo::UiTemplates::Detail::Codec
