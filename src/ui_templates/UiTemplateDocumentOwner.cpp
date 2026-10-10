#include "Horo/UiTemplates/UiTemplateDocumentOwner.h"

#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateBudget.h"
#include "UiTemplateCodec.h"
#include "UiTemplateParameterValidation.h"

#include <algorithm>
#include <map>
#include <new>
#include <set>
#include <tuple>

namespace Horo::UiTemplates {
    namespace UiErrors {
        namespace {
            const ErrorDomainId DocumentDomain{"horo.runtime_ui"};
        }

        /** @copydoc TemplateDocumentShutdown */
        const ErrorCodeDescriptor TemplateDocumentShutdown{DocumentDomain,
                                                           ErrorCode{"runtime_ui.template.document_shutdown"},
                                                           ErrorSeverity::Error,
                                                           "Template document authoring admission is closed.",
                                                           "Prepare through a live document owner.",
                                                           false,
                                                           true};
        /** @copydoc TemplateRebaseInvalid */
        const ErrorCodeDescriptor TemplateRebaseInvalid{DocumentDomain,
                                                        ErrorCode{"runtime_ui.template.rebase_invalid"},
                                                        ErrorSeverity::Error,
                                                        "Template rebase requires an explicit data-preserving migration.",
                                                        "Review the rejected parameter or populated slot.",
                                                        false,
                                                        true};
        /** @copydoc TemplateDocumentOwnerMismatch */
        const ErrorCodeDescriptor TemplateDocumentOwnerMismatch{DocumentDomain,
                                                                ErrorCode{"runtime_ui.template.document_owner_mismatch"},
                                                                ErrorSeverity::Error,
                                                                "The prepared transaction belongs to another document owner.",
                                                                "Prepare again against the intended owner.",
                                                                false,
                                                                true};
        /** @copydoc TemplateDocumentCancelled */
        const ErrorCodeDescriptor TemplateDocumentCancelled{DocumentDomain,
                                                            ErrorCode{"runtime_ui.template.document_cancelled"},
                                                            ErrorSeverity::Error,
                                                            "Template document preparation or publication was cancelled.",
                                                            "Prepare a new explicitly admitted transaction.",
                                                            false,
                                                            true};
        /** @copydoc TemplateUndoEmpty */
        const ErrorCodeDescriptor TemplateUndoEmpty{DocumentDomain,
                                                    ErrorCode{"runtime_ui.template.undo_empty"},
                                                    ErrorSeverity::Error,
                                                    "No prior template document is retained for undo.",
                                                    "Continue from the current accepted document.",
                                                    false,
                                                    true};
        /** @copydoc TemplatePersistenceMalformed */
        const ErrorCodeDescriptor TemplatePersistenceMalformed{DocumentDomain,
                                                               ErrorCode{"runtime_ui.template.persistence_malformed"},
                                                               ErrorSeverity::Error,
                                                               "Linked UI source is malformed or unsupported.",
                                                               "Review source schema and typed identities without dropping authored data.",
                                                               false,
                                                               true};
    }  // namespace UiErrors

    namespace Detail {
        /** @brief One immutable publication; catalogs and derived projections share the source's exact lifetime. */
        struct UiTemplateOwnerState final {
            UiTemplateAuthoredDocument source;
            std::vector<std::shared_ptr<const UiTemplateCatalog>> catalogs;
            std::vector<UiTemplateExpansion> expansions;
            UiTemplateLimits limits;
            std::shared_ptr<const UiTemplateCatalog> authoringSchemas;
        };
    }  // namespace Detail

    namespace {
        using namespace Runtime::Ui;
        using State = Detail::UiTemplateOwnerState;

        /** @brief Constructs a borrowed prospective policy view, including every durable rich attachment. */
        Detail::TemplateDocumentBudgetInput BudgetView(const UiTemplateAuthoredDocument &source, const UiTemplateLimits &limits,
                                                       const std::span<const std::shared_ptr<const UiTemplateCatalog>> catalogs = {},
                                                       const UiTemplateCatalog *fallback = nullptr,
                                                       const std::span<const UiTemplateCatalogLease> leases = {}) {
            Detail::TemplateDocumentBudgetInput input{source.ordinary, source.instances, limits, catalogs};
            input.fallbackCatalog = fallback;
            input.styles = source.styles;
            input.localizedTexts = source.localizedTexts;
            input.bindings = source.bindings;
            input.leases = leases;
            return input;
        }

        /** @brief Canonicalizes bounded paired records/leases and rich attachment keys without allocating another authority. */
        void Canonicalize(UiTemplateAuthoredDocument &source, std::vector<std::shared_ptr<const UiTemplateCatalog>> &catalogs) {
            for (std::size_t i = 1; i < source.instances.size(); ++i)
                for (std::size_t j = i; j > 0 && source.instances[j].id < source.instances[j - 1].id; --j) {
                    std::swap(source.instances[j], source.instances[j - 1]);
                    std::swap(catalogs[j], catalogs[j - 1]);
                }
            std::ranges::sort(source.styles, [](const auto &left, const auto &right) {
                return std::tie(left.element, left.assignment.property) < std::tie(right.element, right.assignment.property);
            });
            std::ranges::sort(source.localizedTexts, [](const auto &left, const auto &right) {
                return std::tie(left.element, left.property) < std::tie(right.element, right.property);
            });
            std::ranges::sort(source.bindings, {}, &UiBindingDescriptor::id);
        }

