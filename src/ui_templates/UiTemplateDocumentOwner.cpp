#include "Horo/UiTemplates/UiTemplateDocumentOwner.h"

#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateBudget.h"
#include "UiTemplateOwnerState.h"
#include "UiTemplateParameterValidation.h"

#include <algorithm>
#include <exception>
#include <map>
#include <new>
#include <set>
#include <tuple>

namespace Horo::UiTemplates {
    namespace {
        using namespace Runtime::Ui;
        using State = Detail::UiTemplateOwnerState;

        /** @brief Admits complete unique exact catalog leases before prospective source budget validation. */
        Result<void> ValidateLeases(std::span<const UiTemplateCatalogLease> leases) {
            for (std::size_t i = 0; i < leases.size(); ++i) {
                if (!leases[i].catalog)
                    return Result<void>::Failure(MakeError(UiErrors::TemplateMissing));
                for (std::size_t j = 0; j < i; ++j)
                    if (leases[i].reference.asset == leases[j].reference.asset &&
                        leases[i].reference.revision == leases[j].reference.revision)
                        return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
            }
            return Result<void>::Success();
        }

        /** @brief Retains a catalog for every accepted instance without ambient revision selection. */
        Result<std::vector<std::shared_ptr<const UiTemplateCatalog>>> ResolveCatalogLeases(const UiTemplateAuthoredDocument &source,
                                                                                           std::span<const UiTemplateCatalogLease> leases) {
            std::vector<std::shared_ptr<const UiTemplateCatalog>> catalogs;
            catalogs.reserve(source.instances.size());
            for (const auto &instance : source.instances) {
                const auto lease = std::ranges::find_if(leases, [&](const UiTemplateCatalogLease &candidate) {
                    return candidate.reference.asset == instance.accepted.asset &&
                           candidate.reference.revision == instance.accepted.revision;
                });
                if (lease == leases.end())
                    return Result<std::vector<std::shared_ptr<const UiTemplateCatalog>>>::Failure(
                        MakeError(UiErrors::TemplateRevisionUnavailable));
                catalogs.push_back(lease->catalog);
            }
            return Result<std::vector<std::shared_ptr<const UiTemplateCatalog>>>::Success(std::move(catalogs));
        }

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
            if (const auto schema = std::ranges::find_if(catalog.Schemas().properties,
                                                         [&style, found](const auto &property) {
                return property.elementType == found->type && property.property == style.assignment.property && property.style;
            });
                schema == catalog.Schemas().properties.end())
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

