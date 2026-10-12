#include "Horo/UiTemplates/UiTemplateCatalog.h"

#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateBudget.h"
#include "UiTemplateParameterValidation.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <new>
#include <set>

namespace Horo::UiTemplates {
    namespace {
        /** @brief Returns a bounded schema failure without exposing argument or binding payloads. */
        Result<void> Invalid(const char *reason) {
            return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid, reason));
        }

        /** @brief Checks the bounded ordinary codec key with no competing style representation. */
        bool ValidOrdinarySchema(const UiTemplatePropertySchema &property) {
            return !property.documentKey.empty() && property.documentKey.size() <= Runtime::Ui::MaximumUiDocumentTextBytes &&
                   !property.style;
        }

        /** @brief Checks the exact registered style descriptor without dereferencing an absent optional. */
        bool ValidStyleSchema(const UiTemplatePropertySchema &property) {
            return property.style && property.style->IsValid() && property.style->id == property.property && property.documentKey.empty();
        }

        /** @brief Validates one closed property representation independently of identity uniqueness. */
        Result<void> ValidateSchemaShape(const UiTemplatePropertySchema &property) {
            using enum UiTemplateValueKind;
            if (property.kind <= Reference) {
                if (!ValidOrdinarySchema(property))
                    return Invalid("An ordinary registered template property needs one bounded codec key.");
            } else if (property.kind == Style) {
                if (!ValidStyleSchema(property))
                    return Invalid("A style template property needs its exact typed registered schema.");
            } else if (property.kind > Binding || !property.documentKey.empty() || property.style)
                return Invalid("Template attachment schema category is invalid.");
            return Result<void>::Success();
        }

        /** @brief Checks unique property identities only after the complete registered element-type set is admitted. */
        Result<void> ValidateSchemaProperties(const UiTemplateSchemaSnapshot &schemas, const std::set<Assets::AssetTypeId> &types) {
            std::set<std::pair<Assets::AssetTypeId, Runtime::Ui::UiStylePropertyId>> properties;
            for (const auto &property : schemas.properties) {
                if (!types.contains(property.elementType) || !property.property.IsValid())
                    return Invalid("Template property schema identities are invalid or duplicated.");
                if (!properties.emplace(property.elementType, property.property).second)
                    return Invalid("Template property schema identities are invalid or duplicated.");
                if (const auto shape = ValidateSchemaShape(property); shape.HasError())
                    return shape;
            }
            return Result<void>::Success();
        }

        /** @brief Validates the finite registered schema before any source document is admitted. */
        Result<void> ValidateSchemas(const UiTemplateSchemaSnapshot &schemas, const UiTemplateLimits &limits) {
            if (schemas.elementTypes.empty() || schemas.elementTypes.size() > limits.maximumElements ||
                schemas.properties.size() > limits.maximumProperties || schemas.bindingProviders.size() > limits.maximumParameters)
                return Invalid("Template schema snapshot exceeds its finite bounds.");
            std::set<Assets::AssetTypeId> types;
            for (const auto &type : schemas.elementTypes) {
                if (type.Value().empty())
                    return Invalid("Template element schema identities are invalid or duplicated.");
                if (!types.insert(type).second)
                    return Invalid("Template element schema identities are invalid or duplicated.");
            }
            return ValidateSchemaProperties(schemas, types);
        }

        /** @brief Finds an exact registered pair without a name, path or implicit type fallback. */
        const UiTemplatePropertySchema *PropertySchema(const UiTemplateSchemaSnapshot &schemas, const Assets::AssetTypeId &element,
                                                       const Runtime::Ui::UiStylePropertyId property) {
            const auto found = std::ranges::find_if(schemas.properties, [&](const UiTemplatePropertySchema &schema) {
                return schema.elementType == element && schema.property == property;
            });
            return found == schemas.properties.end() ? nullptr : std::to_address(found);
        }

        /** @brief Validates a style literal against its exact registered value category and range. */
        Result<void> ValidateStyleLiteral(const Runtime::Ui::UiStyleValue &style, const Runtime::Ui::UiStylePropertyDescriptor &schema) {
            if (style.valueless_by_exception() || Runtime::Ui::UiStyleValueCategoryOf(style) != schema.category)
                return Invalid("Template style value category differs from its registered property.");
            if (const bool valid = std::visit(
                    []<typename Value>(const Value &item) {
                if constexpr (requires { item.IsValid(); })
                    return item.IsValid();
                else
                    return true;
            }, style);
                !valid)
                return Invalid("Template style literal is invalid.");
            if (const auto *scalar = std::get_if<Runtime::Ui::UiStyleScalar>(&style);
                scalar && (scalar->value < schema.minimumScalar || scalar->value > schema.maximumScalar))
                return Invalid("Template style scalar exceeds registered constraints.");
            return Result<void>::Success();
        }

        /** @brief Admits message references and pinned provider schemas without any live provider call. */
        Result<void> ValidateRichValue(const UiTemplateValue &value, const UiTemplateSchemaSnapshot &schemas) {
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
                if (const auto valid = ValidateStyleLiteral(*style, *schema.style); valid.HasError())
                    return valid;
            }
            return ValidateRichValue(value, schemas);
        }

        /** @brief Validates unique typed properties before hierarchy admission. */
        Result<void> ValidateElementProperties(const UiTemplateElement &element, const UiTemplateSchemaSnapshot &schemas) {
            std::set<Runtime::Ui::UiStylePropertyId> written;
            for (const auto &property : element.properties) {
                const auto *schema = PropertySchema(schemas, element.type, property.property);
                if (!schema)
                    return Invalid("Template property is unregistered or duplicated.");
                if (!written.insert(property.property).second)
                    return Invalid("Template property is unregistered or duplicated.");
                if (const auto valid = ValidateValue(property.value, *schema, schemas); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }

        /** @brief Proves every parent chain reaches the declared root within the admitted depth. */
        Result<void> ValidateHierarchy(const UiTemplateDocument &document, const UiTemplateLimits &limits) {
            for (const auto &element : document.elements) {
                const UiTemplateElement *current = &element;
                std::size_t depth{1};
                while (current->parent.IsValid()) {
                    if (current->id == document.root)
                        return Invalid("Template root has a parent, or hierarchy is cyclic or too deep.");
                    ++depth;
                    if (depth > limits.maximumDepth)
                        return Invalid("Template root has a parent, or hierarchy is cyclic or too deep.");
                    const auto parent = std::ranges::find(document.elements, current->parent, &UiTemplateElement::id);
                    if (parent == document.elements.end())
                        return Invalid("Template element parent is absent.");
                    current = std::to_address(parent);
                }
                if (current->id != document.root)
                    return Invalid("Template hierarchy contains a disconnected root.");
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
                if (!element.id.IsValid() || std::ranges::find(schemas.elementTypes, element.type) == schemas.elementTypes.end())
                    return Invalid("Template element identity or registered kind is invalid.");
                if (!ids.insert(element.id).second)
                    return Invalid("Template element identity or registered kind is invalid.");
                if (element.properties.size() > limits.maximumProperties - properties)
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
                properties += element.properties.size();
                if (const auto valid = ValidateElementProperties(element, schemas); valid.HasError())
                    return valid;
            }
            return ValidateHierarchy(document, limits);
        }

        /** @brief Admits the finite public parameter declaration before uniqueness/default validation. */
        bool ValidParameterDeclaration(const UiTemplateParameter &parameter, const UiTemplateLimits &limits) {
            return !(!parameter.id.IsValid() || parameter.kind > UiTemplateValueKind::Binding || parameter.targets.empty() ||
                     parameter.targets.size() > limits.maximumProperties || parameter.maximumTextBytes == 0 ||
                     parameter.maximumTextBytes > Runtime::Ui::MaximumUiDocumentTextBytes);
        }

        /** @brief Validates bounded typed parameters and their default constraints. */
        Result<void> ValidateParameters(const UiTemplateDocument &document, const UiTemplateLimits &limits) {
            std::set<UiTemplateParameterId> parameters;
            for (const auto &parameter : document.parameters) {
                if (!ValidParameterDeclaration(parameter, limits))
                    return Invalid("Template parameter identity, category or target budget is invalid.");
                if (!parameters.insert(parameter.id).second)
                    return Invalid("Template parameter identity, category or target budget is invalid.");
                if (const auto constraints = Detail::ValidateParameterConstraints(parameter); constraints.HasError())
                    return constraints;
                if (parameter.defaultValue)
                    if (const auto value = Detail::ValidateParameterValue(parameter, *parameter.defaultValue); value.HasError())
                        return value;
            }
            return Result<void>::Success();
        }

        /** @brief Admits declared slot cardinality and registered parent identity. */
        Result<void> ValidateSlots(const UiTemplateDocument &document, const UiTemplateLimits &limits) {
            std::set<UiTemplateSlotId> slots;
            for (const auto &slot : document.slots) {
                if (!slot.id.IsValid() || slot.minimumChildren > slot.maximumChildren || slot.maximumChildren > limits.maximumElements ||
                    slot.allowedRootKinds.empty() || slot.allowedRootKinds.size() > limits.maximumElements ||
                    slot.defaultContent.size() > limits.maximumElements ||
                    std::ranges::find(document.elements, slot.parent, &UiTemplateElement::id) == document.elements.end())
                    return Invalid("Template slot identity, parent, cardinality or source budget is invalid.");
                if (!slots.insert(slot.id).second)
                    return Invalid("Template slot identity, parent, cardinality or source budget is invalid.");
            }
            return Result<void>::Success();
        }

        /** @brief Admits unique nested identities before following their dependency edges. */
        Result<void> ValidateNestedInstances(const UiTemplateDocument &document, const UiTemplateLimits &limits) {
            std::set<UiTemplateInstanceId> instances;
            for (const auto &nested : document.nested) {
                if (!nested.id.IsValid() ||
                    std::ranges::find(document.elements, nested.parent, &UiTemplateElement::id) == document.elements.end() ||
                    nested.arguments.size() > limits.maximumParameters || nested.slots.size() > limits.maximumSlots)
                    return Invalid("Nested template identity, parent or customization budget is invalid.");
                if (!instances.insert(nested.id).second)
                    return Invalid("Nested template identity, parent or customization budget is invalid.");
            }
            return Result<void>::Success();
        }

        /** @brief Rejects malformed public interface identities and unbounded target/slot declarations. */
        Result<void> ValidateInterface(const UiTemplateDocument &document, const UiTemplateLimits &limits) {
            if (document.parameters.size() > limits.maximumParameters || document.slots.size() > limits.maximumSlots ||
                document.nested.size() > limits.maximumInstances)
                return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
            if (const auto parameters = ValidateParameters(document, limits); parameters.HasError())
                return parameters;
            if (const auto slots = ValidateSlots(document, limits); slots.HasError())
                return slots;
            return ValidateNestedInstances(document, limits);
        }

        /** @brief Borrows the exact pinned nested destination for this synchronous target validation. */
        Result<const UiTemplateDocument *> ResolveForwardDocument(const UiTemplateNestedParameterTarget &item,
                                                                  const UiTemplateDocument &document,
                                                                  std::span<const UiTemplateDocument> documents) {
            const UiTemplateDocument *current = &document;
            for (const auto instance : item.lineage) {
                const auto child = std::ranges::find(current->nested, instance, &UiTemplateNestedInstance::id);
                if (child == current->nested.end())
                    return Result<const UiTemplateDocument *>::Failure(
                        Invalid("Template forwarding names an absent nested instance.").ErrorValue());
                const auto next = std::ranges::find(documents, child->accepted.asset, [](const UiTemplateDocument &candidate) {
                    return candidate.dependency.asset;
                });
                if (next == documents.end() || next->dependency.revision != child->accepted.revision)
                    return Result<const UiTemplateDocument *>::Failure(
                        Invalid("Template forwarding has no exact pinned target document.").ErrorValue());
                current = std::to_address(next);
            }
            return Result<const UiTemplateDocument *>::Success(current);
        }

        /** @brief Resolves an exact nested forwarding lineage and rejects type mismatch or competing writes. */
        Result<void> ValidateForwardTarget(const UiTemplateNestedParameterTarget &item, const UiTemplateParameter &parameter,
                                           const UiTemplateDocument &document, const std::span<const UiTemplateDocument> documents,
                                           std::vector<const UiTemplateNestedParameterTarget *> &forwarded) {
            if (!item.parameter.IsValid() || item.lineage.empty() || item.lineage.size() > 32)
                return Invalid("Template forwarding target has no bounded stable lineage.");
            auto resolved = ResolveForwardDocument(item, document, documents);
            if (resolved.HasError())
                return Result<void>::Failure(resolved.ErrorValue());
            const auto *current = resolved.Value();
            if (const auto destination = std::ranges::find(current->parameters, item.parameter, &UiTemplateParameter::id);
                destination == current->parameters.end() || destination->kind != parameter.kind)
                return Invalid("Template forwarding has no exact type-compatible declared parameter.");
            if (std::ranges::any_of(forwarded, [&](const auto *previous) {
                return previous->lineage == item.lineage && previous->parameter == item.parameter;
            }))
                return Invalid("Template forwarding targets are duplicated.");
            forwarded.push_back(&item);
            return Result<void>::Success();
        }

        /** @brief Requires the explicit target alternative to agree with its declared typed parameter. */
        template <typename Target> Result<void> ValidateTargetCategory(const Target &item, const UiTemplateValueKind kind) {
            using enum UiTemplateValueKind;
            if constexpr (std::is_same_v<Target, UiTemplateContentTarget>) {
                if (kind > Reference)
                    return Invalid("An ordinary template target cannot address a rich attachment.");
            } else if constexpr (std::is_same_v<Target, UiTemplateStyleTarget>) {
                if (kind != Style)
                    return Invalid("A style target requires a typed style parameter.");
            } else if (item.kind != kind || (item.kind != LocalizedText && item.kind != Binding))
                return Invalid("A template attachment target requires its exact declared attachment category.");
            return Result<void>::Success();
        }

        /** @brief Admits one exact element/property write under the registered typed target contract. */
        template <typename Target>
        Result<void> ValidatePropertyTarget(const Target &item, const UiTemplateParameter &parameter, const UiTemplateDocument &document,
                                            const UiTemplateSchemaSnapshot &schemas,
                                            std::set<std::pair<UiTemplateLocalElementId, Runtime::Ui::UiStylePropertyId>> &written) {
            const auto element = std::ranges::find(document.elements, item.element, &UiTemplateElement::id);
            if (element == document.elements.end() || !written.emplace(item.element, item.property).second)
                return Invalid("Template parameter property target is absent or duplicated.");
            const auto *schema = PropertySchema(schemas, element->type, item.property);
            if (!schema || schema->kind != parameter.kind)
                return Invalid("Template parameter has no exact registered target property category.");
            if (const auto category = ValidateTargetCategory(item, parameter.kind); category.HasError())
                return category;
            return parameter.defaultValue ? ValidateValue(*parameter.defaultValue, *schema, schemas) : Result<void>::Success();
        }

        /** @brief Coordinates all declared writes, including optional parameters without a supplied value. */
        Result<void> ValidateTargets(const UiTemplateDocument &document, const std::span<const UiTemplateDocument> documents,
                                     const UiTemplateSchemaSnapshot &schemas) {
            std::set<std::pair<UiTemplateLocalElementId, Runtime::Ui::UiStylePropertyId>> written;
            std::vector<const UiTemplateNestedParameterTarget *> forwarded;
            for (const auto &parameter : document.parameters)
                for (const auto &target : parameter.targets) {
                    if (target.valueless_by_exception())
                        return Invalid("Template target has no category.");
                    const auto valid = std::visit([&]<typename Target>(const Target &item) -> Result<void> {
                        if constexpr (std::is_same_v<Target, UiTemplateNestedParameterTarget>)
                            return ValidateForwardTarget(item, parameter, document, documents, forwarded);
                        else
                            return ValidatePropertyTarget(item, parameter, document, schemas, written);
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

        /** @brief Validates each document before deriving graph edges from its single nested authority. */
        Result<void> ValidateDocument(const UiTemplateDocument &document, std::span<const UiTemplateDocument> documents,
                                      const UiTemplateSchemaSnapshot &schemas, const UiTemplateLimits &limits) {
            if (const auto valid = ValidateElements(document, schemas, limits); valid.HasError())
                return valid;
            if (const auto valid = ValidateInterface(document, limits); valid.HasError())
                return valid;
            if (const auto valid = ValidateTargets(document, documents, schemas); valid.HasError())
                return valid;
            return Result<void>::Success();
        }

        /** @brief Derives immutable graph metadata from the complete validated authored document. */
        UiTemplateDependencyDescriptor DependencyDescriptor(const UiTemplateDocument &document) {
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
            return dependency;
        }

        /** @brief Resolves every root against the complete package-qualified graph before adopting source ownership. */
        Result<void> ValidateCatalogRoots(std::span<const UiTemplateDocument> documents, const UiTemplateDependencyGraph &graph) {
            for (const auto &document : documents) {
                const UiTemplateReference root{document.dependency.asset, document.dependency.revision,
                                               document.dependency.interfaceVersion};
                if (const auto resolved = graph.Resolve(root); resolved.HasError())
                    return Result<void>::Failure(resolved.ErrorValue());
            }
            return Result<void>::Success();
        }

        /** @brief Checks complete catalog cardinality before allocating or traversing authored source. */
        bool ValidCatalogCardinality(std::span<const UiTemplateDocument> documents, const UiTemplateLimits &limits) {
            return limits.IsValid() && !documents.empty() && documents.size() <= limits.maximumInstances;
        }

        /** @brief Validates cumulative source capacity before registered schema admission, preserving failure precedence. */
        Result<void> ValidateCatalogInput(std::span<const UiTemplateDocument> documents, const UiTemplateSchemaSnapshot &schemas,
                                          const UiTemplateLimits &limits) {
            if (const auto valid = Detail::ValidateTemplateSourceBudget(documents, limits); valid.HasError())
                return valid;
            return ValidateSchemas(schemas, limits);
        }
    }  // namespace

    /** @copydoc UiTemplateCatalog::Create */
    Result<UiTemplateCatalog> UiTemplateCatalog::Create(std::vector<UiTemplateDocument> documents, UiTemplateSchemaSnapshot schemas,
                                                        const std::span<const Packages::LockedPackageReference> packages,
                                                        const UiTemplateLimits &limits) {
        if (!ValidCatalogCardinality(documents, limits))
            return Result<UiTemplateCatalog>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        try {
            if (const auto valid = ValidateCatalogInput(documents, schemas, limits); valid.HasError())
                return Result<UiTemplateCatalog>::Failure(valid.ErrorValue());
            std::vector<UiTemplateDependencyDescriptor> dependencies;
            dependencies.reserve(documents.size());
            for (const auto &document : documents) {
                if (const auto valid = ValidateDocument(document, documents, schemas, limits); valid.HasError())
                    return Result<UiTemplateCatalog>::Failure(valid.ErrorValue());
                dependencies.push_back(DependencyDescriptor(document));
            }
            auto graph = UiTemplateDependencyGraph::Create(dependencies, packages);
            if (graph.HasError())
                return Result<UiTemplateCatalog>::Failure(graph.ErrorValue());
            if (const auto valid = ValidateCatalogRoots(documents, graph.Value()); valid.HasError())
                return Result<UiTemplateCatalog>::Failure(valid.ErrorValue());
            return Result<UiTemplateCatalog>::Success(
                UiTemplateCatalog{std::move(documents), std::move(schemas), std::move(graph).Value(), limits});
        } catch (const std::bad_alloc &) {
            return Result<UiTemplateCatalog>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }

    /** @copydoc UiTemplateCatalog::UiTemplateCatalog */
    UiTemplateCatalog::UiTemplateCatalog(std::vector<UiTemplateDocument> documents, UiTemplateSchemaSnapshot schemas,
                                         UiTemplateDependencyGraph graph, const UiTemplateLimits &limits) noexcept
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
        if (const auto version = found->dependency.interfaceVersion;
            version.major != reference.minimumInterface.major || version.minor < reference.minimumInterface.minor)
            return Result<const UiTemplateDocument *>::Failure(MakeError(UiErrors::TemplateVersionIncompatible));
        // Create already validated every complete graph/lock closure. Exact lookup needs no new traversal allocation.
        return Result<const UiTemplateDocument *>::Success(std::to_address(found));
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