        /** @brief Validates exact element/property registration and literal/token representation for durable styles. */
        Result<void> ValidateStyle(const UiTemplateExpandedStyle &style, const UiDocument &ordinary, const UiTemplateCatalog &catalog) {
            const auto found = std::ranges::find(ordinary.Elements(), style.element, &UiDocumentElement::id);
            if (found == ordinary.Elements().end() || !style.assignment.property.IsValid() || !style.assignment.value.IsValid())
                return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
            const auto schema = std::ranges::find_if(catalog.Schemas().properties, [&](const auto &property) {
                return property.elementType == found->type && property.property == style.assignment.property && property.style;
            });
            if (schema == catalog.Schemas().properties.end())
                return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
            return style.assignment.value.referencesToken
                       ? Result<void>::Success()
                       : catalog.ValidateProperty(found->type, style.assignment.property, UiTemplateValue{style.assignment.value.literal});
        }

        /** @brief Validates an exact durable localization attachment without consulting an ambient localization service. */
        Result<void> ValidateText(const UiTemplateExpandedText &text, const UiDocument &ordinary, const UiTemplateCatalog &catalog) {
            const auto found = std::ranges::find(ordinary.Elements(), text.element, &UiDocumentElement::id);
            if (found == ordinary.Elements().end())
                return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
            return catalog.ValidateProperty(found->type, text.property, UiTemplateValue{text.text});
        }

        /** @brief Validates durable binding metadata against an immutable provider schema; no provider is called. */
        Result<void> ValidateBinding(const UiBindingDescriptor &binding, const UiDocument &ordinary, const UiTemplateCatalog &catalog) {
            if (std::ranges::find(ordinary.Elements(), binding.target.element, &UiDocumentElement::id) == ordinary.Elements().end())
                return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
            const auto schema =
                std::ranges::find(catalog.Schemas().bindingProviders, binding.source.providerType, &UiBindingProviderSchema::Type);
            if (schema == catalog.Schemas().bindingProviders.end())
                return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
            return ValidateUiBindingDescriptor(binding, *schema);
        }

