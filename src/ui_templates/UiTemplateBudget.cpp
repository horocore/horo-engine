#include "UiTemplateBudget.h"

#include "Horo/UiTemplates/UiTemplateErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>

namespace Horo::UiTemplates::Detail {
    namespace {
        /** @brief Measures one owned parent chain without allocating, rejecting absent parents and cycles. */
        template <typename Element, typename Id>
        std::optional<std::size_t> ElementDepth(const std::span<const Element> elements, const Id id, const std::size_t ceiling) {
            const auto found = std::ranges::find(elements, id, &Element::id);
            if (found == elements.end() || !id.IsValid())
                return std::nullopt;
            const Element *current = &*found;
            std::size_t depth{1};
            while (current->parent.IsValid()) {
                if (depth >= ceiling)
                    return std::nullopt;
                const auto parent = std::ranges::find(elements, current->parent, &Element::id);
                if (parent == elements.end())
                    return std::nullopt;
                current = &*parent;
                ++depth;
            }
            return depth;
        }

        /** @brief Stack-only cumulative counters; conservative wire charges cover escaping before codecs allocate. */
        struct Budget final {
            const UiTemplateLimits &limits;
            std::size_t elements{};
            std::size_t instances{};
            std::size_t parameters{};
            std::size_t slots{};
            std::size_t properties{};
            std::size_t dependencies{};
            std::size_t bytes{};

