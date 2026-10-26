#pragma once
/** @file OpenXRInstalledBindings.h
 * @brief Bounded physical declarations read only from an already verified selected product archive.
 */
#include "OpenXRApplicationBindings.h"
#include "OpenXRComponentManifest.h"

namespace Horo::Application::Internal {
    /** @brief Copies one explicit product's bounded schema-one native vocabulary from its sealed archive.
     * @param component Same verified installed product selected by the signed host catalog.
     * @param path Exact signed-catalog declaration path, which must also exist in the package's verified file manifest.
     * @return Immutable owned physical declarations or original archive/typed schema failure.
     * @details Creates no semantic actions, contexts, assignments, native objects or Input publication. Prepare must
     * still join these declarations to the SAME live InputRouter::Actions and current context/player-bound port.
     * Persisted Input overrides/migrations are not supplied by this first-version installer document.
     * @throws std::bad_alloc Bounded cold preparation allocation failure; no partial catalog escapes.
     */
    [[nodiscard]] Result<std::shared_ptr<const OpenXRApplicationBindings>> ReadInstalledOpenXRBindings(
        const VerifiedOpenXRComponent &component, const Packages::PackagePath &path);
}  // namespace Horo::Application::Internal