        /** @brief Checks canonical rich keys for conflicts before validating their own typed responsibilities. */
        Result<void> ValidateAttachments(const UiTemplateAuthoredDocument &source, const UiTemplateCatalog &catalog) {
            for (std::size_t i = 0; i < source.styles.size(); ++i) {
                const auto &style = source.styles[i];
                if (i && source.styles[i - 1].element == style.element &&
                    source.styles[i - 1].assignment.property == style.assignment.property)
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
                if (const auto valid = ValidateStyle(style, source.ordinary, catalog); valid.HasError())
                    return valid;
            }
            for (std::size_t i = 0; i < source.localizedTexts.size(); ++i) {
                const auto &text = source.localizedTexts[i];
                if (i && source.localizedTexts[i - 1].element == text.element && source.localizedTexts[i - 1].property == text.property)
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
                if (const auto valid = ValidateText(text, source.ordinary, catalog); valid.HasError())
                    return valid;
            }
            for (std::size_t i = 0; i < source.bindings.size(); ++i) {
                const auto &binding = source.bindings[i];
                if (i && source.bindings[i - 1].id == binding.id)
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
                if (const auto valid = ValidateBinding(binding, source.ordinary, catalog); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }

        /** @brief Publishes typed asset requirements carried by ordinary expanded references into the candidate builder. */
        Result<void> RequireReference(UiDocumentBuilder &builder, const UiReference &reference) {
            if (reference.kind != UiReferenceKind::Asset)
                return Result<void>::Success();
            return builder.RequireAsset({reference.asset, reference.expectedAssetType, true});
        }

        /** @brief Preserves one expanded element and its complete explicit/typed-property asset reference evidence. */
        Result<void> AppendExpandedElement(UiDocumentBuilder &builder, const UiDocumentElement &element) {
            for (const auto &reference : element.references)
                if (const auto result = RequireReference(builder, reference); result.HasError())
                    return result;
            for (const auto &property : element.properties)
                if (const auto *reference = std::get_if<UiReference>(&property.value))
                    if (const auto result = RequireReference(builder, *reference); result.HasError())
                        return result;
            return builder.AddElement(element);
        }

        /** @brief Composes an admitted projection's ordinary hierarchy and localized message authority. */
        Result<void> AppendExpansion(UiDocumentBuilder &builder, const UiTemplateExpansion &expansion) {
            for (const auto &element : expansion.elements)
                if (const auto result = AppendExpandedElement(builder, element); result.HasError())
                    return result;
            for (const auto &text : expansion.localizedTexts)
                if (const auto result = builder.AddLocalizedText(text.text); result.HasError())
                    return result;
            return Result<void>::Success();
        }

        /** @brief Copies ordinary authority through its existing validator under a new monotonic revision. */
        Result<UiDocument> RevisionCopy(const UiDocument &source, const UiDocumentRevision revision,
                                        const std::span<const UiTemplateExpansion> expansions = {}) {
            UiDocumentBuilder builder{source.Id(), revision, source.SchemaVersion()};
            for (const auto &canvas : source.Canvases())
                if (const auto result = builder.AddCanvas(canvas); result.HasError())
                    return Result<UiDocument>::Failure(result.ErrorValue());
            for (const auto &element : source.Elements())
                if (const auto result = builder.AddElement(element); result.HasError())
                    return Result<UiDocument>::Failure(result.ErrorValue());
            for (const auto &text : source.LocalizedTexts())
                if (const auto result = builder.AddLocalizedText(text); result.HasError())
                    return Result<UiDocument>::Failure(result.ErrorValue());
            for (const auto &asset : source.LocalizedAssets())
                if (const auto result = builder.AddLocalizedAsset(asset); result.HasError())
                    return Result<UiDocument>::Failure(result.ErrorValue());
            for (const auto &dependency : source.Dependencies())
                if (const auto result = builder.RequireAsset(dependency); result.HasError())
                    return Result<UiDocument>::Failure(result.ErrorValue());
            for (const auto &route : source.Routes())
                if (const auto result = builder.AddRoute(route); result.HasError())
                    return Result<UiDocument>::Failure(result.ErrorValue());
            for (const auto &expansion : expansions)
                if (const auto result = AppendExpansion(builder, expansion); result.HasError())
                    return Result<UiDocument>::Failure(result.ErrorValue());
            return std::move(builder).Build();
        }

        /** @brief Admits complete owned expansions without publishing or replacing durable authority. */
        Result<std::shared_ptr<const State>> PrepareState(UiTemplateAuthoredDocument source,
                                                          std::vector<std::shared_ptr<const UiTemplateCatalog>> catalogs,
                                                          const UiTemplateLimits &limits, const CancellationToken &cancellation,
                                                          std::shared_ptr<const UiTemplateCatalog> authoringSchemas) {
            if (const auto valid = Detail::ValidateTemplateDocumentBudget(BudgetView(source, limits, catalogs), cancellation);
                valid.HasError())
                return Result<std::shared_ptr<const State>>::Failure(valid.ErrorValue());
            if (!authoringSchemas)
                return Result<std::shared_ptr<const State>>::Failure(MakeError(UiErrors::TemplateMissing));
            Canonicalize(source, catalogs);
            if (const auto valid = ValidateAttachments(source, *authoringSchemas); valid.HasError())
                return Result<std::shared_ptr<const State>>::Failure(valid.ErrorValue());
            std::vector<UiElementId> occupied;
            occupied.reserve(limits.maximumElements);
            for (const auto &element : source.ordinary.Elements())
                occupied.push_back(element.id);
            std::set<UiTemplateInstanceId> ids;
            std::set<UiBindingId> bindingIds;
            for (const auto &binding : source.bindings)
                bindingIds.insert(binding.id);
            std::vector<UiTemplateExpansion> expansions;
            expansions.reserve(source.instances.size());
            for (std::size_t index = 0; index < source.instances.size(); ++index) {
                const auto &instance = source.instances[index];
                if (!catalogs[index] || !instance.id.IsValid() || !ids.insert(instance.id).second)
                    return Result<std::shared_ptr<const State>>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
                if (cancellation.IsCancellationRequested())
                    return Result<std::shared_ptr<const State>>::Failure(MakeError(UiErrors::TemplateDocumentCancelled));
                auto expansion = ExpandTemplate(*catalogs[index], source.ordinary.Id(), instance, occupied, cancellation);
                if (expansion.HasError())
                    return Result<std::shared_ptr<const State>>::Failure(expansion.ErrorValue());
                if (occupied.size() > limits.maximumElements ||
                    expansion.Value().elements.size() > limits.maximumElements - occupied.size())
                    return Result<std::shared_ptr<const State>>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
                for (const auto &element : expansion.Value().elements)
                    occupied.push_back(element.id);
                if (expansion.Value().bindings.size() > MaximumUiBindingDescriptors - bindingIds.size())
                    return Result<std::shared_ptr<const State>>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
                for (const auto &binding : expansion.Value().bindings)
                    if (!bindingIds.insert(binding.id).second)
                        return Result<std::shared_ptr<const State>>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
                expansions.push_back(std::move(expansion.Value()));
            }
            return Result<std::shared_ptr<const State>>::Success(std::make_shared<const State>(
                State{std::move(source), std::move(catalogs), std::move(expansions), limits, std::move(authoringSchemas)}));
        }

        /** @brief Retains exact authored values; missing declarations never implicitly drop populated customization. */
        Result<void> Reconcile(const UiTemplateInstanceRecord &record, const UiTemplateDocument &target) {
            for (const auto &argument : record.arguments) {
                const auto parameter = std::ranges::find(target.parameters, argument.parameter, &UiTemplateParameter::id);
                if (parameter == target.parameters.end())
                    return Result<void>::Failure(MakeError(UiErrors::TemplateRebaseInvalid, "A supplied parameter was removed."));
                if (const auto valid = Detail::ValidateParameterValue(*parameter, argument.value); valid.HasError())
                    return valid;
            }
            for (const auto &content : record.slots)
                if (std::ranges::find(target.slots, content.slot, &UiTemplateSlot::id) == target.slots.end())
                    return Result<void>::Failure(MakeError(UiErrors::TemplateRebaseInvalid, "An authored slot needs explicit removal."));
            return Result<void>::Success();
        }

        /** @brief Requires complete fresh host allocation before copying any candidate expansion payload. */
        Result<void> ValidateDetachIdentities(const State &state, const UiTemplateExpansion &expansion,
                                              const std::span<const UiTemplateDetachIdentity> identities) {
            if (identities.size() != expansion.templateOwnedElements.size())
                return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
            for (std::size_t i = 0; i < identities.size(); ++i) {
                const auto &identity = identities[i];
                if (!identity.independent.IsValid() ||
                    std::ranges::find(expansion.templateOwnedElements, identity.expanded) == expansion.templateOwnedElements.end())
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
                for (std::size_t j = 0; j < i; ++j)
                    if (identities[j].expanded == identity.expanded || identities[j].independent == identity.independent)
                        return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
                if (std::ranges::find(state.source.ordinary.Elements(), identity.independent, &UiDocumentElement::id) !=
                    state.source.ordinary.Elements().end())
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
                for (const auto &projection : state.expansions)
                    if (std::ranges::find(projection.elements, identity.independent, &UiDocumentElement::id) != projection.elements.end())
                        return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
            }
            return Result<void>::Success();
        }

        /** @brief Rewrites only element-reference alternatives through the complete typed identity allocation. */
        void RemapReference(UiReference &reference, const std::map<UiElementId, UiElementId> &identities) {
            if (reference.kind == UiReferenceKind::Element)
                if (const auto found = identities.find(reference.element); found != identities.end())
                    reference.element = found->second;
        }

        /** @brief Materializes resolved rich values and recursive ordinary content with no linked authority. */
        Result<UiTemplateExpansion> Materialize(const UiTemplateExpansion &accepted,
                                                const std::span<const UiTemplateDetachIdentity> allocation) {
            std::map<UiElementId, UiElementId> identities;
            for (const auto &identity : allocation)
                identities.emplace(identity.expanded, identity.independent);
            const auto remap = [&](const UiElementId id) {
                const auto found = identities.find(id);
                return found == identities.end() ? id : found->second;
            };
            auto output = accepted;
            output.root = remap(output.root);
            for (auto &element : output.elements) {
                element.id = remap(element.id);
                element.parent = remap(element.parent);
                for (auto &reference : element.references)
                    RemapReference(reference, identities);
                for (auto &property : element.properties)
                    if (auto *reference = std::get_if<UiReference>(&property.value))
                        RemapReference(*reference, identities);
            }
            for (auto &style : output.styles)
                style.element = remap(style.element);
            for (auto &text : output.localizedTexts)
                text.element = remap(text.element);
            for (auto &binding : output.bindings) {
                binding.target.element = remap(binding.target.element);
                Sha256Builder hash;
                constexpr std::string_view domain{"horo.ui-template.detached-binding.v1"};
                if (!hash.Update(std::as_bytes(std::span{domain.data(), domain.size()})) ||
                    !hash.Update(std::as_bytes(std::span{binding.id.Bytes()})) ||
                    !hash.Update(std::as_bytes(std::span{binding.target.element.Bytes()})))
                    return Result<UiTemplateExpansion>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
                SerializedUiId bytes{};
                const auto digest = hash.Finalize();
                std::copy_n(digest.bytes.begin(), bytes.size(), bytes.begin());
                auto id = UiBindingId::Create(bytes);
                if (id.HasError())
                    return Result<UiTemplateExpansion>::Failure(id.ErrorValue());
                binding.id = id.Value();
            }
            output.acceptedDependencies.clear();
            output.templateOwnedElements.clear();
            return Result<UiTemplateExpansion>::Success(std::move(output));
        }
    }  // namespace

    UiTemplateDocumentSnapshot::UiTemplateDocumentSnapshot(std::shared_ptr<const State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc UiTemplateDocumentSnapshot::Source */
    const UiTemplateAuthoredDocument &UiTemplateDocumentSnapshot::Source() const noexcept {
        return state_->source;
    }

    /** @copydoc UiTemplateDocumentSnapshot::Expansions */
    std::span<const UiTemplateExpansion> UiTemplateDocumentSnapshot::Expansions() const noexcept {
        return state_->expansions;
    }

    /** @copydoc UiTemplateDocumentSnapshot::Limits */
    const UiTemplateLimits &UiTemplateDocumentSnapshot::Limits() const noexcept {
        return state_->limits;
    }

    /** @copydoc UiTemplateDocumentSnapshot::Flatten */
    Result<UiTemplateFlattenedDocument> UiTemplateDocumentSnapshot::Flatten(const CancellationToken &cancellation) const {
        try {
            if (cancellation.IsCancellationRequested())
                return Result<UiTemplateFlattenedDocument>::Failure(MakeError(UiErrors::TemplateDocumentCancelled));
            auto ordinary = RevisionCopy(state_->source.ordinary, state_->source.ordinary.Revision(), state_->expansions);
            if (ordinary.HasError())
                return Result<UiTemplateFlattenedDocument>::Failure(ordinary.ErrorValue());
            UiTemplateFlattenedDocument output{std::move(ordinary).Value(),
                                               state_->source.styles,
                                               state_->source.localizedTexts,
                                               state_->source.bindings,
                                               {}};
            for (const auto &expansion : state_->expansions) {
                if (cancellation.IsCancellationRequested())
                    return Result<UiTemplateFlattenedDocument>::Failure(MakeError(UiErrors::TemplateDocumentCancelled));
                output.styles.insert(output.styles.end(), expansion.styles.begin(), expansion.styles.end());
                output.localizedTexts.insert(output.localizedTexts.end(), expansion.localizedTexts.begin(), expansion.localizedTexts.end());
                output.bindings.insert(output.bindings.end(), expansion.bindings.begin(), expansion.bindings.end());
                output.acceptedDependencies.insert(output.acceptedDependencies.end(), expansion.acceptedDependencies.begin(),
                                                   expansion.acceptedDependencies.end());
            }
            return Result<UiTemplateFlattenedDocument>::Success(std::move(output));
        } catch (const std::bad_alloc &) {
            return Result<UiTemplateFlattenedDocument>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }

    /** @copydoc UiTemplateDocumentSnapshot::Cook */
    Result<UiTemplateCookedDocument> UiTemplateDocumentSnapshot::Cook(const UiTemplateCookContext &context,
                                                                      const CancellationToken &cancellation) const {
        using namespace Detail::Codec;
        using Output = Result<UiTemplateCookedDocument>;
        try {
            if (std::ranges::all_of(context.verifiedDependencyManifest.bytes, [](const auto byte) {
                return byte == 0;
            }))
                return Output::Failure(MakeError(UiErrors::TemplateGraphInvalid));
            auto flattened = Flatten(cancellation);
            if (flattened.HasError())
                return Output::Failure(flattened.ErrorValue());
            auto value = std::move(flattened).Value();
            Runtime::Ui::UiDocumentCookLimits limits;
            limits.maximumElements = state_->limits.maximumElements;
            limits.maximumPayloadBytes = state_->limits.maximumPayloadBytes;
            auto ordinary = Runtime::Ui::CookedUiDocument::Cook(value.ordinary, limits);
            if (ordinary.HasError())
                return Output::Failure(ordinary.ErrorValue());
            auto durable = SerializeTemplateDocument(*this);
            if (durable.HasError())
                return Output::Failure(durable.ErrorValue());
            UiTemplateAuthoredDocument rich{std::move(value.ordinary),
                                            {},
                                            std::move(value.styles),
                                            std::move(value.localizedTexts),
                                            std::move(value.bindings)};
            auto payload = WriteRich(rich).dump();
            Json schemas = Json::array({WriteSchemas(state_->authoringSchemas->Schemas())});
            for (const auto &catalog : state_->catalogs)
                schemas.push_back(WriteSchemas(catalog->Schemas()));
            const auto schemaBytes = schemas.dump();
            if (ordinary.Value().Payload().size() > state_->limits.maximumPayloadBytes ||
                payload.size() > state_->limits.maximumPayloadBytes - ordinary.Value().Payload().size() ||
                schemaBytes.size() > state_->limits.maximumPayloadBytes)
                return Output::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
            const auto digest = [](const std::string_view bytes) {
                return ComputeSha256(std::as_bytes(std::span{bytes.data(), bytes.size()}));
            };
            const Json identity{{"algorithm", 1},
                                {"source", Hex(digest(durable.Value()).bytes)},
                                {"schemas", Hex(digest(schemaBytes).bytes)},
                                {"dependencies", Hex(context.verifiedDependencyManifest.bytes)},
                                {"ordinary", Hex(ComputeSha256(std::as_bytes(ordinary.Value().Payload())).bytes)},
                                {"rich", Hex(digest(payload).bytes)}};
            const auto identityBytes = identity.dump();
            if (cancellation.IsCancellationRequested())
                return Output::Failure(MakeError(UiErrors::TemplateDocumentCancelled));
            return Output::Success({std::move(ordinary).Value(), std::move(rich.styles), std::move(rich.localizedTexts),
                                    std::move(rich.bindings), std::move(payload), digest(identityBytes),
                                    context.verifiedDependencyManifest});
        } catch (const Failure &failure) {
            return Output::Failure(failure.error);
        } catch (const Json::exception &) {
            return Output::Failure(MakeError(UiErrors::TemplatePersistenceMalformed));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }

    UiTemplatePreparedTransaction::UiTemplatePreparedTransaction(std::shared_ptr<const State> base, std::shared_ptr<const State> next,
                                                                 std::weak_ptr<const std::uint8_t> owner,
                                                                 CancellationToken cancellation) noexcept
        : base_(std::move(base)), next_(std::move(next)), owner_(std::move(owner)), cancellation_(std::move(cancellation)) {}

    /** @copydoc UiTemplatePreparedTransaction::Preview */
    UiTemplateDocumentSnapshot UiTemplatePreparedTransaction::Preview() const noexcept {
        return UiTemplateDocumentSnapshot{next_};
    }

    UiTemplateDocumentOwner::UiTemplateDocumentOwner(std::shared_ptr<const State> state, std::shared_ptr<const std::uint8_t> identity,
                                                     const std::size_t undoLimit)
        : state_(std::move(state)), identity_(std::move(identity)), undoLimit_(undoLimit) {
        undo_.reserve(undoLimit_);
    }

    /** @copydoc UiTemplateDocumentOwner::Create */
    Result<std::unique_ptr<UiTemplateDocumentOwner>> UiTemplateDocumentOwner::Create(UiTemplateAuthoredDocument source,
                                                                                     std::shared_ptr<const UiTemplateCatalog> catalog) {
        try {
            if (!catalog)
                return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Failure(MakeError(UiErrors::TemplateMissing));
            const auto limits = catalog->Limits();
            if (const auto valid = Detail::ValidateTemplateDocumentBudget(BudgetView(source, limits, {}, catalog.get()), {});
                valid.HasError())
                return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Failure(valid.ErrorValue());
            std::ranges::sort(source.instances, {}, &UiTemplateInstanceRecord::id);
            const auto undoLimit = limits.maximumUndoEntries;
            std::vector<std::shared_ptr<const UiTemplateCatalog>> catalogs(source.instances.size(), catalog);
            auto state = PrepareState(std::move(source), std::move(catalogs), limits, {}, catalog);
            if (state.HasError())
                return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Failure(state.ErrorValue());
            return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Success(std::unique_ptr<UiTemplateDocumentOwner>{
                new UiTemplateDocumentOwner{std::move(state.Value()), std::make_shared<const std::uint8_t>(0), undoLimit}});
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }

    /** @copydoc UiTemplateDocumentOwner::Create */
    Result<std::unique_ptr<UiTemplateDocumentOwner>> UiTemplateDocumentOwner::Create(
        UiTemplateAuthoredDocument source, const std::span<const UiTemplateCatalogLease> leases,
        std::shared_ptr<const UiTemplateCatalog> authoringSchemas, const UiTemplateLimits &limits) {
        try {
            if (!authoringSchemas || leases.size() > 256)
                return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
            for (std::size_t i = 0; i < leases.size(); ++i) {
                if (!leases[i].catalog)
                    return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Failure(MakeError(UiErrors::TemplateMissing));
                for (std::size_t j = 0; j < i; ++j)
                    if (leases[i].reference.asset == leases[j].reference.asset &&
                        leases[i].reference.revision == leases[j].reference.revision)
                        return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
            }
            if (const auto valid = Detail::ValidateTemplateDocumentBudget(BudgetView(source, limits, {}, nullptr, leases), {});
                valid.HasError())
                return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Failure(valid.ErrorValue());
            std::vector<std::shared_ptr<const UiTemplateCatalog>> catalogs;
            catalogs.reserve(source.instances.size());
            for (const auto &instance : source.instances) {
                const auto lease = std::ranges::find_if(leases, [&](const UiTemplateCatalogLease &candidate) {
                    return candidate.reference.asset == instance.accepted.asset &&
                           candidate.reference.revision == instance.accepted.revision;
                });
                if (lease == leases.end())
                    return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Failure(MakeError(UiErrors::TemplateRevisionUnavailable));
                catalogs.push_back(lease->catalog);
            }
            auto state = PrepareState(std::move(source), std::move(catalogs), limits, {}, std::move(authoringSchemas));
            if (state.HasError())
                return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Failure(state.ErrorValue());
            return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Success(std::unique_ptr<UiTemplateDocumentOwner>{
                new UiTemplateDocumentOwner{std::move(state).Value(), std::make_shared<const std::uint8_t>(0), limits.maximumUndoEntries}});
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }

    UiTemplateDocumentOwner::~UiTemplateDocumentOwner() {
        Shutdown();
    }

    /** @copydoc UiTemplateDocumentOwner::Capture */
    UiTemplateDocumentSnapshot UiTemplateDocumentOwner::Capture() const noexcept {
        return UiTemplateDocumentSnapshot{state_};
    }

    /** @copydoc UiTemplateDocumentOwner::UpdateAvailable */
    Result<bool> UiTemplateDocumentOwner::UpdateAvailable(const UiTemplateInstanceId instance, const UiTemplateReference &available) const {
        if (!identity_)
            return Result<bool>::Failure(MakeError(UiErrors::TemplateDocumentShutdown));
        const auto record = std::ranges::find(state_->source.instances, instance, &UiTemplateInstanceRecord::id);
        if (record == state_->source.instances.end() || available.asset != record->accepted.asset)
            return Result<bool>::Failure(MakeError(UiErrors::TemplateRebaseInvalid));
        return Result<bool>::Success(available.revision != record->accepted.revision);
    }

    /** @copydoc UiTemplateDocumentOwner::PrepareRebase */
    Result<UiTemplatePreparedTransaction> UiTemplateDocumentOwner::PrepareRebase(const UiTemplateInstanceId instance,
                                                                                 const UiTemplateReference &target,
                                                                                 std::shared_ptr<const UiTemplateCatalog> catalog,
                                                                                 const CancellationToken &cancellation) const {
        try {
            if (!identity_)
                return Result<UiTemplatePreparedTransaction>::Failure(MakeError(UiErrors::TemplateDocumentShutdown));
            if (cancellation.IsCancellationRequested())
                return Result<UiTemplatePreparedTransaction>::Failure(MakeError(UiErrors::TemplateDocumentCancelled));
            const auto found = std::ranges::find(state_->source.instances, instance, &UiTemplateInstanceRecord::id);
            if (!catalog || found == state_->source.instances.end() || target.asset != found->accepted.asset)
                return Result<UiTemplatePreparedTransaction>::Failure(MakeError(UiErrors::TemplateRebaseInvalid));
            const auto index = static_cast<std::size_t>(found - state_->source.instances.begin());
            const auto previous = state_->catalogs[index]->Resolve(found->accepted);
            if (previous.HasError())
                return Result<UiTemplatePreparedTransaction>::Failure(previous.ErrorValue());
            const auto selected = catalog->Resolve(target);
            if (selected.HasError())
                return Result<UiTemplatePreparedTransaction>::Failure(selected.ErrorValue());
            if (selected.Value()->dependency.interfaceVersion.major != previous.Value()->dependency.interfaceVersion.major)
                return Result<UiTemplatePreparedTransaction>::Failure(MakeError(UiErrors::TemplateVersionIncompatible));
            if (const auto valid = Reconcile(*found, *selected.Value()); valid.HasError())
                return Result<UiTemplatePreparedTransaction>::Failure(valid.ErrorValue());
            auto prospective = BudgetView(state_->source, state_->limits, state_->catalogs);
            prospective.replacementIndex = index;
            prospective.replacement = &target;
            prospective.replacementCatalog = catalog.get();
            if (const auto valid = Detail::ValidateTemplateDocumentBudget(prospective, cancellation); valid.HasError())
                return Result<UiTemplatePreparedTransaction>::Failure(valid.ErrorValue());
            const auto revision = state_->source.ordinary.Revision().Next();
            if (revision.HasError())
                return Result<UiTemplatePreparedTransaction>::Failure(revision.ErrorValue());
            auto ordinary = RevisionCopy(state_->source.ordinary, revision.Value());
            if (ordinary.HasError())
                return Result<UiTemplatePreparedTransaction>::Failure(ordinary.ErrorValue());
            UiTemplateAuthoredDocument source{std::move(ordinary).Value(), state_->source.instances, state_->source.styles,
                                              state_->source.localizedTexts, state_->source.bindings};
            auto catalogs = state_->catalogs;
            source.instances[index].accepted = target;
            catalogs[index] = std::move(catalog);
            auto next = PrepareState(std::move(source), std::move(catalogs), state_->limits, cancellation, state_->authoringSchemas);
            if (next.HasError())
                return Result<UiTemplatePreparedTransaction>::Failure(next.ErrorValue());
            return Result<UiTemplatePreparedTransaction>::Success(
                UiTemplatePreparedTransaction{state_, std::move(next.Value()), identity_, cancellation});
        } catch (const std::bad_alloc &) {
            return Result<UiTemplatePreparedTransaction>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }

    /** @copydoc UiTemplateDocumentOwner::PrepareInsert */
    Result<UiTemplatePreparedTransaction> UiTemplateDocumentOwner::PrepareInsert(UiTemplateInstanceRecord instance,
                                                                                 std::shared_ptr<const UiTemplateCatalog> catalog,
                                                                                 const CancellationToken &cancellation) const {
        using Output = Result<UiTemplatePreparedTransaction>;
        try {
            if (!identity_)
                return Output::Failure(MakeError(UiErrors::TemplateDocumentShutdown));
            if (cancellation.IsCancellationRequested())
                return Output::Failure(MakeError(UiErrors::TemplateDocumentCancelled));
            if (!catalog || !instance.id.IsValid() ||
                std::ranges::find(state_->source.instances, instance.id, &UiTemplateInstanceRecord::id) != state_->source.instances.end())
                return Output::Failure(MakeError(UiErrors::TemplateGraphInvalid));
            auto prospective = BudgetView(state_->source, state_->limits, state_->catalogs);
            prospective.insertion = &instance;
            prospective.insertionCatalog = catalog.get();
            if (const auto valid = Detail::ValidateTemplateDocumentBudget(prospective, cancellation); valid.HasError())
                return Output::Failure(valid.ErrorValue());
            auto revision = state_->source.ordinary.Revision().Next();
            if (revision.HasError())
                return Output::Failure(revision.ErrorValue());
            auto ordinary = RevisionCopy(state_->source.ordinary, revision.Value());
            if (ordinary.HasError())
                return Output::Failure(ordinary.ErrorValue());
            UiTemplateAuthoredDocument source{std::move(ordinary).Value(), state_->source.instances, state_->source.styles,
                                              state_->source.localizedTexts, state_->source.bindings};
            source.instances.push_back(std::move(instance));
            auto catalogs = state_->catalogs;
            catalogs.push_back(std::move(catalog));
            auto next = PrepareState(std::move(source), std::move(catalogs), state_->limits, cancellation, state_->authoringSchemas);
            if (next.HasError())
                return Output::Failure(next.ErrorValue());
            return Output::Success(UiTemplatePreparedTransaction{state_, std::move(next).Value(), identity_, cancellation});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }

    /** @copydoc UiTemplateDocumentOwner::PrepareInsertIndependent */
    Result<UiTemplatePreparedTransaction> UiTemplateDocumentOwner::PrepareInsertIndependent(
        UiTemplateInstanceRecord instance, std::shared_ptr<const UiTemplateCatalog> catalog,
        const std::span<const UiTemplateDetachIdentity> identities, const CancellationToken &cancellation) const {
        using Output = Result<UiTemplatePreparedTransaction>;
        try {
            const auto id = instance.id;
            auto inserted = PrepareInsert(std::move(instance), std::move(catalog), cancellation);
            if (inserted.HasError())
                return Output::Failure(inserted.ErrorValue());
            // The intermediate owner is private preparation only; neither authority nor undo history is published.
            UiTemplateDocumentOwner intermediate{inserted.Value().next_, std::make_shared<const std::uint8_t>(0), undoLimit_};
            auto detached = intermediate.PrepareDetach(id, identities, cancellation);
            if (detached.HasError())
                return Output::Failure(detached.ErrorValue());
            return Output::Success(UiTemplatePreparedTransaction{state_, detached.Value().next_, identity_, cancellation});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }

    /** @copydoc UiTemplateDocumentOwner::PrepareDetach */
    Result<UiTemplatePreparedTransaction> UiTemplateDocumentOwner::PrepareDetach(const UiTemplateInstanceId instance,
                                                                                 const std::span<const UiTemplateDetachIdentity> identities,
                                                                                 const CancellationToken &cancellation) const {
        using Output = Result<UiTemplatePreparedTransaction>;
        try {
            if (!identity_)
                return Output::Failure(MakeError(UiErrors::TemplateDocumentShutdown));
            if (cancellation.IsCancellationRequested())
                return Output::Failure(MakeError(UiErrors::TemplateDocumentCancelled));
            const auto found = std::ranges::find(state_->source.instances, instance, &UiTemplateInstanceRecord::id);
            if (found == state_->source.instances.end())
                return Output::Failure(MakeError(UiErrors::TemplateGraphInvalid));
            const auto index = static_cast<std::size_t>(found - state_->source.instances.begin());
            const auto &projection = state_->expansions[index];
            if (const auto valid = ValidateDetachIdentities(*state_, projection, identities); valid.HasError())
                return Output::Failure(valid.ErrorValue());
            auto materialized = Materialize(projection, identities);
            if (materialized.HasError())
                return Output::Failure(materialized.ErrorValue());
            auto revision = state_->source.ordinary.Revision().Next();
            if (revision.HasError())
                return Output::Failure(revision.ErrorValue());
            const std::array additions{std::move(materialized).Value()};
            auto ordinary = RevisionCopy(state_->source.ordinary, revision.Value(), additions);
            if (ordinary.HasError())
                return Output::Failure(ordinary.ErrorValue());
            UiTemplateAuthoredDocument source{std::move(ordinary).Value(), state_->source.instances, state_->source.styles,
                                              state_->source.localizedTexts, state_->source.bindings};
            source.instances.erase(source.instances.begin() + static_cast<std::ptrdiff_t>(index));
            auto catalogs = state_->catalogs;
            catalogs.erase(catalogs.begin() + static_cast<std::ptrdiff_t>(index));
            source.styles.insert(source.styles.end(), additions[0].styles.begin(), additions[0].styles.end());
            source.localizedTexts.insert(source.localizedTexts.end(), additions[0].localizedTexts.begin(),
                                         additions[0].localizedTexts.end());
            source.bindings.insert(source.bindings.end(), additions[0].bindings.begin(), additions[0].bindings.end());
            auto next = PrepareState(std::move(source), std::move(catalogs), state_->limits, cancellation, state_->authoringSchemas);
            if (next.HasError())
                return Output::Failure(next.ErrorValue());
            return Output::Success(UiTemplatePreparedTransaction{state_, std::move(next).Value(), identity_, cancellation});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }

    /** @copydoc UiTemplateDocumentOwner::Commit */
    Result<UiTemplateDocumentSnapshot> UiTemplateDocumentOwner::Commit(const UiTemplatePreparedTransaction &candidate) {
        if (!identity_)
            return Result<UiTemplateDocumentSnapshot>::Failure(MakeError(UiErrors::TemplateDocumentShutdown));
        if (candidate.owner_.lock() != identity_)
            return Result<UiTemplateDocumentSnapshot>::Failure(MakeError(UiErrors::TemplateDocumentOwnerMismatch));
        if (candidate.base_ != state_)
            return Result<UiTemplateDocumentSnapshot>::Failure(MakeError(Runtime::Ui::UiErrors::RevisionStale));
        if (candidate.cancellation_.IsCancellationRequested())
            return Result<UiTemplateDocumentSnapshot>::Failure(MakeError(UiErrors::TemplateDocumentCancelled));
        if (undo_.size() == undoLimit_)
            undo_.erase(undo_.begin());
        undo_.push_back(state_);
        state_ = candidate.next_;
        return Result<UiTemplateDocumentSnapshot>::Success(Capture());
    }

    /** @copydoc UiTemplateDocumentOwner::Undo */
    Result<UiTemplateDocumentSnapshot> UiTemplateDocumentOwner::Undo() {
        try {
            if (!identity_)
                return Result<UiTemplateDocumentSnapshot>::Failure(MakeError(UiErrors::TemplateDocumentShutdown));
            if (undo_.empty())
                return Result<UiTemplateDocumentSnapshot>::Failure(MakeError(UiErrors::TemplateUndoEmpty));
            const auto revision = state_->source.ordinary.Revision().Next();
            if (revision.HasError())
                return Result<UiTemplateDocumentSnapshot>::Failure(revision.ErrorValue());
            const auto previous = undo_.back();
            auto ordinary = RevisionCopy(previous->source.ordinary, revision.Value());
            if (ordinary.HasError())
                return Result<UiTemplateDocumentSnapshot>::Failure(ordinary.ErrorValue());
            auto restored = PrepareState({std::move(ordinary).Value(), previous->source.instances, previous->source.styles,
                                          previous->source.localizedTexts, previous->source.bindings},
                                         previous->catalogs, state_->limits, {}, previous->authoringSchemas);
            if (restored.HasError())
                return Result<UiTemplateDocumentSnapshot>::Failure(restored.ErrorValue());
            state_ = std::move(restored.Value());
            undo_.pop_back();
            return Result<UiTemplateDocumentSnapshot>::Success(Capture());
        } catch (const std::bad_alloc &) {
            return Result<UiTemplateDocumentSnapshot>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }

    /** @copydoc UiTemplateDocumentOwner::Shutdown */
    void UiTemplateDocumentOwner::Shutdown() noexcept {
        identity_.reset();
        undo_.clear();
    }
}  // namespace Horo::UiTemplates
