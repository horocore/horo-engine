#pragma once

#include "Horo/Editor/ProposedChangeSet.h"

namespace Horo::Editor::Proposals::PatchDetail {
    /** @brief Resolves an exact offered source with no fallback or ambiguous selection. */
    inline const SourceDocumentSnapshot *ResolveBase(const SourceDocumentId &path, std::span<const SourceDocumentSnapshot> sources) {
        const SourceDocumentSnapshot *matched{};
        for (const auto &source : sources) {
            const auto identity = source.Identity();
            if (identity.key.kind != DocumentKind::Source || identity.key.source != path)
                continue;
            if (matched)
                return nullptr;
            matched = &source;
        }
        return matched && matched->IsCurrent() ? matched : nullptr;
    }
}  // namespace Horo::Editor::Proposals::PatchDetail
