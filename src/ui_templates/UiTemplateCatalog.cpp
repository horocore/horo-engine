#include "Horo/UiTemplates/UiTemplateCatalog.h"

#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateBudget.h"
#include "UiTemplateParameterValidation.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <set>

namespace Horo::UiTemplates {
    namespace {
        /** @brief Returns a bounded schema failure without exposing argument or binding payloads. */
        Result<void> Invalid(const char *reason) {
            return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid, reason));
        }

        /** @brief Validates the finite registered schema before any source document is admitted. */
        Result<void> ValidateSchemas(const UiTemplateSchemaSnapshot &schemas, const UiTemplateLimits &limits) {
            if (schemas.elementTypes.empty() || schemas.elementTypes.size() > limits.maximumElements ||
                schemas.properties.size() > limits.maximumProperties || schemas.bindingProviders.size() > limits.maximumParameters)
                return Invalid("Template schema snapshot exceeds its finite bounds.");
            std::set<Assets::AssetTypeId> types;
            for (const auto &type : schemas.elementTypes)
                if (type.Value().empty() || !types.insert(type).second)
                    return Invalid("Template element schema identities are invalid or duplicated.");
            std::set<std::pair<Assets::AssetTypeId, Runtime::Ui::UiStylePropertyId>> properties;
            for (const auto &property : schemas.properties) {
                if (!types.contains(property.elementType) || !property.property.IsValid() ||
                    !properties.emplace(property.elementType, property.property).second)
                    return Invalid("Template property schema identities are invalid or duplicated.");
                if (property.kind <= UiTemplateValueKind::Reference) {
                    if (property.documentKey.empty() || property.documentKey.size() > Runtime::Ui::MaximumUiDocumentTextBytes ||
                        property.style)
                        return Invalid("An ordinary registered template property needs one bounded codec key.");
                } else if (property.kind == UiTemplateValueKind::Style) {
                    if (!property.style || !property.style->IsValid() || property.style->id != property.property ||
                        !property.documentKey.empty())
                        return Invalid("A style template property needs its exact typed registered schema.");
                } else if (property.kind > UiTemplateValueKind::Binding || !property.documentKey.empty() || property.style)
                    return Invalid("Template attachment schema category is invalid.");
            }
            return Result<void>::Success();
        }

        /** @brief Finds an exact registered pair without a name, path or implicit type fallback. */
        const UiTemplatePropertySchema *PropertySchema(const UiTemplateSchemaSnapshot &schemas, const Assets::AssetTypeId &element,
                                                       const Runtime::Ui::UiStylePropertyId property) {
            const auto found = std::ranges::find_if(schemas.properties, [&](const UiTemplatePropertySchema &schema) {
                return schema.elementType == element && schema.property == property;
            });
            return found == schemas.properties.end() ? nullptr : &*found;
        }

        /** @brief Checks type and primitive validity; full ordinary document/reference validation occurs after composition. */
        Result<void> ValidateValue(const UiTemplateValue &value, const UiTemplatePropertySchema &schema,
                                   const UiTemplateSchemaSnapshot &schemas) {
            if (value.valueless_by_exception() || TemplateValueKind(value) != schema.kind)
                return Invalid("Template property value does not match its exact registered category.");
            if (const auto *number = std::get_if<double>(&value); number && !std::isfinite(*number))
                return Invalid("Template numeric value is non-finite.");
            if (const auto *text = std::get_if<std::string>(&value); text && text->size() > Runtime::Ui::MaximumUiDocumentTextBytes)
                return Invalid("Template text value exceeds the registered document bound.");
            if (const auto *style = std::get_if<Runtime::Ui::UiStyleValue>(&value)) {
                if (style->valueless_by_exception() || Runtime::Ui::UiStyleValueCategoryOf(*style) != schema.style->category)
                    return Invalid("Template style value category differs from its registered property.");
                const bool valid = std::visit([]<typename Value>(const Value &item) {
                    if constexpr (requires { item.IsValid(); })
                        return item.IsValid();
                    else
                        return true;
                }, *style);
                if (!valid)
                    return Invalid("Template style literal is invalid.");
                if (const auto *scalar = std::get_if<Runtime::Ui::UiStyleScalar>(style);
                    scalar && (scalar->value < schema.style->minimumScalar || scalar->value > schema.style->maximumScalar))
                    return Invalid("Template style scalar exceeds registered constraints.");
            }
            if (const auto *localized = std::get_if<Runtime::Ui::LocalizedMessageRef>(&value); localized && !localized->IsValid())
                return Invalid("Template localized reference is invalid.");
            if (const auto *binding = std::get_if<Runtime::Ui::UiBindingDescriptor>(&value)) {
                const auto found =
                    std::ranges::find_if(schemas.bindingProviders, [&](const Runtime::Ui::UiBindingProviderSchema &provider) {
                    return Runtime::Ui::ValidateUiBindingDescriptor(*binding, provider).HasValue();
                });
                if (found == schemas.bindingProviders.end())
                    return Invalid("Template binding has no exact compatible pinned provider schema.");
            }
            return Result<void>::Success();
        }

        /** @brief Checks unique registered elements and connected local parentage under the global depth bound. */
        Result<void> ValidateElements(const UiTemplateDocument &document, const UiTemplateSchemaSnapshot &schemas,
                                      const UiTemplateLimits &limits) {
            if (!document.root.IsValid() || document.elements.empty() || document.elements.size() > limits.maximumElements)
                return Invalid("Template root or source element count is invalid.");
            std::set<UiTemplateLocalElementId> ids;
            std::size_t properties{};
            for (const auto &element : document.elements) {
                if (!element.id.IsValid() || !ids.insert(element.id).second ||
                    std::ranges::find(schemas.elementTypes, element.type) == schemas.elementTypes.end())
                    return Invalid("Template element identity or registered kind is invalid.");
                if (element.properties.size() > limits.maximumProperties - properties)
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
                properties += element.properties.size();
                std::set<Runtime::Ui::UiStylePropertyId> written;
                for (const auto &property : element.properties) {
                    const auto *schema = PropertySchema(schemas, element.type, property.property);
                    if (!schema || !written.insert(property.property).second)
                        return Invalid("Template property is unregistered or duplicated.");
                    if (const auto valid = ValidateValue(property.value, *schema, schemas); valid.HasError())
                        return valid;
                }
            }
            for (const auto &element : document.elements) {
                const UiTemplateElement *current = &element;
                std::size_t depth{1};
                while (current->parent.IsValid()) {
                    if (current->id == document.root || ++depth > limits.maximumDepth)
                        return Invalid("Template root has a parent, or hierarchy is cyclic or too deep.");
                    const auto parent = std::ranges::find(document.elements, current->parent, &UiTemplateElement::id);
                    if (parent == document.elements.end())
                        return Invalid("Template element parent is absent.");
                    current = &*parent;
                }
                if (current->id != document.root)
                    return Invalid("Template hierarchy contains a disconnected root.");
            }
            return Result<void>::Success();
        }

        /** @brief Rejects malformed public interface identities and unbounded target/slot declarations. */
        Result<void> ValidateInterface(const UiTemplateDocument &document, const UiTemplateLimits &limits) {
            if (document.parameters.size() > limits.maximumParameters || document.slots.size() > limits.maximumSlots ||
                document.nested.size() > limits.maximumInstances)
                return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
            std::set<UiTemplateParameterId> parameters;
            for (const auto &parameter : document.parameters) {
                if (!parameter.id.IsValid() || !parameters.insert(parameter.id).second || parameter.kind > UiTemplateValueKind::Binding ||
                    parameter.targets.empty() || parameter.targets.size() > limits.maximumProperties || parameter.maximumTextBytes == 0 ||
                    parameter.maximumTextBytes > Runtime::Ui::MaximumUiDocumentTextBytes)
                    return Invalid("Template parameter identity, category or target budget is invalid.");
                if (const auto constraints = Detail::ValidateParameterConstraints(parameter); constraints.HasError())
                    return constraints;
                if (parameter.defaultValue)
                    if (const auto value = Detail::ValidateParameterValue(parameter, *parameter.defaultValue); value.HasError())
                        return value;
            }
            std::set<UiTemplateSlotId> slots;
            for (const auto &slot : document.slots) {
                if (!slot.id.IsValid() || !slots.insert(slot.id).second || slot.minimumChildren > slot.maximumChildren ||
                    slot.maximumChildren > limits.maximumElements || slot.allowedRootKinds.empty() ||
                    slot.allowedRootKinds.size() > limits.maximumElements || slot.defaultContent.size() > limits.maximumElements ||
                    std::ranges::find(document.elements, slot.parent, &UiTemplateElement::id) == document.elements.end())
                    return Invalid("Template slot identity, parent, cardinality or source budget is invalid.");
            }
            std::set<UiTemplateInstanceId> instances;
            for (const auto &nested : document.nested)
                if (!nested.id.IsValid() || !instances.insert(nested.id).second ||
                    std::ranges::find(document.elements, nested.parent, &UiTemplateElement::id) == document.elements.end() ||
                    nested.arguments.size() > limits.maximumParameters || nested.slots.size() > limits.maximumSlots)
                    return Invalid("Nested template identity, parent or customization budget is invalid.");
            return Result<void>::Success();
        }

        /** @brief Validates every declared target, including targets whose optional parameter has no value. */
        Result<void> ValidateTargets(const UiTemplateDocument &document, const std::span<const UiTemplateDocument> documents,
                                     const UiTemplateSchemaSnapshot &schemas) {
            std::set<std::pair<UiTemplateLocalElementId, Runtime::Ui::UiStylePropertyId>> written;
            std::vector<const UiTemplateNestedParameterTarget *> forwarded;
            for (const auto &parameter : document.parameters)
                for (const auto &target : parameter.targets) {
                    if (target.valueless_by_exception())
                        return Invalid("Template target has no category.");
                    const auto valid = std::visit([&]<typename Target>(const Target &item) -> Result<void> {
                        if constexpr (std::is_same_v<Target, UiTemplateNestedParameterTarget>) {
                            if (!item.parameter.IsValid() || item.lineage.empty() || item.lineage.size() > 32)
                                return Invalid("Template forwarding target has no bounded stable lineage.");
                            const UiTemplateDocument *current = &document;
                            for (const auto instance : item.lineage) {
                                const auto child = std::ranges::find(current->nested, instance, &UiTemplateNestedInstance::id);
                                if (child == current->nested.end())
                                    return Invalid("Template forwarding names an absent nested instance.");
                                const auto next =
                                    std::ranges::find(documents, child->accepted.asset, [](const UiTemplateDocument &candidate) {
                                    return candidate.dependency.asset;
                                });
                                if (next == documents.end() || next->dependency.revision != child->accepted.revision)
                                    return Invalid("Template forwarding has no exact pinned target document.");
                                current = &*next;
                            }
                            const auto destination = std::ranges::find(current->parameters, item.parameter, &UiTemplateParameter::id);
                            if (destination == current->parameters.end() || destination->kind != parameter.kind)
                                return Invalid("Template forwarding has no exact type-compatible declared parameter.");
                            if (std::ranges::any_of(forwarded, [&](const auto *previous) {
                                return previous->lineage == item.lineage && previous->parameter == item.parameter;
                            }))
                                return Invalid("Template forwarding targets are duplicated.");
                            forwarded.push_back(&item);
                            return Result<void>::Success();
                        } else {
                            const auto element = std::ranges::find(document.elements, item.element, &UiTemplateElement::id);
                            if (element == document.elements.end() || !written.emplace(item.element, item.property).second)
                                return Invalid("Template parameter property target is absent or duplicated.");
                            const auto *schema = PropertySchema(schemas, element->type, item.property);
                            if (!schema || schema->kind != parameter.kind)
                                return Invalid("Template parameter has no exact registered target property category.");
                            if constexpr (std::is_same_v<Target, UiTemplateContentTarget>) {
                                if (parameter.kind > UiTemplateValueKind::Reference)
                                    return Invalid("An ordinary template target cannot address a rich attachment.");
                            } else if constexpr (std::is_same_v<Target, UiTemplateStyleTarget>) {
                                if (parameter.kind != UiTemplateValueKind::Style)
                                    return Invalid("A style target requires a typed style parameter.");
                            } else if (item.kind != parameter.kind ||
                                       (item.kind != UiTemplateValueKind::LocalizedText && item.kind != UiTemplateValueKind::Binding))
                                return Invalid("A template attachment target requires its exact declared attachment category.");
                            return parameter.defaultValue ? ValidateValue(*parameter.defaultValue, *schema, schemas)
                                                          : Result<void>::Success();
                        }
                    }, target);
                    if (valid.HasError())
                        return valid;
                }
            return Result<void>::Success();
        }

        /** @brief Derives graph edges from the single owned nested-record authority, including recursive slot uses. */
        void AppendSlotDependencies(const std::span<const UiTemplateSlotContent> slots, std::vector<UiTemplateReference> &references) {
            for (const auto &slot : slots)
                for (const auto &nested : slot.nested) {
                    references.push_back(nested.accepted);
                    AppendSlotDependencies(nested.slots, references);
                }
        }
    }  // namespace

    /** @copydoc UiTemplateCatalog::Create */
    Result<UiTemplateCatalog> UiTemplateCatalog::Create(std::vector<UiTemplateDocument> documents, UiTemplateSchemaSnapshot schemas,
                                                        const std::span<const Packages::LockedPackageReference> packages,
                                                        const UiTemplateLimits &limits) {
        if (!limits.IsValid() || documents.empty() || documents.size() > limits.maximumInstances)
            return Result<UiTemplateCatalog>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        try {
            if (const auto valid = Detail::ValidateTemplateSourceBudget(documents, limits); valid.HasError())
                return Result<UiTemplateCatalog>::Failure(valid.ErrorValue());
            if (const auto valid = ValidateSchemas(schemas, limits); valid.HasError())
                return Result<UiTemplateCatalog>::Failure(valid.ErrorValue());
            std::vector<UiTemplateDependencyDescriptor> dependencies;
            dependencies.reserve(documents.size());
            for (const auto &document : documents) {
                if (const auto valid = ValidateElements(document, schemas, limits); valid.HasError())
                    return Result<UiTemplateCatalog>::Failure(valid.ErrorValue());
                if (const auto valid = ValidateInterface(document, limits); valid.HasError())
                    return Result<UiTemplateCatalog>::Failure(valid.ErrorValue());
                if (const auto valid = ValidateTargets(document, documents, schemas); valid.HasError())
                    return Result<UiTemplateCatalog>::Failure(valid.ErrorValue());
                UiTemplateDependencyDescriptor dependency{document.dependency.asset,
                                                          document.dependency.schema,
                                                          document.dependency.interfaceVersion,
                                                          document.dependency.revision,
                                                          {},
                                                          document.dependency.packages};
                for (const auto &nested : document.nested) {
                    dependency.nested.push_back(nested.accepted);
                    AppendSlotDependencies(nested.slots, dependency.nested);
                }
                dependencies.push_back(std::move(dependency));
            }
            auto graph = UiTemplateDependencyGraph::Create(dependencies, packages);
            if (graph.HasError())
                return Result<UiTemplateCatalog>::Failure(graph.ErrorValue());
            for (const auto &document : documents) {
                const UiTemplateReference root{document.dependency.asset, document.dependency.revision,
                                               document.dependency.interfaceVersion};
                if (const auto resolved = graph.Value().Resolve(root); resolved.HasError())
                    return Result<UiTemplateCatalog>::Failure(resolved.ErrorValue());
            }
            return Result<UiTemplateCatalog>::Success(
                UiTemplateCatalog{std::move(documents), std::move(schemas), std::move(graph).Value(), limits});
        } catch (const std::bad_alloc &) {
            return Result<UiTemplateCatalog>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }

    /** @brief Adopts complete validated snapshots without borrowing the host's mutable registries. */
    UiTemplateCatalog::UiTemplateCatalog(std::vector<UiTemplateDocument> documents, UiTemplateSchemaSnapshot schemas,
                                         UiTemplateDependencyGraph graph, UiTemplateLimits limits) noexcept
        : documents_(std::move(documents)), schemas_(std::move(schemas)), graph_(std::move(graph)), limits_(limits) {}

    /** @copydoc UiTemplateCatalog::Resolve */
    Result<const UiTemplateDocument *> UiTemplateCatalog::Resolve(const UiTemplateReference &reference) const {
        if (!active_)
            return Result<const UiTemplateDocument *>::Failure(MakeError(UiErrors::TemplateGraphShutdown));
        if (!reference.asset.IsValid() || reference.revision == Sha256Digest{} || reference.minimumInterface.major == 0)
            return Result<const UiTemplateDocument *>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
        const auto found = std::ranges::find(documents_, reference.asset, [](const UiTemplateDocument &document) {
            return document.dependency.asset;
        });
        if (found == documents_.end())
            return Result<const UiTemplateDocument *>::Failure(MakeError(UiErrors::TemplateMissing));
        if (found->dependency.revision != reference.revision)
            return Result<const UiTemplateDocument *>::Failure(MakeError(UiErrors::TemplateRevisionUnavailable));
        const auto version = found->dependency.interfaceVersion;
        if (version.major != reference.minimumInterface.major || version.minor < reference.minimumInterface.minor)
            return Result<const UiTemplateDocument *>::Failure(MakeError(UiErrors::TemplateVersionIncompatible));
        // Create already validated every complete graph/lock closure. Exact lookup needs no new traversal allocation.
        return Result<const UiTemplateDocument *>::Success(&*found);
    }

    /** @copydoc UiTemplateCatalog::Schemas */
    const UiTemplateSchemaSnapshot &UiTemplateCatalog::Schemas() const noexcept {
        return schemas_;
    }

    /** @copydoc UiTemplateCatalog::Limits */
    const UiTemplateLimits &UiTemplateCatalog::Limits() const noexcept {
        return limits_;
    }

    /** @copydoc UiTemplateCatalog::ValidateProperty */
    Result<void> UiTemplateCatalog::ValidateProperty(const Assets::AssetTypeId &element, const Runtime::Ui::UiStylePropertyId property,
                                                     const UiTemplateValue &value) const {
        if (!active_)
            return Result<void>::Failure(MakeError(UiErrors::TemplateGraphShutdown));
        const auto *schema = PropertySchema(schemas_, element, property);
        if (!schema)
            return Invalid("Template target property is not registered for its exact element kind.");
        return ValidateValue(value, *schema, schemas_);
    }

    /** @copydoc UiTemplateCatalog::Shutdown */
    void UiTemplateCatalog::Shutdown() noexcept {
        active_ = false;
        graph_.Shutdown();
    }
}  // namespace Horo::UiTemplates
