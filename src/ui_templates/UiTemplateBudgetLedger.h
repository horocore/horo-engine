#pragma once

#include "UiTemplateBudget.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>

namespace Horo::UiTemplates::Detail {
    /** @brief Measures one owned parent chain without allocating, rejecting absent parents and cycles. */
    template <typename Element, typename Id>
    std::optional<std::size_t> ElementDepth(const std::span<const Element> elements, const Id id, const std::size_t ceiling) {
        const auto found = std::ranges::find(elements, id, &Element::id);
        if (found == elements.end() || !id.IsValid())
            return std::nullopt;
        const Element *current = std::to_address(found);
        std::size_t depth{1};
        while (current->parent.IsValid()) {
            if (depth >= ceiling)
                return std::nullopt;
            const auto parent = std::ranges::find(elements, current->parent, &Element::id);
            if (parent == elements.end())
                return std::nullopt;
            current = std::to_address(parent);
            ++depth;
        }
        return depth;
    }

    /** @brief Checks closed numeric fallback alternatives independently of cumulative budget counters. */
    template <typename Fallback> bool FiniteBindingFallback(const Fallback &fallback) {
        const auto pair = []<typename Scalar>(const Scalar first, const Scalar second) {
            return std::isfinite(first) && std::isfinite(second);
        };
        if constexpr (std::is_floating_point_v<Fallback>)
            return std::isfinite(fallback);
        else if constexpr (std::is_same_v<Fallback, Runtime::Ui::UiBindingVector2>)
            return pair(fallback.x, fallback.y);
        else if constexpr (std::is_same_v<Fallback, Runtime::Ui::UiBindingVector3>)
            return pair(fallback.x, fallback.y) && std::isfinite(fallback.z);
        else if constexpr (std::is_same_v<Fallback, Runtime::Ui::UiBindingColor>)
            return pair(fallback.r, fallback.g) && pair(fallback.b, fallback.a);
        else
            return true;
    }

    /** @brief Checks numeric declaration shape without accessing or charging cumulative budget state. */
    inline bool FiniteTemplateRange(const UiTemplateParameter &parameter) {
        if (parameter.minimum.has_value() && !std::isfinite(*parameter.minimum))
            return false;
        if (parameter.maximum.has_value() && !std::isfinite(*parameter.maximum))
            return false;
        return !parameter.minimum.has_value() || !parameter.maximum.has_value() || *parameter.minimum <= *parameter.maximum;
    }

    /** @brief Stack-only cumulative counters; conservative wire charges cover escaping before codecs allocate.
     * @details Range checks use the sequential algorithm overload, borrowing this ledger only for the synchronous call.
     * Each charge retains its original field order and false result stops further charging; no callback or state escapes.
     */
    struct TemplateBudgetLedger final {
        const UiTemplateLimits &limits;
        std::size_t elements{};
        std::size_t instances{};
        std::size_t parameters{};
        std::size_t slots{};
        std::size_t properties{};
        std::size_t dependencies{};
        std::size_t bytes{};

        bool Add(std::size_t &counter, const std::size_t count, const std::size_t maximum) const noexcept {
            if (counter > maximum || count > maximum - counter)
                return false;
            counter += count;
            return true;
        }

        bool Text(const std::size_t count) noexcept {
            if (bytes > limits.maximumPayloadBytes || count > (limits.maximumPayloadBytes - bytes) / 6)
                return false;
            return Add(bytes, count * 6, limits.maximumPayloadBytes);
        }

        /** @brief Charges a borrowed style literal without copying an owning outer variant. */
        bool Style(const Runtime::Ui::UiStyleValue &item) {
            if (item.valueless_by_exception() || !Add(dependencies, 1, limits.maximumDependencies))
                return false;
            return std::visit([]<typename Literal>(const Literal &literal) {
                if constexpr (requires { literal.IsValid(); })
                    return literal.IsValid();
                else
                    return true;
            }, item);
        }

