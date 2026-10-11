#pragma once

/** @file SourceDocumentCodeQuery.h
 * @brief Read-only projection of existing editor source sessions into project code queries.
 */
#include "Horo/Application/ProjectCodeQuery.h"
#include "Horo/Editor/SourceDocumentService.h"

namespace Horo::Editor {
    /** @brief Retains existing source owners and queries only Find/Snapshot; never opens or edits a document.
     * @param documents Existing source owner, retained through admitted callbacks.
     * @param identities Existing identity registry, synchronized with the same owner thread.
     * @param projectIdentity Exact host-owned project identity.
     * @param projectGeneration Nonzero host-owned project-session generation.
     * @return Existing-text provider; absent owners leave the capability unavailable.
     * @pre The host creates the adapter on the source owner's thread, schedules calls on that same thread and drains admitted queries
     * before shutdown. Registry lookups are bounded by the host's existing document admission policy. A missing Source key permits disk
     * fallback; an existing but closed/stale session returns its original typed error. The adapter copies at most one MiB from the
     * immutable snapshot.
     */
    [[nodiscard]] Application::CodeQueryProviders MakeSourceDocumentCodeQueryProviders(
        std::shared_ptr<const SourceDocumentService> documents, std::shared_ptr<const DocumentIdentityRegistry> identities,
        std::string projectIdentity, std::uint64_t projectGeneration);
}  // namespace Horo::Editor
