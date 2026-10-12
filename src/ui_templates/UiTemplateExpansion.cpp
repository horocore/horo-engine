#include "Horo/UiTemplates/UiTemplateExpansion.h"

#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateBudget.h"
#include "UiTemplateIdentityEncoder.h"
#include "UiTemplateParameterValidation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <new>
#include <set>
#include <string_view>

namespace Horo::UiTemplates {
    namespace {
        using namespace Runtime::Ui;

        /** @brief Refuses a complete candidate without including user content in diagnostics. */
        Result<void> Invalid(const char *reason) {
            return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid, reason));
        }

        /** @brief Separates authored default-fragment IDs from local elements and from other instance/slot scopes. */
        Result<UiElementId> DefaultElementId(const UiDocumentId document, const std::span<const UiTemplateInstanceId> lineage,
                                             const UiTemplateSlotId slot, const UiElementId authored) {
            if (!slot.IsValid() || !authored.IsValid())
                return Result<UiElementId>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
            const std::array suffix{slot.Bytes(), authored.Bytes()};
            return Detail::EncodeTemplateIdentity(Detail::TemplateIdentityDomain::SlotDefault, document, lineage, suffix);
        }

        /** @brief One typed argument write forwarded through an exact stable nested lineage. */
        struct Forward final {
            std::vector<UiTemplateInstanceId> lineage;
            UiTemplateArgument argument;
        };

        /** @brief Checks declared parameter type and scalar/text constraints before any target write. */
        Result<void> ValidateArgument(const UiTemplateParameter &parameter, const UiTemplateValue &value) {
            return Detail::ValidateParameterValue(parameter, value);
        }

        /** @brief Owner of one private bounded expansion; recursive calls borrow this synchronous stack only. */
        class Expansion final {
        public:
            Expansion(const UiTemplateCatalog &catalog, const UiDocumentId document, const CancellationToken &cancellation,
                      const std::span<const UiElementId> occupied)
                : catalog_(catalog), document_(document), cancellation_(cancellation), identities_(occupied.begin(), occupied.end()) {}

            Result<UiTemplateExpansion> Run(const UiTemplateInstanceRecord &instance) {
                if (!document_.IsValid() || !instance.id.IsValid() || !instance.parent.IsValid() || !identities_.contains(instance.parent))
                    return Result<UiTemplateExpansion>::Failure(
                        MakeError(UiErrors::TemplateGraphInvalid, "Template instance has no exact document-owned attachment point."));
                lineage_.push_back(instance.id);
                const auto source = catalog_.Resolve(instance.accepted);
                if (source.HasError())
                    return Result<UiTemplateExpansion>::Failure(source.ErrorValue());
                auto root = DeriveTemplateElementId(document_, lineage_, source.Value()->root);
                if (root.HasError())
                    return Result<UiTemplateExpansion>::Failure(root.ErrorValue());
                output_.root = root.Value();
                if (const auto expanded = Instance(instance.accepted, instance.parent, instance.arguments, instance.slots, {}, 1, true);
                    expanded.HasError())
                    return Result<UiTemplateExpansion>::Failure(expanded.ErrorValue());
                if (const auto valid = ValidateOutput(instance.parent); valid.HasError())
                    return Result<UiTemplateExpansion>::Failure(valid.ErrorValue());
                return Result<UiTemplateExpansion>::Success(std::move(output_));
            }

        private:
            /** @brief Checks the fully assembled hierarchy rather than treating nested call depth as actual element depth. */
            Result<void> ValidateOutput(const UiElementId attachment) const {
                for (const auto &element : output_.elements) {
                    const UiDocumentElement *current = &element;
                    std::size_t depth{};
                    while (current->parent != attachment) {
                        if (++depth >= catalog_.Limits().maximumDepth)
                            return Invalid("Expanded template hierarchy is cyclic or exceeds the complete depth budget.");
                        const auto parent = std::ranges::find(output_.elements, current->parent, &UiDocumentElement::id);
                        if (parent == output_.elements.end())
                            return Invalid("Expanded template hierarchy is disconnected from its owning attachment.");
                        current = &*parent;
                    }
                    if (element.properties.size() > MaximumUiDocumentProperties || element.references.size() > MaximumUiDocumentReferences)
                        return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
                }
                return Result<void>::Success();
            }

            /** @brief Accounts work and cancellation before allocating another output element. */
            Result<void> Admit(const std::size_t depth) const {
                if (cancellation_.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphShutdown, "Template preparation was cancelled."));
                if (depth > catalog_.Limits().maximumDepth || output_.elements.size() >= catalog_.Limits().maximumElements)
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
                return Result<void>::Success();
            }

            /** @brief Applies a validated argument to one property or forwards it through an exact nested lineage. */
            Result<void> WriteArgumentTarget(const UiTemplateParameterTarget &target, const UiTemplateValue &value,
                                             std::vector<UiTemplateElement> &elements, std::vector<Forward> &forwarded,
                                             std::set<std::pair<UiTemplateLocalElementId, UiStylePropertyId>> &targets) {
                return std::visit([&]<typename Target>(const Target &item) -> Result<void> {
                    if constexpr (std::is_same_v<Target, UiTemplateNestedParameterTarget>) {
                        if (item.lineage.empty() || item.lineage.size() > catalog_.Limits().maximumDepth || !item.parameter.IsValid())
                            return Invalid("Template forwarding lineage is invalid.");
                        if (std::ranges::any_of(forwarded, [&](const Forward &previous) {
                            return previous.lineage == item.lineage && previous.argument.parameter == item.parameter;
                        }))
                            return Invalid("Template parameters write the same nested target twice.");
                        forwarded.push_back({item.lineage, {item.parameter, value}});
                        return Result<void>::Success();
                    } else {
                        const auto element = std::ranges::find(elements, item.element, &UiTemplateElement::id);
                        if (element == elements.end() || !targets.emplace(item.element, item.property).second)
                            return Invalid("Template parameter target is absent or written more than once.");
                        if (const auto valid = catalog_.ValidateProperty(element->type, item.property, value); valid.HasError())
                            return valid;
                        const auto property = std::ranges::find(element->properties, item.property, &UiTemplateProperty::property);
                        if (property == element->properties.end())
                            element->properties.push_back({item.property, value});
                        else
                            property->value = value;
                        return Result<void>::Success();
                    }
                }, target);
            }

            /** @brief Applies parameter writes to private element copies and explicit nested forwarding. */
            Result<void> Arguments(const UiTemplateDocument &source, const std::span<const UiTemplateArgument> arguments,
                                   std::vector<UiTemplateElement> &elements, std::vector<Forward> &forwarded) {
                if (arguments.size() > catalog_.Limits().maximumParameters)
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
                std::set<UiTemplateParameterId> supplied;
                for (const auto &argument : arguments)
                    if (!supplied.insert(argument.parameter).second ||
                        std::ranges::find(source.parameters, argument.parameter, &UiTemplateParameter::id) == source.parameters.end())
                        return Invalid("Template arguments contain an unknown or duplicated parameter.");
                std::set<std::pair<UiTemplateLocalElementId, UiStylePropertyId>> targets;
                for (const auto &parameter : source.parameters) {
                    const auto argument = std::ranges::find(arguments, parameter.id, &UiTemplateArgument::parameter);
                    const UiTemplateValue *value = argument != arguments.end() ? &argument->value
                                                   : parameter.defaultValue    ? &*parameter.defaultValue
                                                                               : nullptr;
                    if (!value) {
                        if (parameter.required)
                            return Invalid("A required template parameter has no admitted value.");
                        continue;
                    }
                    if (const auto valid = ValidateArgument(parameter, *value); valid.HasError())
                        return valid;
                    for (const auto &target : parameter.targets) {
                        const auto written = WriteArgumentTarget(target, *value, elements, forwarded, targets);
                        if (written.HasError())
                            return written;
                    }
                }
                return Result<void>::Success();
            }

            /** @brief Converts an exact validated literal into ordinary content or an existing typed composition input. */
            Result<void> Property(UiDocumentElement &element, const UiTemplateProperty &property) {
                if (++properties_ > catalog_.Limits().maximumProperties)
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
                if (const auto valid = catalog_.ValidateProperty(element.type, property.property, property.value); valid.HasError())
                    return valid;
                const auto schema = std::ranges::find_if(catalog_.Schemas().properties, [&](const UiTemplatePropertySchema &candidate) {
                    return candidate.elementType == element.type && candidate.property == property.property;
                });
                return std::visit([&]<typename Value>(const Value &value) -> Result<void> {
                    if constexpr (std::is_same_v<Value, UiStyleValue>)
                        output_.styles.push_back({element.id, {property.property, UiStyleValueSource::Literal(value), false}});
                    else if constexpr (std::is_same_v<Value, LocalizedMessageRef>)
                        output_.localizedTexts.push_back({element.id, value, property.property});
                    else if constexpr (std::is_same_v<Value, UiBindingDescriptor>) {
                        auto binding = value;
                        auto local = UiTemplateLocalElementId::Create(binding.id.Bytes());
                        if (local.HasError())
                            return Invalid("Template binding has no stable authored identity.");
                        auto derived = DeriveTemplateElementId(document_, lineage_, local.Value());
                        if (derived.HasError())
                            return Result<void>::Failure(derived.ErrorValue());
                        binding.id = UiBindingId::Create(derived.Value().Bytes()).Value();
                        if (!bindings_.insert(binding.id).second)
                            return Invalid("Expanded template binding identities collide.");
                        binding.target.element = element.id;
                        output_.bindings.push_back(std::move(binding));
                    } else {
                        auto ordinary = value;
                        if constexpr (std::is_same_v<Value, UiReference>) {
                            if (ordinary.kind == UiReferenceKind::Element) {
                                const auto local = UiTemplateLocalElementId::Create(ordinary.element.Bytes());
                                if (local.HasError())
                                    return Invalid("Template element reference is malformed.");
                                const auto found = localIds_.find(local.Value());
                                if (found == localIds_.end())
                                    return Invalid("Template reference escapes its declared local element hierarchy.");
                                ordinary.element = found->second;
                            }
                        }
                        element.properties.push_back({schema->documentKey, std::move(ordinary)});
                    }
                    return Result<void>::Success();
                }, property.value);
            }

            /** @brief Materializes local elements in canonical parent-first/local-ID order before nested/slot composition. */
            Result<void> Elements(const std::vector<UiTemplateElement> &elements, const UiTemplateLocalElementId root,
                                  const UiElementId parent, const std::size_t depth) {
                localIds_.clear();
                for (const auto &element : elements) {
                    const auto derived = DeriveTemplateElementId(document_, lineage_, element.id);
                    if (derived.HasError())
                        return Result<void>::Failure(derived.ErrorValue());
                    if (!identities_.insert(derived.Value()).second)
                        return Invalid("Expanded template element identities collide with the target document.");
                    localIds_.emplace(element.id, derived.Value());
                }
                return Element(elements, root, parent, depth);
            }

            /** @brief Visits one local subtree with finite recursion and private output ownership. */
            Result<void> Element(const std::vector<UiTemplateElement> &elements, const UiTemplateLocalElementId id,
                                 const UiElementId parent, const std::size_t depth) {
                if (const auto admitted = Admit(depth); admitted.HasError())
                    return admitted;
                const auto source = std::ranges::find(elements, id, &UiTemplateElement::id);
                if (source == elements.end())
                    return Invalid("Template subtree root is missing.");
                UiDocumentElement element{localIds_.at(id), parent, source->type, {}, {}};
                auto properties = source->properties;
                std::ranges::sort(properties, {}, &UiTemplateProperty::property);
                for (const auto &property : properties)
                    if (const auto written = Property(element, property); written.HasError())
                        return written;
                output_.templateOwnedElements.push_back(element.id);
                output_.elements.push_back(std::move(element));
                std::vector<UiTemplateLocalElementId> children;
                for (const auto &child : elements)
                    if (child.parent == id)
                        children.push_back(child.id);
                std::ranges::sort(children);
                for (const auto child : children)
                    if (const auto written = Element(elements, child, localIds_.at(id), depth + 1); written.HasError())
                        return written;
                return Result<void>::Success();
            }

            /** @brief Maps template-authored defaults into their distinct slot/instance identity domain. */
            Result<void> RemapDefaults(const UiTemplateSlotId slot, std::vector<UiDocumentElement> &elements) {
                std::map<UiElementId, UiElementId> defaults;
                for (const auto &element : elements) {
                    const auto derived = DefaultElementId(document_, lineage_, slot, element.id);
                    if (derived.HasError())
                        return Result<void>::Failure(derived.ErrorValue());
                    if (!defaults.emplace(element.id, derived.Value()).second)
                        return Invalid("Default slot content has duplicated authored identities.");
                }
                const auto remap = [&](UiReference &reference) -> Result<void> {
                    if (reference.kind != UiReferenceKind::Element)
                        return Result<void>::Success();
                    const auto found = defaults.find(reference.element);
                    if (found == defaults.end())
                        return Invalid("Default slot element reference escapes its authored fragment.");
                    reference.element = found->second;
                    return Result<void>::Success();
                };
                for (auto &element : elements) {
                    element.id = defaults.at(element.id);
                    if (element.parent.IsValid())
                        element.parent = defaults.at(element.parent);
                    for (auto &reference : element.references)
                        if (const auto valid = remap(reference); valid.HasError())
                            return valid;
                    for (auto &property : element.properties)
                        if (auto *reference = std::get_if<UiReference>(&property.value))
                            if (const auto valid = remap(*reference); valid.HasError())
                                return valid;
                }
                return Result<void>::Success();
            }

            /** @brief Publishes a validated fragment in canonical parent-first order and returns its exact owned IDs. */
            Result<std::set<UiElementId>> AppendSlotElements(const UiTemplateSlot &slot, std::vector<UiDocumentElement> elements,
                                                             const UiElementId parent, const std::size_t depth, const bool templateOwned) {
                std::set<UiElementId> fragmentIds;
                for (const auto &element : elements)
                    if (!element.id.IsValid() || !fragmentIds.insert(element.id).second)
                        return Result<std::set<UiElementId>>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
                const auto fragmentDepth = [&](const UiElementId id) {
                    std::size_t result{1};
                    auto current = std::ranges::find(elements, id, &UiDocumentElement::id);
                    while (current->parent.IsValid()) {
                        current = std::ranges::find(elements, current->parent, &UiDocumentElement::id);
                        ++result;
                    }
                    return result;
                };
                std::vector<std::pair<std::size_t, UiElementId>> order;
                order.reserve(elements.size());
                for (const auto &element : elements)
                    order.emplace_back(fragmentDepth(element.id), element.id);
                std::ranges::sort(order);
                std::size_t roots{};
                for (const auto &[elementDepth, id] : order) {
                    auto &element = *std::ranges::find(elements, id, &UiDocumentElement::id);
                    std::ranges::sort(element.properties, {}, &UiTypedProperty::key);
                    std::ranges::sort(element.references);
                    if (!element.parent.IsValid()) {
                        ++roots;
                        if (std::ranges::find(slot.allowedRootKinds, element.type) == slot.allowedRootKinds.end())
                            return Result<std::set<UiElementId>>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
                        element.parent = parent;
                    } else if (!fragmentIds.contains(element.parent))
                        return Result<std::set<UiElementId>>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
                    if (const auto admitted = Admit(depth + 1); admitted.HasError())
                        return Result<std::set<UiElementId>>::Failure(admitted.ErrorValue());
                    if (!identities_.insert(element.id).second)
                        return Result<std::set<UiElementId>>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
                    if (templateOwned)
                        output_.templateOwnedElements.push_back(element.id);
                    output_.elements.push_back(std::move(element));
                }
                if (roots < slot.minimumChildren || roots > slot.maximumChildren)
                    return Result<std::set<UiElementId>>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
                return Result<std::set<UiElementId>>::Success(std::move(fragmentIds));
            }

            /** @brief Expands linked slot uses in stable identity order without copying their owned argument/slot payloads. */
            Result<void> AppendSlotInstances(const std::span<const UiTemplateSlotNestedInstance> nested,
                                             const std::set<UiElementId> &fragmentIds, const std::size_t depth, const UiTemplateSlotId slot,
                                             const bool documentOwned) {
                std::vector<const UiTemplateSlotNestedInstance *> children;
                children.reserve(nested.size());
                for (const auto &child : nested)
                    children.push_back(&child);
                std::ranges::sort(children, {}, [](const auto *child) {
                    return child->id;
                });
                for (const auto *child : children) {
                    const auto parent = documentOwned ? Result<UiElementId>::Success(child->parent)
                                                      : DefaultElementId(document_, lineage_, slot, child->parent);
                    if (parent.HasError())
                        return Result<void>::Failure(parent.ErrorValue());
                    if (!fragmentIds.contains(parent.Value()) || !child->id.IsValid())
                        return Invalid("Nested slot template has no document-owned slot parent.");
                    lineage_.push_back(child->id);
                    const auto expanded =
                        Instance(child->accepted, parent.Value(), child->arguments, child->slots, {}, depth + 1, documentOwned);
                    lineage_.pop_back();
                    if (expanded.HasError())
                        return expanded;
                }
                return Result<void>::Success();
            }

            /** @brief Composes separately validated default identities, ordinary fragments and nested linked slot uses. */
            Result<void> Slots(const UiTemplateDocument &source, const std::span<const UiTemplateSlotContent> content,
                               const std::map<UiTemplateLocalElementId, UiElementId> &parents, const std::size_t depth,
                               const bool documentOwned) {
                if (content.size() > catalog_.Limits().maximumSlots)
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
                std::set<UiTemplateSlotId> supplied;
                for (const auto &slot : content)
                    if (!supplied.insert(slot.slot).second ||
                        std::ranges::find(source.slots, slot.slot, &UiTemplateSlot::id) == source.slots.end())
                        return Invalid("Template content targets an unknown or duplicated slot.");
                std::vector<const UiTemplateSlot *> slots;
                slots.reserve(source.slots.size());
                for (const auto &slot : source.slots)
                    slots.push_back(&slot);
                std::ranges::sort(slots, {}, [](const auto *slot) {
                    return slot->id;
                });
                for (const auto *selected : slots) {
                    const auto &slot = *selected;
                    const auto suppliedContent = std::ranges::find(content, slot.id, &UiTemplateSlotContent::slot);
                    if (suppliedContent != content.end() && (suppliedContent->elements.size() > catalog_.Limits().maximumElements ||
                                                             suppliedContent->nested.size() > catalog_.Limits().maximumInstances))
                        return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
                    auto elements = suppliedContent == content.end() ? slot.defaultContent : suppliedContent->elements;
                    const bool templateOwned = suppliedContent == content.end() || !documentOwned;
                    if (templateOwned)
                        if (const auto remapped = RemapDefaults(slot.id, elements); remapped.HasError())
                            return remapped;
                    auto fragment = AppendSlotElements(slot, std::move(elements), parents.at(slot.parent), depth, templateOwned);
                    if (fragment.HasError())
                        return Result<void>::Failure(fragment.ErrorValue());
                    if (suppliedContent != content.end())
                        if (const auto expanded =
                                AppendSlotInstances(suppliedContent->nested, fragment.Value(), depth, slot.id, documentOwned);
                            expanded.HasError())
                            return expanded;
                }
                return Result<void>::Success();
            }

            /** @brief Resolves canonical child order and routes forwarded arguments before recursive expansion. */
            Result<void> ExpandNested(const std::span<const UiTemplateNestedInstance> authored,
                                      const std::map<UiTemplateLocalElementId, UiElementId> &parents,
                                      const std::span<const Forward> forwarded, const std::size_t depth) {
                std::vector<UiTemplateNestedInstance> nested(authored.begin(), authored.end());
                std::ranges::sort(nested, {}, &UiTemplateNestedInstance::id);
                for (auto &child : nested) {
                    std::vector<Forward> remaining;
                    for (const auto &forward : forwarded) {
                        if (forward.lineage.front() != child.id)
                            continue;
                        if (forward.lineage.size() == 1) {
                            const auto argument =
                                std::ranges::find(child.arguments, forward.argument.parameter, &UiTemplateArgument::parameter);
                            if (argument == child.arguments.end())
                                child.arguments.push_back(forward.argument);
                            else
                                argument->value = forward.argument.value;
                        } else
                            remaining.push_back({{forward.lineage.begin() + 1, forward.lineage.end()}, forward.argument});
                    }
                    lineage_.push_back(child.id);
                    const auto expanded = Instance(child.accepted, parents.at(child.parent), child.arguments, child.slots,
                                                   std::move(remaining), depth + 1, false);
                    lineage_.pop_back();
                    if (expanded.HasError())
                        return expanded;
                }
                for (const auto &forward : forwarded)
                    if (std::ranges::find(nested, forward.lineage.front(), &UiTemplateNestedInstance::id) == nested.end())
                        return Invalid("Template forwarding lineage names an absent nested instance.");
                return Result<void>::Success();
            }

            /** @brief Expands a pinned nested use while rejecting asset cycles independently of revision selection. */
            Result<void> Instance(const UiTemplateReference &reference, const UiElementId parent,
                                  const std::span<const UiTemplateArgument> arguments, const std::span<const UiTemplateSlotContent> slots,
                                  std::vector<Forward> forwarded, const std::size_t depth, const bool documentOwned) {
                if (++instances_ > catalog_.Limits().maximumInstances || lineage_.size() > catalog_.Limits().maximumDepth)
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
                if (std::ranges::find(assets_, reference.asset) != assets_.end())
                    return Result<void>::Failure(MakeError(UiErrors::TemplateDependencyCycle));
                const auto resolved = catalog_.Resolve(reference);
                if (resolved.HasError())
                    return Result<void>::Failure(resolved.ErrorValue());
                assets_.push_back(reference.asset);
                const auto &source = *resolved.Value();
                auto elements = source.elements;
                if (const auto applied = Arguments(source, arguments, elements, forwarded); applied.HasError())
                    return applied;
                if (const auto expanded = Elements(elements, source.root, parent, depth); expanded.HasError())
                    return expanded;
                const auto parents = localIds_;
                if (const auto expanded = Slots(source, slots, parents, depth, documentOwned); expanded.HasError())
                    return expanded;
                if (const auto expanded = ExpandNested(source.nested, parents, forwarded, depth); expanded.HasError())
                    return expanded;
                output_.acceptedDependencies.push_back(reference);
                assets_.pop_back();
                return Result<void>::Success();
            }

            const UiTemplateCatalog &catalog_;
            UiDocumentId document_;
            const CancellationToken &cancellation_;
            std::set<UiElementId> identities_;
            std::set<UiBindingId> bindings_;
            std::vector<UiTemplateInstanceId> lineage_;
            std::vector<UiTemplateAssetId> assets_;
            std::map<UiTemplateLocalElementId, UiElementId> localIds_;
            std::size_t instances_{};
            std::size_t properties_{};
            UiTemplateExpansion output_;
        };
    }  // namespace

    /** @copydoc ExpandTemplate */
    Result<UiTemplateExpansion> ExpandTemplate(const UiTemplateCatalog &catalog, const Runtime::Ui::UiDocumentId document,
                                               const UiTemplateInstanceRecord &instance,
                                               const std::span<const Runtime::Ui::UiElementId> occupied,
                                               const CancellationToken &cancellation) {
        if (occupied.empty() || occupied.size() > catalog.Limits().maximumElements ||
            instance.arguments.size() > catalog.Limits().maximumParameters || instance.slots.size() > catalog.Limits().maximumSlots)
            return Result<UiTemplateExpansion>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        try {
            if (const auto admitted = Detail::ValidateTemplateExpansionBudget(catalog, instance, cancellation); admitted.HasError())
                return Result<UiTemplateExpansion>::Failure(admitted.ErrorValue());
            return Expansion{catalog, document, cancellation, occupied}.Run(instance);
        } catch (const std::bad_alloc &) {
            return Result<UiTemplateExpansion>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }
}  // namespace Horo::UiTemplates