        /** @brief Charges one closed fallback alternative after complete binding metadata admission. */
        template <typename Fallback> bool BindingFallback(const Fallback &fallback) {
            if constexpr (std::is_same_v<Fallback, std::string>)
                return Text(fallback.size());
            else if constexpr (std::is_same_v<Fallback, Runtime::Ui::UiBindingLocalizedMessage>)
                return Text(fallback.key.size());
            else if constexpr (std::is_same_v<Fallback, Runtime::Ui::UiBindingReference>)
                return Text(fallback.value.size()) && Add(dependencies, 1, limits.maximumDependencies);
            else
                return FiniteBindingFallback(fallback);
        }

        /** @brief Charges borrowed binding metadata/fallback without allocating a candidate or provider instance. */
        bool Binding(const Runtime::Ui::UiBindingDescriptor &item) {
            if (!Text(item.source.providerType.Value().size()) || !Text(item.source.property.Value().size()) ||
                (item.converter && !Text(item.converter->id.Value().size())))
                return false;
            if (!item.fallback)
                return true;
            if (item.fallback->valueless_by_exception())
                return false;
            return std::visit([this](const auto &fallback) {
                return BindingFallback(fallback);
            }, *item.fallback);
        }

        bool Value(const UiTemplateValue &value) {
            if (value.valueless_by_exception() || !Add(bytes, 512, limits.maximumPayloadBytes))
                return false;
            return std::visit([&]<typename Value>(const Value &item) {
                if constexpr (std::is_same_v<Value, std::string>)
                    return Text(item.size());
                else if constexpr (std::is_same_v<Value, double>)
                    return std::isfinite(item);
                else if constexpr (std::is_same_v<Value, Runtime::Ui::UiReference>)
                    return Reference(item);
                else if constexpr (std::is_same_v<Value, Runtime::Ui::UiStyleValue>)
                    return Style(item);
                else if constexpr (std::is_same_v<Value, Runtime::Ui::LocalizedMessageRef>)
                    return Localized(item);
                else if constexpr (std::is_same_v<Value, Runtime::Ui::UiBindingDescriptor>)
                    return Binding(item);
                else
                    return true;
            }, value);
        }

        /** @brief Charges explicit asset evidence without constructing an owning variant. */
        bool Reference(const Runtime::Ui::UiReference &reference) {
            return Text(reference.expectedAssetType.Value().size()) &&
                   (reference.kind != Runtime::Ui::UiReferenceKind::Asset || Add(dependencies, 1, limits.maximumDependencies));
        }

        /** @brief Admits one typed localized argument's fixed charge and optional owning payload. */
        bool LocalizedArgument(const Runtime::Ui::UiLocalizedArgument &argument) {
            if (argument.value.valueless_by_exception() || !Add(bytes, 128, limits.maximumPayloadBytes) || !Text(argument.name.size()))
                return false;
            if (const auto *text = std::get_if<std::string>(&argument.value); text && !Text(text->size()))
                return false;
            if (const auto *number = std::get_if<double>(&argument.value); number && !std::isfinite(*number))
                return false;
            return true;
        }

        /** @brief Charges source/expanded localization payload directly from its immutable borrowed owner. */
        bool Localized(const Runtime::Ui::LocalizedMessageRef &item) {
            if (!Text(item.Key().namespaceId.size()) || !Text(item.Key().localKey.size()) || !Text(item.FallbackText().size()) ||
                item.Arguments().size() > Runtime::Ui::MaximumUiLocalizedArguments)
                return false;
            return std::ranges::all_of(item.Arguments(), [this](const auto &argument) {
                return LocalizedArgument(argument);
            });
        }

        bool Arguments(const std::span<const UiTemplateArgument> arguments) {
            if (!Add(parameters, arguments.size(), limits.maximumParameters))
                return false;
            return std::ranges::all_of(arguments, [this](const auto &argument) {
                return Value(argument.value);
            });
        }

        /** @brief Charges one ordinary property in wire order without copying its variant. */
        bool FragmentProperty(const Runtime::Ui::UiTypedProperty &property) {
            if (property.value.valueless_by_exception() || !Text(property.key.size()) || !Add(bytes, 128, limits.maximumPayloadBytes))
                return false;
            if (const auto *number = std::get_if<double>(&property.value); number && !std::isfinite(*number))
                return false;
            if (const auto *text = std::get_if<std::string>(&property.value); text && !Text(text->size()))
                return false;
            if (const auto *reference = std::get_if<Runtime::Ui::UiReference>(&property.value); reference && !Reference(*reference))
                return false;
            return true;
        }

