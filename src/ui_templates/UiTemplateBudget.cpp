#include "UiTemplateBudget.h"

#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateBudgetLedger.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>

namespace Horo::UiTemplates::Detail {
    namespace {
        using Budget = TemplateBudgetLedger;

        /** @brief Walks actual nested references/defaults/slot instances with a fixed-size active asset stack. */
        struct ExpansionBudget final {
            const UiTemplateCatalog &catalog;
            const CancellationToken &cancellation;
            Budget &budget;
            // Validated limits cap depth at this capacity; Instance admits depth before indexing the active path.
            std::array<UiTemplateAssetId, 32> ancestry;

            /** @brief Charges one recursive activation before following any dependency edge. */
            bool AdmitDepth(const std::size_t depth) const {
                return depth < budget.limits.maximumDepth && budget.Add(budget.instances, 1, budget.limits.maximumInstances) &&
                       budget.Add(budget.dependencies, 1, budget.limits.maximumDependencies);
            }

            /** @brief Rejects unrecognized slot identities before charging their content. */
            static bool ContainsSlots(const UiTemplateDocument &document, const std::span<const UiTemplateSlotContent> slots) {
                return std::ranges::all_of(slots, [&document](const auto &slot) {
                    return std::ranges::find(document.slots, slot.slot, &UiTemplateSlot::id) != document.slots.end();
                });
            }

            /** @brief Charges the complete resolved instance in the existing document/arguments/slots/depth/target order. */
            bool ChargeInstance(const UiTemplateDocument &document, const std::span<const UiTemplateArgument> arguments,
                                const std::span<const UiTemplateSlotContent> slots, const std::size_t depth,
                                const std::size_t parentDepth) {
                return budget.Document(document) && budget.Arguments(arguments) && budget.SlotInput(slots, depth + 1, false) &&
                       budget.ComposedDepth(std::span{document.elements}, parentDepth) && budget.TargetValues(catalog, document, arguments);
            }

            /** @brief Walks typed nested records at their actual local attachment depth using the same active ancestry. */
            template <typename InstanceRecord, typename Element>
            Result<void> Nested(const std::span<const InstanceRecord> instances, const std::span<const Element> elements,
                                const std::size_t depth, const std::size_t parentDepth) {
                for (const auto &nested : instances) {
                    const auto localDepth = ElementDepth(elements, nested.parent, budget.limits.maximumDepth);
                    if (!localDepth.has_value() || parentDepth > budget.limits.maximumDepth - *localDepth)
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
                    if (!localDepth.has_value() || parentDepth > budget.limits.maximumDepth - *localDepth)
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
                if (!AdmitDepth(depth))
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
                if (const auto active = std::span{ancestry}.first(depth); std::ranges::find(active, reference.asset) != active.end())
                    return Result<void>::Failure(MakeError(UiErrors::TemplateDependencyCycle));
                ancestry[depth] = reference.asset;
                const auto resolved = catalog.Resolve(reference);
                if (resolved.HasError())
                    return Result<void>::Failure(resolved.ErrorValue());
                const auto &document = *resolved.Value();
                if (!ContainsSlots(document, slots))
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphInvalid, "Unknown template slot."));
                if (!ChargeInstance(document, arguments, slots, depth, parentDepth))
                    return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
                if (const auto nested = Nested(std::span{document.nested}, std::span{document.elements}, depth, parentDepth);
                    nested.HasError())
                    return nested;
                return Slots(document, slots, depth, parentDepth);
            }
        };

        /** @brief Selects only the exact replacement, fallback, leased or parallel catalog authority. */
        const UiTemplateCatalog *SelectCatalog(const TemplateDocumentBudgetInput &input, const std::size_t index) {
            if (index == input.replacementIndex)
                return input.replacementCatalog;
            if (input.fallbackCatalog)
                return input.fallbackCatalog;
            if (input.leases.empty())
                return input.catalogs[index].get();
            const auto &accepted = input.instances[index].accepted;
            const auto lease = std::ranges::find_if(input.leases, [&accepted](const UiTemplateCatalogLease &candidate) {
                return candidate.reference.asset == accepted.asset && candidate.reference.revision == accepted.revision;
            });
            return lease == input.leases.end() ? nullptr : lease->catalog.get();
        }

        /** @brief Charges a private insertion before walking the existing instance set. */
        Result<void> ChargeInsertion(const TemplateDocumentBudgetInput &input, const CancellationToken &cancellation, Budget &counters) {
            if (!input.insertion)
                return Result<void>::Success();
            if (!input.insertionCatalog || input.instances.size() >= input.limits.maximumInstances)
                return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
            const auto depth = ElementDepth(input.ordinary.Elements(), input.insertion->parent, input.limits.maximumDepth);
            if (!depth.has_value())
                return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
            ExpansionBudget walk{*input.insertionCatalog, cancellation, counters, {}};
            return walk.Instance(input.insertion->accepted, input.insertion->arguments, input.insertion->slots, 0, *depth);
        }

        /** @brief Admits a finite view with exact parallel catalog coverage or explicit fallback/lease authority. */
        bool ValidInputEnvelope(const TemplateDocumentBudgetInput &input) {
            return input.limits.IsValid() && input.instances.size() <= input.limits.maximumInstances && input.leases.size() <= 256 &&
                   (input.fallbackCatalog || !input.leases.empty() || input.catalogs.size() == input.instances.size());
        }

        /** @brief Charges one current/replacement instance after selecting its exact owner-provided catalog. */
        Result<void> ChargeExisting(const TemplateDocumentBudgetInput &input, const std::size_t index,
                                    const CancellationToken &cancellation, Budget &counters) {
            const auto &instance = input.instances[index];
            const bool replaced = index == input.replacementIndex;
            const auto *catalog = SelectCatalog(input, index);
            const auto &reference = replaced && input.replacement ? *input.replacement : instance.accepted;
            if (!catalog)
                return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
            const auto parentDepth = ElementDepth(input.ordinary.Elements(), instance.parent, input.limits.maximumDepth);
            if (!parentDepth.has_value())
                return Result<void>::Failure(MakeError(UiErrors::TemplateGraphBudgetExceeded));
            ExpansionBudget walk{*catalog, cancellation, counters, {}};
            return walk.Instance(reference, instance.arguments, instance.slots, 0, *parentDepth);
        }
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
        if (!ValidInputEnvelope(input))
            return fail();
        Budget counters{input.limits};
        if (!counters.Ordinary(input.ordinary) || !counters.Attachments(input))
            return fail();
        if (const auto insertion = ChargeInsertion(input, cancellation, counters); insertion.HasError())
            return insertion;
        for (std::size_t index = 0; index < input.instances.size(); ++index)
            if (const auto valid = ChargeExisting(input, index, cancellation, counters); valid.HasError())
                return valid;
        return Result<void>::Success();
    }
}  // namespace Horo::UiTemplates::Detail
