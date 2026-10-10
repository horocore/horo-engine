#pragma once

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/UiTemplates/UiTemplateDocumentOwner.h"

namespace Horo::UiTemplates::Detail {
    /** @brief Allocation-free view of a complete prospective owner publication; replacement fields borrow one rebase selection. */
    struct TemplateDocumentBudgetInput final {
        const Runtime::Ui::UiDocument &ordinary;
        std::span<const UiTemplateInstanceRecord> instances;
        const UiTemplateLimits &limits;
        std::span<const std::shared_ptr<const UiTemplateCatalog>> catalogs;
        const UiTemplateCatalog *fallbackCatalog{};
        std::size_t replacementIndex{static_cast<std::size_t>(-1)};
        const UiTemplateReference *replacement{};
        const UiTemplateCatalog *replacementCatalog{};
        std::span<const UiTemplateExpandedStyle> styles;
        std::span<const UiTemplateExpandedText> localizedTexts;
        std::span<const Runtime::Ui::UiBindingDescriptor> bindings;
        std::span<const UiTemplateCatalogLease> leases;
        const UiTemplateInstanceRecord *insertion{};
        const UiTemplateCatalog *insertionCatalog{};
    };

    /** @brief Checks admitted aggregate ordinary+expanded elements/properties/payload/dependency occurrences before candidate allocation.
     */
    [[nodiscard]] Result<void> ValidateTemplateDocumentBudget(const TemplateDocumentBudgetInput &input,
                                                              const CancellationToken &cancellation);
    /** @brief Checks cumulative owned source/default/nested-slot budgets without copying or constructing expansion output. */
    [[nodiscard]] Result<void> ValidateTemplateSourceBudget(std::span<const UiTemplateDocument> documents, const UiTemplateLimits &limits);
    /** @brief Walks the actual pinned expansion/slot inputs under stack-only ancestry before expansion allocation. */
    [[nodiscard]] Result<void> ValidateTemplateExpansionBudget(const UiTemplateCatalog &catalog, const UiTemplateInstanceRecord &instance,
                                                               const CancellationToken &cancellation);
}  // namespace Horo::UiTemplates::Detail
