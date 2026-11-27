#pragma once

#include "Horo/Editor/ProposedChangeSet.h"

#include <algorithm>

namespace Horo::Editor::Proposals::PatchDetail {
    /** @brief Folds ASCII for portable alias detection without altering canonical spellings. */
    inline char Lower(const char character) noexcept {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A')) : character;
    }

    /** @brief Owns a portable collision key. The workspace owner checks physical filesystem aliases. */
    inline std::string Fold(const std::string_view text) {
        std::string key(text);
        for (auto &character : key)
            character = Lower(character);
        return key;
    }

    /** @brief Compares portable aliases without allocating collision keys. */
    inline bool SamePortablePath(const SourceDocumentId &left, const SourceDocumentId &right) noexcept {
        return std::ranges::equal(left.Value(), right.Value(), {}, Lower, Lower);
    }

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