        /** @brief Charges the fixed element envelope before touching any property/reference payload. */
        bool FragmentEnvelope(const Runtime::Ui::UiDocumentElement &element) {
            return Text(element.type.Value().size()) && Add(bytes, 128, limits.maximumPayloadBytes) &&
                   Add(properties, element.properties.size(), limits.maximumProperties) &&
                   element.properties.size() <= Runtime::Ui::MaximumUiDocumentProperties &&
                   element.references.size() <= Runtime::Ui::MaximumUiDocumentReferences;
        }

        /** @brief Charges one ordinary element after its parent chain has been admitted. */
        bool FragmentElement(const Runtime::Ui::UiDocumentElement &element) {
            if (!FragmentEnvelope(element))
                return false;
            for (const auto &property : element.properties)
                if (!FragmentProperty(property))
                    return false;
            return std::ranges::all_of(element.references, [this](const auto &reference) {
                if (!Reference(reference))
                    return false;
                return Add(bytes, 128, limits.maximumPayloadBytes);
            });
        }

        bool Fragment(const std::span<const Runtime::Ui::UiDocumentElement> fragment) {
            if (!Add(elements, fragment.size(), limits.maximumElements))
                return false;
            return std::ranges::all_of(fragment, [this, fragment](const auto &element) {
                if (!ElementDepth(fragment, element.id, limits.maximumDepth).has_value())
                    return false;
                return FragmentElement(element);
            });
        }

        /** @brief Charges one supplied slot's ordinary fragment before any nested record admission. */
        bool SlotContent(const UiTemplateSlotContent &slot, const std::size_t depth, const bool recursive) {
            if (!Fragment(slot.elements))
                return false;
            if (!recursive)
                return true;
            if (!Add(instances, slot.nested.size(), limits.maximumInstances))
                return false;
            return std::ranges::all_of(slot.nested, [this, depth](const auto &nested) {
                if (!Arguments(nested.arguments))
                    return false;
                return SlotInput(nested.slots, depth + 1);
            });
        }

        bool SlotInput(const std::span<const UiTemplateSlotContent> content, const std::size_t depth, const bool recursive = true) {
            if (depth > limits.maximumDepth || !Add(slots, content.size(), limits.maximumSlots))
                return false;
            return std::ranges::all_of(content, [this, depth, recursive](const auto &slot) {
                return SlotContent(slot, depth, recursive);
            });
        }

        /** @brief Charges the complete declared locale-variant set without resolving asset fallback. */
        bool LocalizedAsset(const Runtime::Ui::UiLocalizedAssetReference &asset) {
            if (!Text(asset.ExpectedType().Value().size()))
                return false;
            return std::ranges::all_of(asset.Variants(), [this](const auto &variant) {
                if (!Add(bytes, 128, limits.maximumPayloadBytes))
                    return false;
                return Text(variant.locale.Value().size());
            });
        }

        /** @brief Charges the ordinary hierarchy, dependency count and canvas/route envelope in their original order. */
        bool OrdinaryEnvelope(const Runtime::Ui::UiDocument &document) {
            return Fragment(document.Elements()) && Add(dependencies, document.Dependencies().size(), limits.maximumDependencies) &&
                   Add(bytes, (document.Canvases().size() + document.Routes().size()) * 128, limits.maximumPayloadBytes);
        }

        /** @brief Charges one explicit asset requirement's fixed envelope before its registered type text. */
        bool AssetDependency(const Runtime::Ui::UiAssetDependency &dependency) {
            return Add(bytes, 128, limits.maximumPayloadBytes) && Text(dependency.expectedType.Value().size());
        }

