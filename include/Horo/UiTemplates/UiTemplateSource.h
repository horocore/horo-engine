#pragma once

/** @file UiTemplateSource.h
 * @brief Canonical source commitment for exact Assets-backed template revisions.
 */
#include "Horo/UiTemplates/UiTemplateCatalog.h"

namespace Horo::UiTemplates {
    /** @brief Encodes a complete admitted template's canonical semantic source without a self-referential revision field.
     * @param catalog Exact immutable source/schema authority; the compiled and caller ceilings are already enforced.
     * @param reference Exact accepted source revision/interface to resolve.
     * @return Canonical UTF-8 source or a preserved dependency/capacity failure.
     * @details The host compares these bytes with Assets' independently captured source and derives the SHA-256 revision
     * from these bytes. Nested accepted revisions, packages, all typed constraints, targets and default content are included.
     */
    [[nodiscard]] Result<std::string> SerializeTemplateSource(const UiTemplateCatalog &catalog, const UiTemplateReference &reference);
}  // namespace Horo::UiTemplates
