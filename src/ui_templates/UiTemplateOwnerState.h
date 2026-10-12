#pragma once

#include "Horo/UiTemplates/UiTemplateDocumentOwner.h"

namespace Horo::UiTemplates::Detail {
    /** @brief Complete immutable authoring publication; projections retain exactly the same catalog lifetimes. */
    struct UiTemplateOwnerState final {
        UiTemplateAuthoredDocument source;
        std::vector<std::shared_ptr<const UiTemplateCatalog>> catalogs;
        std::vector<UiTemplateExpansion> expansions;
        UiTemplateLimits limits;
        std::shared_ptr<const UiTemplateCatalog> authoringSchemas;
    };

    /** @brief Copies ordinary authority and admitted projections through the owning document validator. */
    Result<Runtime::Ui::UiDocument> CopyTemplateOrdinary(const Runtime::Ui::UiDocument &source, Runtime::Ui::UiDocumentRevision revision,
                                                         std::span<const UiTemplateExpansion> expansions = {},
                                                         std::span<const Runtime::Ui::UiAssetDependency> dependencies = {});

    /**
     * @brief Copies a validated expansion into an independently identified detached projection.
     * @param accepted Immutable expanded content, borrowed only for this synchronous operation.
     * @param allocation Complete fresh identity allocation already validated by the document owner.
     * @return Owned remapped content, or the original typed binding-identity failure.
     * @pre The owner has admitted the allocation's completeness, uniqueness and absence from its current authority.
     */
    Result<UiTemplateExpansion> MaterializeTemplateExpansion(const UiTemplateExpansion &accepted,
                                                             std::span<const UiTemplateDetachIdentity> allocation);
}  // namespace Horo::UiTemplates::Detail