        /** @brief Charges all ordinary authority into the same counters used by accepted template expansions. */
        bool Ordinary(const Runtime::Ui::UiDocument &document) {
            if (!OrdinaryEnvelope(document))
                return false;
            for (const auto &dependency : document.Dependencies())
                if (!AssetDependency(dependency))
                    return false;
            for (const auto &text : document.LocalizedTexts())
                if (!Add(bytes, 512, limits.maximumPayloadBytes) || !Localized(text))
                    return false;
            return std::ranges::all_of(document.LocalizedAssets(), [this](const auto &asset) {
                return LocalizedAsset(asset);
            });
        }

        /** @brief Charges a token dependency or typed literal after validating the exact value source. */
        bool StyleAttachment(const UiTemplateExpandedStyle &style) {
            if (!style.assignment.value.IsValid() || !Add(bytes, 512, limits.maximumPayloadBytes))
                return false;
            if (style.assignment.value.referencesToken)
                return Add(dependencies, 1, limits.maximumDependencies);
            return Style(style.assignment.value.literal);
        }

        /** @brief Charges shared property cardinality for style/text/bindings before any attachment payload. */
        bool AttachmentEnvelope(const TemplateDocumentBudgetInput &input) {
            return input.bindings.size() <= Runtime::Ui::MaximumUiBindingDescriptors &&
                   Add(properties, input.styles.size(), limits.maximumProperties) &&
                   Add(properties, input.localizedTexts.size(), limits.maximumProperties) &&
                   Add(properties, input.bindings.size(), limits.maximumProperties);
        }

        /** @brief Charges durable ordinary/detached rich attachments into the same aggregate policy before candidate copies. */
        bool Attachments(const TemplateDocumentBudgetInput &input) {
            if (!AttachmentEnvelope(input))
                return false;
            for (const auto &style : input.styles)
                if (!StyleAttachment(style))
                    return false;
            for (const auto &text : input.localizedTexts)
                if (!Add(bytes, 512, limits.maximumPayloadBytes) || !Localized(text.text))
                    return false;
            return std::ranges::all_of(input.bindings, [this](const auto &binding) {
                if (!Add(bytes, 512, limits.maximumPayloadBytes))
                    return false;
                return Binding(binding);
            });
        }

        /** @brief Admits actual composed hierarchy depth before any parent offset or candidate allocation. */
        template <typename Element> bool ComposedDepth(const std::span<const Element> fragment, const std::size_t parentDepth) const {
            return std::ranges::all_of(fragment, [this, fragment, parentDepth](const auto &element) {
                const auto depth = ElementDepth(fragment, element.id, limits.maximumDepth);
                return depth.has_value() && parentDepth <= limits.maximumDepth - *depth;
            });
        }

        /** @brief Charges each traversed forwarding edge before resolving its exact destination. */
        bool ForwardTargets(const UiTemplateCatalog &catalog, const UiTemplateDocument &document,
                            const UiTemplateNestedParameterTarget &forward, const UiTemplateValue &value, const std::size_t depth) {
            const UiTemplateDocument *current = &document;
            std::size_t nextDepth = depth;
            for (const auto id : forward.lineage) {
                ++nextDepth;
                if (nextDepth >= limits.maximumDepth || !Value(value))
                    return false;
                const auto child = std::ranges::find(current->nested, id, &UiTemplateNestedInstance::id);
                if (child == current->nested.end())
                    return false;
                const auto selected = catalog.Resolve(child->accepted);
                if (selected.HasError())
                    return false;
                current = selected.Value();
            }
            const auto destination = std::ranges::find(current->parameters, forward.parameter, &UiTemplateParameter::id);
            return destination != current->parameters.end() && ParameterTargets(catalog, *current, *destination, value, nextDepth);
        }

        /** @brief Charges forwarded payload copies and descendant target fanout before expansion allocations. */
        bool ParameterTargets(const UiTemplateCatalog &catalog, const UiTemplateDocument &document, const UiTemplateParameter &parameter,
                              const UiTemplateValue &value, const std::size_t depth) {
            if (depth >= limits.maximumDepth)
                return false;
            return std::ranges::all_of(parameter.targets, [this, &catalog, &document, &value, depth](const auto &target) {
                if (!Value(value))
                    return false;
                const auto *forward = std::get_if<UiTemplateNestedParameterTarget>(&target);
                if (!forward)
                    return true;
                return ForwardTargets(catalog, document, *forward, value, depth);
            });
        }

