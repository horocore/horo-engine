#pragma once

#include "Horo/UiTemplates/UiTemplateModel.h"

namespace Horo::UiTemplates::Detail {
    /** @brief Closed version-one domains; their wire strings and field order are persistent identity contracts. */
    enum class TemplateIdentityDomain : std::uint8_t {
        Element,
        SlotDefault
    };

    /** @brief Encodes fixed-width identity fields after one document and counted stable lineage.
     * @param domain Versioned domain. @param document Owning document identity.
     * @param lineage Nonempty valid instance lineage. @param suffix Domain-specific fixed-width identity fields.
     * @return Derived element identity or typed invalid/budget failure without allocation.
     */
    [[nodiscard]] Result<Runtime::Ui::UiElementId> EncodeTemplateIdentity(TemplateIdentityDomain domain, Runtime::Ui::UiDocumentId document,
                                                                          std::span<const UiTemplateInstanceId> lineage,
                                                                          std::span<const Runtime::Ui::SerializedUiId> suffix);
}  // namespace Horo::UiTemplates::Detail