            bool Add(std::size_t &counter, const std::size_t count, const std::size_t maximum) noexcept {
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

            /** @brief Charges borrowed binding metadata/fallback without allocating a candidate or provider instance. */
            bool Binding(const Runtime::Ui::UiBindingDescriptor &item) {
                if (!Text(item.source.providerType.Value().size()) || !Text(item.source.property.Value().size()) ||
                    (item.converter && !Text(item.converter->id.Value().size())))
                    return false;
                if (!item.fallback)
                    return true;
                if (item.fallback->valueless_by_exception())
                    return false;
                return std::visit([&]<typename Fallback>(const Fallback &fallback) {
                    if constexpr (std::is_same_v<Fallback, std::string>)
                        return Text(fallback.size());
                    else if constexpr (std::is_same_v<Fallback, Runtime::Ui::UiBindingLocalizedMessage>)
                        return Text(fallback.key.size());
                    else if constexpr (std::is_same_v<Fallback, Runtime::Ui::UiBindingReference>)
                        return Text(fallback.value.size()) && Add(dependencies, 1, limits.maximumDependencies);
                    else if constexpr (std::is_floating_point_v<Fallback>)
                        return std::isfinite(fallback);
                    else if constexpr (std::is_same_v<Fallback, Runtime::Ui::UiBindingVector2>)
                        return std::isfinite(fallback.x) && std::isfinite(fallback.y);
                    else if constexpr (std::is_same_v<Fallback, Runtime::Ui::UiBindingVector3>)
                        return std::isfinite(fallback.x) && std::isfinite(fallback.y) && std::isfinite(fallback.z);
                    else if constexpr (std::is_same_v<Fallback, Runtime::Ui::UiBindingColor>)
                        return std::isfinite(fallback.r) && std::isfinite(fallback.g) && std::isfinite(fallback.b) &&
                               std::isfinite(fallback.a);
                    else
                        return true;
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

            /** @brief Charges source/expanded localization payload directly from its immutable borrowed owner. */
            bool Localized(const Runtime::Ui::LocalizedMessageRef &item) {
                if (!Text(item.Key().namespaceId.size()) || !Text(item.Key().localKey.size()) || !Text(item.FallbackText().size()) ||
                    item.Arguments().size() > Runtime::Ui::MaximumUiLocalizedArguments)
                    return false;
                for (const auto &argument : item.Arguments()) {
                    if (argument.value.valueless_by_exception() || !Add(bytes, 128, limits.maximumPayloadBytes) ||
                        !Text(argument.name.size()))
                        return false;
                    if (const auto *text = std::get_if<std::string>(&argument.value); text && !Text(text->size()))
                        return false;
                    if (const auto *number = std::get_if<double>(&argument.value); number && !std::isfinite(*number))
                        return false;
                }
                return true;
            }

            bool Arguments(const std::span<const UiTemplateArgument> arguments) {
                if (!Add(parameters, arguments.size(), limits.maximumParameters))
                    return false;
                for (const auto &argument : arguments)
                    if (!Value(argument.value))
                        return false;
                return true;
            }

            bool Fragment(const std::span<const Runtime::Ui::UiDocumentElement> fragment) {
                if (!Add(elements, fragment.size(), limits.maximumElements))
                    return false;
                for (const auto &element : fragment) {
                    if (!ElementDepth(fragment, element.id, limits.maximumDepth))
                        return false;
                    if (!Text(element.type.Value().size()) || !Add(bytes, 128, limits.maximumPayloadBytes) ||
                        !Add(properties, element.properties.size(), limits.maximumProperties) ||
                        element.properties.size() > Runtime::Ui::MaximumUiDocumentProperties ||
                        element.references.size() > Runtime::Ui::MaximumUiDocumentReferences)
                        return false;
                    for (const auto &property : element.properties) {
                        if (property.value.valueless_by_exception() || !Text(property.key.size()) ||
                            !Add(bytes, 128, limits.maximumPayloadBytes))
                            return false;
                        if (const auto *number = std::get_if<double>(&property.value); number && !std::isfinite(*number))
                            return false;
                        if (const auto *text = std::get_if<std::string>(&property.value); text && !Text(text->size()))
                            return false;
                        if (const auto *reference = std::get_if<Runtime::Ui::UiReference>(&property.value);
                            reference && !Reference(*reference))
                            return false;
                    }
                    for (const auto &reference : element.references)
                        if (!Reference(reference) || !Add(bytes, 128, limits.maximumPayloadBytes))
                            return false;
                }
                return true;
            }

            bool SlotInput(const std::span<const UiTemplateSlotContent> content, const std::size_t depth, const bool recursive = true) {
                if (depth > limits.maximumDepth || !Add(slots, content.size(), limits.maximumSlots))
                    return false;
                for (const auto &slot : content) {
                    if (!Fragment(slot.elements) || (recursive && !Add(instances, slot.nested.size(), limits.maximumInstances)))
                        return false;
                    if (!recursive)
                        continue;
                    for (const auto &nested : slot.nested)
                        if (!Arguments(nested.arguments) || !SlotInput(nested.slots, depth + 1))
                            return false;
                }
                return true;
            }

            /** @brief Charges all ordinary authority into the same counters used by accepted template expansions. */
            bool Ordinary(const Runtime::Ui::UiDocument &document) {
                if (!Fragment(document.Elements()) || !Add(dependencies, document.Dependencies().size(), limits.maximumDependencies) ||
                    !Add(bytes, (document.Canvases().size() + document.Routes().size()) * 128, limits.maximumPayloadBytes))
                    return false;
                for (const auto &dependency : document.Dependencies())
                    if (!Add(bytes, 128, limits.maximumPayloadBytes) || !Text(dependency.expectedType.Value().size()))
                        return false;
                for (const auto &text : document.LocalizedTexts())
                    if (!Add(bytes, 512, limits.maximumPayloadBytes) || !Localized(text))
                        return false;
                for (const auto &asset : document.LocalizedAssets()) {
                    if (!Text(asset.ExpectedType().Value().size()))
                        return false;
                    for (const auto &variant : asset.Variants())
                        if (!Add(bytes, 128, limits.maximumPayloadBytes) || !Text(variant.locale.Value().size()))
                            return false;
                }
                return true;
            }

            /** @brief Charges durable ordinary/detached rich attachments into the same aggregate policy before candidate copies. */
            bool Attachments(const TemplateDocumentBudgetInput &input) {
                if (input.bindings.size() > Runtime::Ui::MaximumUiBindingDescriptors ||
                    !Add(properties, input.styles.size(), limits.maximumProperties) ||
                    !Add(properties, input.localizedTexts.size(), limits.maximumProperties) ||
                    !Add(properties, input.bindings.size(), limits.maximumProperties))
                    return false;
                for (const auto &style : input.styles) {
                    if (!style.assignment.value.IsValid() || !Add(bytes, 512, limits.maximumPayloadBytes))
                        return false;
                    if (style.assignment.value.referencesToken) {
                        if (!Add(dependencies, 1, limits.maximumDependencies))
                            return false;
                    } else if (!Style(style.assignment.value.literal))
                        return false;
                }
                for (const auto &text : input.localizedTexts)
                    if (!Add(bytes, 512, limits.maximumPayloadBytes) || !Localized(text.text))
                        return false;
                for (const auto &binding : input.bindings)
                    if (!Add(bytes, 512, limits.maximumPayloadBytes) || !Binding(binding))
                        return false;
                return true;
            }

            /** @brief Admits actual composed hierarchy depth before any parent offset or candidate allocation. */
            template <typename Element> bool ComposedDepth(const std::span<const Element> fragment, const std::size_t parentDepth) const {
                for (const auto &element : fragment) {
                    const auto depth = ElementDepth(fragment, element.id, limits.maximumDepth);
                    if (!depth || parentDepth > limits.maximumDepth - *depth)
                        return false;
                }
                return true;
            }

            /** @brief Charges forwarded payload copies and descendant target fanout before expansion allocations. */
            bool ParameterTargets(const UiTemplateCatalog &catalog, const UiTemplateDocument &document,
                                  const UiTemplateParameter &parameter, const UiTemplateValue &value, const std::size_t depth) {
                if (depth >= limits.maximumDepth)
                    return false;
                for (const auto &target : parameter.targets) {
                    if (!Value(value))
                        return false;
                    const auto *forward = std::get_if<UiTemplateNestedParameterTarget>(&target);
                    if (!forward)
                        continue;
                    const UiTemplateDocument *current = &document;
                    std::size_t nextDepth = depth;
                    for (const auto id : forward->lineage) {
                        if (++nextDepth >= limits.maximumDepth || !Value(value))
                            return false;
                        const auto child = std::ranges::find(current->nested, id, &UiTemplateNestedInstance::id);
                        if (child == current->nested.end())
                            return false;
                        const auto selected = catalog.Resolve(child->accepted);
                        if (selected.HasError())
                            return false;
                        current = selected.Value();
                    }
                    const auto destination = std::ranges::find(current->parameters, forward->parameter, &UiTemplateParameter::id);
                    if (destination == current->parameters.end() || !ParameterTargets(catalog, *current, *destination, value, nextDepth))
                        return false;
                }
                return true;
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

            bool Document(const UiTemplateDocument &document) {
                if (!Add(elements, document.elements.size(), limits.maximumElements) ||
                    !Add(parameters, document.parameters.size(), limits.maximumParameters) ||
                    !Add(slots, document.slots.size(), limits.maximumSlots) ||
                    document.dependency.packages.size() > limits.maximumInstances || !Add(bytes, 256, limits.maximumPayloadBytes))
                    return false;
                for (const auto &element : document.elements) {
                    if (!ElementDepth(std::span{document.elements}, element.id, limits.maximumDepth))
                        return false;
                    if (!Text(element.type.Value().size()) || !Add(bytes, 128, limits.maximumPayloadBytes) ||
                        !Add(properties, element.properties.size(), limits.maximumProperties))
                        return false;
                    for (const auto &property : element.properties)
                        if (!Value(property.value))
                            return false;
                }
                for (const auto &parameter : document.parameters) {
                    if (parameter.targets.empty() || parameter.targets.size() > limits.maximumProperties ||
                        (parameter.minimum && !std::isfinite(*parameter.minimum)) ||
                        (parameter.maximum && !std::isfinite(*parameter.maximum)) ||
                        (parameter.minimum && parameter.maximum && *parameter.minimum > *parameter.maximum) ||
                        !Add(properties, parameter.targets.size(), limits.maximumProperties))
                        return false;
                    // Target writes can duplicate an owned value; charge them before a mutable element copy exists.
                    if (parameter.defaultValue)
                        for (std::size_t index = 0; index <= parameter.targets.size(); ++index)
                            if (!Value(*parameter.defaultValue))
                                return false;
                    for (const auto &target : parameter.targets)
                        if (target.valueless_by_exception())
                            return false;
                        else if (const auto *nested = std::get_if<UiTemplateNestedParameterTarget>(&target)) {
                            if (nested->lineage.empty() || nested->lineage.size() > limits.maximumDepth ||
                                !Add(bytes, nested->lineage.size() * 16 + 64, limits.maximumPayloadBytes))
                                return false;
                        } else if (!Add(bytes, 64, limits.maximumPayloadBytes))
                            return false;
                }
                for (const auto &slot : document.slots) {
                    if (!Fragment(slot.defaultContent) || slot.allowedRootKinds.size() > limits.maximumElements)
                        return false;
                    for (const auto &kind : slot.allowedRootKinds)
                        if (!Text(kind.Value().size()))
                            return false;
                }
                return true;
            }
        };

        /** @brief Walks actual nested references/defaults/slot instances with a fixed-size active asset stack. */
        struct ExpansionBudget final {
            const UiTemplateCatalog &catalog;
            const CancellationToken &cancellation;
            Budget &budget;
            std::array<UiTemplateAssetId, 32> ancestry;

            /** @brief Walks typed nested records at their actual local attachment depth using the same active ancestry. */
            template <typename InstanceRecord, typename Element>
            Result<void> Nested(const std::span<const InstanceRecord> instances, const std::span<const Element> elements,
                                const std::size_t depth, const std::size_t parentDepth) {
                for (const auto &nested : instances) {
                    const auto localDepth = ElementDepth(elements, nested.parent, budget.limits.maximumDepth);
                    if (!localDepth || parentDepth > budget.limits.maximumDepth - *localDepth)
                        return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
                    if (const auto valid = Instance(nested.accepted, nested.arguments, nested.slots, depth + 1, parentDepth + *localDepth);
                        valid.HasError())
                        return valid;
                }
                return Result<void>::Success();
            }

            /** @brief Selects authored or default slot content, then admits its fully composed nested hierarchy. */
            Result<void> Slots(const UiTemplateDocument &document, const std::span<const UiTemplateSlotContent> inputs,
                               const std::size_t depth, const std::size_t parentDepth) {
                for (const auto &slot : document.slots) {
                    const auto localDepth = ElementDepth(std::span{document.elements}, slot.parent, budget.limits.maximumDepth);
                    if (!localDepth || parentDepth > budget.limits.maximumDepth - *localDepth)
                        return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
                    const auto input = std::ranges::find(inputs, slot.id, &UiTemplateSlotContent::slot);
                    const auto fragment = input == inputs.end() ? std::span{slot.defaultContent} : std::span{input->elements};
                    const std::size_t slotDepth = parentDepth + *localDepth;
                    if (!budget.ComposedDepth(fragment, slotDepth))
                        return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
                    if (input != inputs.end())
                        if (const auto valid = Nested(std::span{input->nested}, fragment, depth, slotDepth); valid.HasError())
                            return valid;
                }
                return Result<void>::Success();
            }

            Result<void> Instance(const UiTemplateReference &reference, const std::span<const UiTemplateArgument> arguments,
                                  const std::span<const UiTemplateSlotContent> slots, const std::size_t depth,
                                  const std::size_t parentDepth) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphShutdown, "Template preparation was cancelled."));
                if (depth >= budget.limits.maximumDepth || !budget.Add(budget.instances, 1, budget.limits.maximumInstances) ||
                    !budget.Add(budget.dependencies, 1, budget.limits.maximumDependencies))
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
                const auto active = std::span{ancestry}.first(depth);
                if (std::ranges::find(active, reference.asset) != active.end())
                    return Result<void>::Failure(MakeError(UiErrors::TemplateDependencyCycle));
                ancestry[depth] = reference.asset;
                const auto resolved = catalog.Resolve(reference);
                if (resolved.HasError())
                    return Result<void>::Failure(resolved.ErrorValue());
                const auto &document = *resolved.Value();
                for (const auto &slot : slots)
                    if (std::ranges::find(document.slots, slot.slot, &UiTemplateSlot::id) == document.slots.end())
                        return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid, "Unknown template slot."));
                if (!budget.Document(document) || !budget.Arguments(arguments) || !budget.SlotInput(slots, depth + 1, false) ||
                    !budget.ComposedDepth(std::span{document.elements}, parentDepth) || !budget.TargetValues(catalog, document, arguments))
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
                if (const auto nested = Nested(std::span{document.nested}, std::span{document.elements}, depth, parentDepth);
                    nested.HasError())
                    return nested;
                return Slots(document, slots, depth, parentDepth);
            }
        };
    }  // namespace