        /** @brief Charges each supplied value's complete target fanout, independently of encoded source storage. */
        bool TargetValues(const UiTemplateCatalog &catalog, const UiTemplateDocument &document,
                          const std::span<const UiTemplateArgument> arguments) {
            for (const auto &argument : arguments) {
                const auto parameter = std::ranges::find(document.parameters, argument.parameter, &UiTemplateParameter::id);
                if (parameter == document.parameters.end())
                    continue;  // Exact parameter admission reports the unknown identity; it has no target fanout to charge.
                if (!ParameterTargets(catalog, document, *parameter, argument.value, 0))
                    return false;
            }
            return true;
        }

        /** @brief Charges one declared target, including bounded forwarding lineage. */
        bool Target(const UiTemplateParameterTarget &target) {
            if (target.valueless_by_exception())
                return false;
            if (const auto *nested = std::get_if<UiTemplateNestedParameterTarget>(&target)) {
                if (nested->lineage.empty() || nested->lineage.size() > limits.maximumDepth)
                    return false;
                return Add(bytes, nested->lineage.size() * 16 + 64, limits.maximumPayloadBytes);
            }
            return Add(bytes, 64, limits.maximumPayloadBytes);
        }

        /** @brief Charges each default target copy, plus its retained source value, before validating target payloads. */
        bool DefaultValueCopies(const UiTemplateParameter &parameter) {
            if (!parameter.defaultValue)
                return true;
            for (std::size_t index = 0; index <= parameter.targets.size(); ++index)
                if (!Value(*parameter.defaultValue))
                    return false;
            return true;
        }

        /** @brief Charges default-value fanout before expansion allocates mutable element copies. */
        bool Parameter(const UiTemplateParameter &parameter) {
            if (parameter.targets.empty() || parameter.targets.size() > limits.maximumProperties || !FiniteTemplateRange(parameter) ||
                !Add(properties, parameter.targets.size(), limits.maximumProperties))
                return false;
            if (!DefaultValueCopies(parameter))
                return false;
            return std::ranges::all_of(parameter.targets, [this](const auto &target) {
                return Target(target);
            });
        }

        /** @brief Charges a source element's hierarchy and typed property payload in declaration order. */
        bool DocumentElement(const UiTemplateElement &element, const std::span<const UiTemplateElement> source) {
            if (!ElementDepth(source, element.id, limits.maximumDepth).has_value())
                return false;
            if (!Text(element.type.Value().size()) || !Add(bytes, 128, limits.maximumPayloadBytes) ||
                !Add(properties, element.properties.size(), limits.maximumProperties))
                return false;
            return std::ranges::all_of(element.properties, [this](const auto &property) {
                return Value(property.value);
            });
        }

        /** @brief Charges document counts in element/parameter/slot/package/frame order before walking declarations. */
        bool DocumentEnvelope(const UiTemplateDocument &document) {
            return Add(elements, document.elements.size(), limits.maximumElements) &&
                   Add(parameters, document.parameters.size(), limits.maximumParameters) &&
                   Add(slots, document.slots.size(), limits.maximumSlots) &&
                   document.dependency.packages.size() <= limits.maximumInstances && Add(bytes, 256, limits.maximumPayloadBytes);
        }

        /** @brief Charges a slot's full default fragment and every allowed root kind without dropping unused defaults. */
        bool SlotDefault(const UiTemplateSlot &slot) {
            if (!Fragment(slot.defaultContent) || slot.allowedRootKinds.size() > limits.maximumElements)
                return false;
            return std::ranges::all_of(slot.allowedRootKinds, [this](const auto &kind) {
                return Text(kind.Value().size());
            });
        }

        bool Document(const UiTemplateDocument &document) {
            if (!DocumentEnvelope(document))
                return false;
            for (const auto &element : document.elements)
                if (!DocumentElement(element, document.elements))
                    return false;
            for (const auto &parameter : document.parameters)
                if (!Parameter(parameter))
                    return false;
            return std::ranges::all_of(document.slots, [this](const auto &slot) {
                return SlotDefault(slot);
            });
        }
    };
}  // namespace Horo::UiTemplates::Detail