        /** @brief Validates complete canonical style keys and their registered value authority. */
        Result<void> ValidateStyles(const UiTemplateAuthoredDocument &source, const UiTemplateCatalog &catalog) {
            for (std::size_t i = 0; i < source.styles.size(); ++i) {
                const auto &style = source.styles[i];
                if (i && source.styles[i - 1].element == style.element &&
                    source.styles[i - 1].assignment.property == style.assignment.property)
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
                if (const auto valid = ValidateStyle(style, source.ordinary, catalog); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }

        /** @brief Validates unique localized property keys before retaining message references. */
        Result<void> ValidateTexts(const UiTemplateAuthoredDocument &source, const UiTemplateCatalog &catalog) {
            for (std::size_t i = 0; i < source.localizedTexts.size(); ++i) {
                const auto &text = source.localizedTexts[i];
                if (i && source.localizedTexts[i - 1].element == text.element && source.localizedTexts[i - 1].property == text.property)
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
                if (const auto valid = ValidateText(text, source.ordinary, catalog); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }

        /** @brief Validates unique durable binding identities against pinned provider schemas. */
        Result<void> ValidateBindings(const UiTemplateAuthoredDocument &source, const UiTemplateCatalog &catalog) {
            for (std::size_t i = 0; i < source.bindings.size(); ++i) {
                const auto &binding = source.bindings[i];
                if (i && source.bindings[i - 1].id == binding.id)
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
                if (const auto valid = ValidateBinding(binding, source.ordinary, catalog); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }

        /** @brief Checks canonical rich keys for conflicts before validating their own typed responsibilities. */
        Result<void> ValidateAttachments(const UiTemplateAuthoredDocument &source, const UiTemplateCatalog &catalog) {
            if (const auto styles = ValidateStyles(source, catalog); styles.HasError())
                return styles;
            if (const auto texts = ValidateTexts(source, catalog); texts.HasError())
                return texts;
            return ValidateBindings(source, catalog);
        }

        /** @brief Owns a private mutable candidate with paired instance/catalog authority until complete admission. */
        struct AuthorityDraft final {
            UiTemplateAuthoredDocument source;
            std::vector<std::shared_ptr<const UiTemplateCatalog>> catalogs;
        };

        /** @brief Copies a complete accepted authority under one new revision after the caller's prospective budget check. */
        Result<AuthorityDraft> CopyNextAuthority(const State &state, const std::span<const UiTemplateExpansion> additions = {}) {
            const auto revision = state.source.ordinary.Revision().Next();
            if (revision.HasError())
                return Result<AuthorityDraft>::Failure(revision.ErrorValue());
            auto ordinary = Detail::CopyTemplateOrdinary(state.source.ordinary, revision.Value(), additions);
            if (ordinary.HasError())
                return Result<AuthorityDraft>::Failure(ordinary.ErrorValue());
            return Result<AuthorityDraft>::Success(AuthorityDraft{{std::move(ordinary).Value(), state.source.instances, state.source.styles,
                                                                   state.source.localizedTexts, state.source.bindings},
                                                                  state.catalogs});
        }

        /** @brief Charges and admits one expansion's complete element and binding identity sets. */
        Result<void> AdmitExpansion(const UiTemplateExpansion &expansion, std::vector<UiElementId> &occupied,
                                    std::set<UiBindingId> &bindings, const UiTemplateLimits &limits) {
            if (occupied.size() > limits.maximumElements || expansion.elements.size() > limits.maximumElements - occupied.size())
                return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
            for (const auto &element : expansion.elements)
                occupied.push_back(element.id);
            if (expansion.bindings.size() > MaximumUiBindingDescriptors - bindings.size())
                return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
            for (const auto &binding : expansion.bindings)
                if (!bindings.insert(binding.id).second)
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
            return Result<void>::Success();
        }

        /** @brief Expands the complete canonical authority with shared identity and binding collision fences. */
        Result<std::vector<UiTemplateExpansion>> ExpandAuthority(const UiTemplateAuthoredDocument &source,
                                                                 std::span<const std::shared_ptr<const UiTemplateCatalog>> catalogs,
                                                                 const UiTemplateLimits &limits, const CancellationToken &cancellation) {
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
                if (!catalogs[index] || !instance.id.IsValid())
                    return Result<std::vector<UiTemplateExpansion>>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
                if (!ids.insert(instance.id).second)
                    return Result<std::vector<UiTemplateExpansion>>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
                if (cancellation.IsCancellationRequested())
                    return Result<std::vector<UiTemplateExpansion>>::Failure(MakeError(UiErrors::TemplateDocumentCancelled));
                auto expansion = ExpandTemplate(*catalogs[index], source.ordinary.Id(), instance, occupied, cancellation);
                if (expansion.HasError())
                    return Result<std::vector<UiTemplateExpansion>>::Failure(expansion.ErrorValue());
                if (const auto admitted = AdmitExpansion(expansion.Value(), occupied, bindingIds, limits); admitted.HasError())
                    return Result<std::vector<UiTemplateExpansion>>::Failure(admitted.ErrorValue());
                expansions.push_back(std::move(expansion).Value());
            }
            return Result<std::vector<UiTemplateExpansion>>::Success(std::move(expansions));
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
            auto expansions = ExpandAuthority(source, catalogs, limits, cancellation);
            if (expansions.HasError())
                return Result<std::shared_ptr<const State>>::Failure(expansions.ErrorValue());
            return Result<std::shared_ptr<const State>>::Success(std::make_shared<const State>(std::move(source), std::move(catalogs),
                                                                                               std::move(expansions).Value(), limits,
                                                                                               std::move(authoringSchemas)));
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

        /** @brief Resolves and reconciles the exact accepted interface before preparing any source draft. */
        Result<std::size_t> SelectRebase(const State &state, const UiTemplateInstanceId instance, const UiTemplateReference &target,
                                         const std::shared_ptr<const UiTemplateCatalog> &catalog) {
            const auto found = std::ranges::find(state.source.instances, instance, &UiTemplateInstanceRecord::id);
            if (!catalog || found == state.source.instances.end() || target.asset != found->accepted.asset)
                return Result<std::size_t>::Failure(MakeError(UiErrors::TemplateRebaseInvalid));
            const auto index = static_cast<std::size_t>(found - state.source.instances.begin());
            const auto previous = state.catalogs[index]->Resolve(found->accepted);
            if (previous.HasError())
                return Result<std::size_t>::Failure(previous.ErrorValue());
            const auto selected = catalog->Resolve(target);
            if (selected.HasError())
                return Result<std::size_t>::Failure(selected.ErrorValue());
            if (selected.Value()->dependency.interfaceVersion.major != previous.Value()->dependency.interfaceVersion.major)
                return Result<std::size_t>::Failure(MakeError(UiErrors::TemplateVersionIncompatible));
            if (const auto valid = Reconcile(*found, *selected.Value()); valid.HasError())
                return Result<std::size_t>::Failure(valid.ErrorValue());
            return Result<std::size_t>::Success(index);
        }

        /** @brief Checks all accepted ordinary and expanded identities before materializing a detached allocation. */
        bool IndependentIdentityOccupied(const State &state, const UiElementId identity) {
            if (std::ranges::find(state.source.ordinary.Elements(), identity, &UiDocumentElement::id) !=
                state.source.ordinary.Elements().end())
                return true;
            return std::ranges::any_of(state.expansions, [identity](const auto &projection) {
                return std::ranges::find(projection.elements, identity, &UiDocumentElement::id) != projection.elements.end();
            });
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
                if (IndependentIdentityOccupied(state, identity.independent))
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid));
            }
            return Result<void>::Success();
        }

    }  // namespace

    UiTemplatePreparedTransaction::UiTemplatePreparedTransaction(std::shared_ptr<const State> base, std::shared_ptr<const State> next,
                                                                 std::weak_ptr<const std::uint8_t> owner, CancellationToken cancellation,
                                                                 std::shared_ptr<const State> retirement, const bool undo) noexcept
        : base_(std::move(base)), next_(std::move(next)), owner_(std::move(owner)), cancellation_(std::move(cancellation)),
          retirement_(std::move(retirement)), undo_(undo) {}

    /** @copydoc UiTemplatePreparedTransaction::Preview */
    UiTemplateDocumentSnapshot UiTemplatePreparedTransaction::Preview() const noexcept {
        return UiTemplateDocumentSnapshot{next_};
    }

    /** @copydoc UiTemplateDocumentOwner::UiTemplateDocumentOwner */
    UiTemplateDocumentOwner::UiTemplateDocumentOwner(ConstructionKey &&, std::shared_ptr<const State> state,
                                                     std::shared_ptr<const std::uint8_t> identity, const std::size_t undoLimit)
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
            std::vector catalogs(source.instances.size(), catalog);
            auto state = PrepareState(std::move(source), std::move(catalogs), limits, {}, catalog);
            if (state.HasError())
                return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Failure(state.ErrorValue());
            return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Success(
                std::make_unique<UiTemplateDocumentOwner>(ConstructionKey{}, std::move(state).Value(),
                                                          std::make_shared<const std::uint8_t>(0), undoLimit));
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
            if (const auto valid = ValidateLeases(leases); valid.HasError())
                return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Failure(valid.ErrorValue());
            if (const auto valid = Detail::ValidateTemplateDocumentBudget(BudgetView(source, limits, {}, nullptr, leases), {});
                valid.HasError())
                return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Failure(valid.ErrorValue());
            auto catalogs = ResolveCatalogLeases(source, leases);
            if (catalogs.HasError())
                return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Failure(catalogs.ErrorValue());
            auto state = PrepareState(std::move(source), std::move(catalogs).Value(), limits, {}, std::move(authoringSchemas));
            if (state.HasError())
                return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Failure(state.ErrorValue());
            return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Success(
                std::make_unique<UiTemplateDocumentOwner>(ConstructionKey{}, std::move(state).Value(),
                                                          std::make_shared<const std::uint8_t>(0), limits.maximumUndoEntries));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<UiTemplateDocumentOwner>>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }

    UiTemplateDocumentOwner::~UiTemplateDocumentOwner() {
        Shutdown();
    }

    /** @copydoc UiTemplateDocumentOwner::Capture */
    UiTemplateDocumentSnapshot UiTemplateDocumentOwner::Capture() const noexcept {
        if (ownerThread_ != std::this_thread::get_id())
            std::terminate();
        return UiTemplateDocumentSnapshot{state_};
    }

    /** @copydoc UiTemplateDocumentOwner::UpdateAvailable */
    Result<bool> UiTemplateDocumentOwner::UpdateAvailable(const UiTemplateInstanceId instance, const UiTemplateReference &available) const {
        if (const auto thread = CheckOwnerThread(); thread.HasError())
            return Result<bool>::Failure(thread.ErrorValue());
        if (!identity_)
            return Result<bool>::Failure(MakeError(UiErrors::TemplateDocumentShutdown));
        const auto record = std::ranges::find(state_->source.instances, instance, &UiTemplateInstanceRecord::id);
        if (record == state_->source.instances.end() || available.asset != record->accepted.asset)
            return Result<bool>::Failure(MakeError(UiErrors::TemplateRebaseInvalid));
        return Result<bool>::Success(available.revision != record->accepted.revision);
    }

    /** @copydoc UiTemplateDocumentOwner::AdmitPublication */
    Result<UiTemplatePreparedTransaction> UiTemplateDocumentOwner::AdmitPublication(
        UiTemplateAuthoredDocument source, std::vector<std::shared_ptr<const UiTemplateCatalog>> catalogs,
        const CancellationToken &cancellation) const {
        auto next = PrepareState(std::move(source), std::move(catalogs), state_->limits, cancellation, state_->authoringSchemas);
        if (next.HasError())
            return Result<UiTemplatePreparedTransaction>::Failure(next.ErrorValue());
        return Result<UiTemplatePreparedTransaction>::Success(PreparePublication(std::move(next).Value(), cancellation));
    }

    /** @copydoc UiTemplateDocumentOwner::PrepareRebase */
    Result<UiTemplatePreparedTransaction> UiTemplateDocumentOwner::PrepareRebase(const UiTemplateInstanceId instance,
                                                                                 const UiTemplateReference &target,
                                                                                 std::shared_ptr<const UiTemplateCatalog> catalog,
                                                                                 const CancellationToken &cancellation) const {
        if (const auto thread = CheckOwnerThread(); thread.HasError())
            return Result<UiTemplatePreparedTransaction>::Failure(thread.ErrorValue());
        try {
            if (const auto admission = CheckPreparation(cancellation); admission.HasError())
                return Result<UiTemplatePreparedTransaction>::Failure(admission.ErrorValue());
            auto selected = SelectRebase(*state_, instance, target, catalog);
            if (selected.HasError())
                return Result<UiTemplatePreparedTransaction>::Failure(selected.ErrorValue());
            const auto index = selected.Value();
            auto prospective = BudgetView(state_->source, state_->limits, state_->catalogs);
            prospective.replacementIndex = index;
            prospective.replacement = &target;
            prospective.replacementCatalog = catalog.get();
            if (const auto valid = Detail::ValidateTemplateDocumentBudget(prospective, cancellation); valid.HasError())
                return Result<UiTemplatePreparedTransaction>::Failure(valid.ErrorValue());
            auto copied = CopyNextAuthority(*state_);
            if (copied.HasError())
                return Result<UiTemplatePreparedTransaction>::Failure(copied.ErrorValue());
            auto draft = std::move(copied).Value();
            draft.source.instances[index].accepted = target;
            draft.catalogs[index] = std::move(catalog);
            return AdmitPublication(std::move(draft.source), std::move(draft.catalogs), cancellation);
        } catch (const std::bad_alloc &) {
            return Result<UiTemplatePreparedTransaction>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }

    /** @copydoc UiTemplateDocumentOwner::PrepareInsert */
    Result<UiTemplatePreparedTransaction> UiTemplateDocumentOwner::PrepareInsert(UiTemplateInstanceRecord instance,
                                                                                 std::shared_ptr<const UiTemplateCatalog> catalog,
                                                                                 const CancellationToken &cancellation) const {
        if (const auto thread = CheckOwnerThread(); thread.HasError())
            return Result<UiTemplatePreparedTransaction>::Failure(thread.ErrorValue());
        using Output = Result<UiTemplatePreparedTransaction>;
        try {
            if (const auto admission = CheckPreparation(cancellation); admission.HasError())
                return Output::Failure(admission.ErrorValue());
            if (!catalog || !instance.id.IsValid() ||
                std::ranges::find(state_->source.instances, instance.id, &UiTemplateInstanceRecord::id) != state_->source.instances.end())
                return Output::Failure(MakeError(UiErrors::TemplateGraphInvalid));
            auto prospective = BudgetView(state_->source, state_->limits, state_->catalogs);
            prospective.insertion = &instance;
            prospective.insertionCatalog = catalog.get();
            if (const auto valid = Detail::ValidateTemplateDocumentBudget(prospective, cancellation); valid.HasError())
                return Output::Failure(valid.ErrorValue());
            auto copied = CopyNextAuthority(*state_);
            if (copied.HasError())
                return Output::Failure(copied.ErrorValue());
            auto draft = std::move(copied).Value();
            draft.source.instances.push_back(std::move(instance));
            draft.catalogs.push_back(std::move(catalog));
            return AdmitPublication(std::move(draft.source), std::move(draft.catalogs), cancellation);
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }

    /** @copydoc UiTemplateDocumentOwner::PrepareInsertIndependent */
    Result<UiTemplatePreparedTransaction> UiTemplateDocumentOwner::PrepareInsertIndependent(
        UiTemplateInstanceRecord instance, std::shared_ptr<const UiTemplateCatalog> catalog,
        const std::span<const UiTemplateDetachIdentity> identities, const CancellationToken &cancellation) const {
        if (const auto thread = CheckOwnerThread(); thread.HasError())
            return Result<UiTemplatePreparedTransaction>::Failure(thread.ErrorValue());
        using Output = Result<UiTemplatePreparedTransaction>;
        try {
            const auto id = instance.id;
            auto inserted = PrepareInsert(std::move(instance), std::move(catalog), cancellation);
            if (inserted.HasError())
                return Output::Failure(inserted.ErrorValue());
            // The intermediate owner is private preparation only; neither authority nor undo history is published.
            UiTemplateDocumentOwner intermediate{ConstructionKey{}, inserted.Value().next_, std::make_shared<const std::uint8_t>(0),
                                                 undoLimit_};
            auto detached = intermediate.PrepareDetach(id, identities, cancellation);
            if (detached.HasError())
                return Output::Failure(detached.ErrorValue());
            return Output::Success(PreparePublication(detached.Value().next_, cancellation));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }

    /** @copydoc UiTemplateDocumentOwner::PrepareDetach */
    Result<UiTemplatePreparedTransaction> UiTemplateDocumentOwner::PrepareDetach(const UiTemplateInstanceId instance,
                                                                                 const std::span<const UiTemplateDetachIdentity> identities,
                                                                                 const CancellationToken &cancellation) const {
        if (const auto thread = CheckOwnerThread(); thread.HasError())
            return Result<UiTemplatePreparedTransaction>::Failure(thread.ErrorValue());
        using Output = Result<UiTemplatePreparedTransaction>;
        try {
            if (const auto admission = CheckPreparation(cancellation); admission.HasError())
                return Output::Failure(admission.ErrorValue());
            const auto found = std::ranges::find(state_->source.instances, instance, &UiTemplateInstanceRecord::id);
            if (found == state_->source.instances.end())
                return Output::Failure(MakeError(UiErrors::TemplateGraphInvalid));
            const auto index = static_cast<std::size_t>(found - state_->source.instances.begin());
            const auto &projection = state_->expansions[index];
            if (const auto valid = ValidateDetachIdentities(*state_, projection, identities); valid.HasError())
                return Output::Failure(valid.ErrorValue());
            auto materialized = Detail::MaterializeTemplateExpansion(projection, identities);
            if (materialized.HasError())
                return Output::Failure(materialized.ErrorValue());
            const std::array additions{std::move(materialized).Value()};
            auto copied = CopyNextAuthority(*state_, additions);
            if (copied.HasError())
                return Output::Failure(copied.ErrorValue());
            auto draft = std::move(copied).Value();
            auto &source = draft.source;
            source.instances.erase(source.instances.begin() + static_cast<std::ptrdiff_t>(index));
            auto &catalogs = draft.catalogs;
            catalogs.erase(catalogs.begin() + static_cast<std::ptrdiff_t>(index));
            source.styles.insert(source.styles.end(), additions[0].styles.begin(), additions[0].styles.end());
            source.localizedTexts.insert(source.localizedTexts.end(), additions[0].localizedTexts.begin(),
                                         additions[0].localizedTexts.end());
            source.bindings.insert(source.bindings.end(), additions[0].bindings.begin(), additions[0].bindings.end());
            return AdmitPublication(std::move(draft.source), std::move(draft.catalogs), cancellation);
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }

    /** @copydoc UiTemplateDocumentOwner::PrepareUndo */
    Result<UiTemplatePreparedTransaction> UiTemplateDocumentOwner::PrepareUndo(const CancellationToken &cancellation) const {
        if (const auto thread = CheckOwnerThread(); thread.HasError())
            return Result<UiTemplatePreparedTransaction>::Failure(thread.ErrorValue());
        using Output = Result<UiTemplatePreparedTransaction>;
        try {
            if (const auto admission = CheckPreparation(cancellation); admission.HasError())
                return Output::Failure(admission.ErrorValue());
            if (undo_.empty())
                return Output::Failure(MakeError(UiErrors::TemplateUndoEmpty));
            const auto revision = state_->source.ordinary.Revision().Next();
            if (revision.HasError())
                return Output::Failure(revision.ErrorValue());
            const auto previous = undo_.back();
            auto ordinary = Detail::CopyTemplateOrdinary(previous->source.ordinary, revision.Value());
            if (ordinary.HasError())
                return Output::Failure(ordinary.ErrorValue());
            auto restored = PrepareState({std::move(ordinary).Value(), previous->source.instances, previous->source.styles,
                                          previous->source.localizedTexts, previous->source.bindings},
                                         previous->catalogs, state_->limits, cancellation, previous->authoringSchemas);
            if (restored.HasError())
                return Output::Failure(restored.ErrorValue());
            return Output::Success(
                UiTemplatePreparedTransaction{state_, std::move(restored).Value(), identity_, cancellation, previous, true});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
        }
    }

}  // namespace Horo::UiTemplates
