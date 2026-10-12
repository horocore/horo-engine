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
}  // namespace Horo::UiTemplates::Detail
