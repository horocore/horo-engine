#pragma once
#include "SettingsModalInternal.h"

namespace Horo::Editor::SettingsModalInternal {
    /** @brief Explicit transient authoring section; persistent identity never derives from a row index. */
    enum class CollisionItemKind : int {
        Layer,
        Profile,
        Channel
    };
    /** @brief Invalidates a stale review whenever local form fields change. */
    void CollisionEdited(SettingsCollisionDraft &draft) noexcept;
    /** @brief Draws complete layer pair responses using shared response controls. */
    void DrawCollisionMatrix(SettingsCollisionDraft &draft, const EditorGuiContext &ctx);
    /** @brief Draws stable-ID profile and query response controls without allocating a per-frame projection. */
    void DrawCollisionProfiles(SettingsCollisionDraft &draft, const EditorGuiContext &ctx);
    /** @brief Draws typed definition selection, metadata, flags, create and reviewed deletion controls. */
    void DrawCollisionDefinitions(SettingsCollisionDraft &draft, const CollisionProjectSettingsSession &owner, const EditorGuiContext &ctx);
}  // namespace Horo::Editor::SettingsModalInternal
