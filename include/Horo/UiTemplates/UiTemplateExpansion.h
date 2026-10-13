#pragma once

/** @file UiTemplateExpansion.h
 * @brief Bounded immutable expansion into ordinary document elements and existing rich typed UI inputs.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/UiTemplates/UiTemplateCatalog.h"

namespace Horo::UiTemplates {
    /** @brief One exact ordinary element/style-property literal for existing style-owner composition. */
    struct UiTemplateExpandedStyle final {
        Runtime::Ui::UiElementId element;
        Runtime::Ui::UiStyleAssignment assignment;
    };

    /** @brief Localized authored attachment with exact final document element identity. */
    struct UiTemplateExpandedText final {
        Runtime::Ui::UiElementId element;
        Runtime::Ui::LocalizedMessageRef text;
        Runtime::Ui::UiStylePropertyId property; /**< Exact declared attachment identity, retained through detach and cook. */
    };

    /** @brief Complete independently owned detached projection; no linked authority or catalog borrow is retained. */
    struct UiTemplateExpansion final {
        Runtime::Ui::UiElementId root;
        std::vector<Runtime::Ui::UiDocumentElement> elements;
        std::vector<UiTemplateExpandedStyle> styles;
        std::vector<UiTemplateExpandedText> localizedTexts;
        std::vector<Runtime::Ui::UiBindingDescriptor> bindings;
        std::vector<UiTemplateReference> acceptedDependencies;
        std::vector<Runtime::Ui::UiElementId>
            templateOwnedElements; /**< Exact derived IDs that detach must freshly allocate; host slot IDs are excluded. */
    };

    /**
     * @brief Expands one pinned linked use under complete source/output/identity/type constraints.
     * @param catalog Owned pinned source and registered schemas; borrowed only during this call.
     * @param document Stable owning document identity used by version-one deterministic identity derivation.
     * @param instance Accepted revision and typed public arguments/slot content; never mutated.
     * @param occupied All ordinary/other-instance identities already owned by the target document.
     * @param cancellation Captured cooperative cancellation ancestry.
     * @return Owned complete projection or a typed failure; neither input nor target document is modified.
     * @pre Explicit load-time operation. Host validates complete composed focus/accessibility/layout before activation.
     */
    [[nodiscard]] Result<UiTemplateExpansion> ExpandTemplate(const UiTemplateCatalog &catalog, Runtime::Ui::UiDocumentId document,
                                                             const UiTemplateInstanceRecord &instance,
                                                             std::span<const Runtime::Ui::UiElementId> occupied,
                                                             const CancellationToken &cancellation = {});
}  // namespace Horo::UiTemplates