    /** @copydoc ValidateTemplateSourceBudget */
    Result<void> ValidateTemplateSourceBudget(const std::span<const UiTemplateDocument> documents, const UiTemplateLimits &limits) {
        Budget budget{limits};
        for (const auto &document : documents) {
            if (!budget.Document(document) || !budget.Add(budget.instances, document.nested.size(), limits.maximumInstances))
                return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
            for (const auto &nested : document.nested)
                if (!budget.Arguments(nested.arguments) || !budget.SlotInput(nested.slots, 1))
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidateTemplateExpansionBudget */
    Result<void> ValidateTemplateExpansionBudget(const UiTemplateCatalog &catalog, const UiTemplateInstanceRecord &instance,
                                                 const CancellationToken &cancellation) {
        Budget counters{catalog.Limits()};
        ExpansionBudget budget{catalog, cancellation, counters, {}};
        return budget.Instance(instance.accepted, instance.arguments, instance.slots, 0, 0);
    }

    /** @copydoc ValidateTemplateDocumentBudget */
    Result<void> ValidateTemplateDocumentBudget(const TemplateDocumentBudgetInput &input, const CancellationToken &cancellation) {
        const auto fail = [] {
            return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        };
        if (!input.limits.IsValid() || input.instances.size() > input.limits.maximumInstances || input.leases.size() > 256 ||
            (!input.fallbackCatalog && input.leases.empty() && input.catalogs.size() != input.instances.size()))
            return fail();
        Budget counters{input.limits};
        if (!counters.Ordinary(input.ordinary) || !counters.Attachments(input))
            return fail();
        if (input.insertion) {
            if (!input.insertionCatalog || input.instances.size() >= input.limits.maximumInstances)
                return fail();
            const auto depth = ElementDepth(input.ordinary.Elements(), input.insertion->parent, input.limits.maximumDepth);
            if (!depth)
                return fail();
            ExpansionBudget walk{*input.insertionCatalog, cancellation, counters, {}};
            if (const auto valid = walk.Instance(input.insertion->accepted, input.insertion->arguments, input.insertion->slots, 0, *depth);
                valid.HasError())
                return valid;
        }
        for (std::size_t index = 0; index < input.instances.size(); ++index) {
            const auto &instance = input.instances[index];
            const bool replaced = index == input.replacementIndex;
            const UiTemplateCatalog *catalog{};
            if (replaced)
                catalog = input.replacementCatalog;
            else if (input.fallbackCatalog)
                catalog = input.fallbackCatalog;
            else if (!input.leases.empty()) {
                const auto lease = std::ranges::find_if(input.leases, [&](const UiTemplateCatalogLease &candidate) {
                    return candidate.reference.asset == instance.accepted.asset &&
                           candidate.reference.revision == instance.accepted.revision;
                });
                if (lease != input.leases.end())
                    catalog = lease->catalog.get();
            } else
                catalog = input.catalogs[index].get();
            const auto &reference = replaced && input.replacement ? *input.replacement : instance.accepted;
            if (!catalog)
                return fail();
            const auto parentDepth = ElementDepth(input.ordinary.Elements(), instance.parent, input.limits.maximumDepth);
            if (!parentDepth)
                return fail();
            ExpansionBudget walk{*catalog, cancellation, counters, {}};
            if (const auto valid = walk.Instance(reference, instance.arguments, instance.slots, 0, *parentDepth); valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::UiTemplates::Detail
