#pragma once
#include "Horo/Extensions/EditorSurfaceRegistry.h"

#include <string>
#include <vector>

namespace Horo::Editor {
    struct EditorGuiContext;

    /** @brief Retained host-only unavailable-surface panel; no provider code or persisted activation evidence is consumed. */
    class ExtensionRecoveryView final {
    public:
        /** @brief Borrows the GUI and its existing registry for the workspace view lifetime. */
        ExtensionRecoveryView(const EditorGuiContext &context, Extensions::EditorSurfaceRegistry *registry) noexcept;
        /** @brief Captures bounded labels and intent on registry/locale changes, outside draw traversal. */
        void Update();
        /** @brief Draws identifiable open placeholders and queues owner-lane close operations without activating providers. */
        void Draw() const;

    private:
        /** @brief Cached display for one untrusted but validated saved contribution identity. */
        struct Entry {
            std::string id;
            std::string provider;
            Extensions::EditorSurfaceProviderKey providerKey;
            std::string reason;
        };

        const EditorGuiContext &context_;
        Extensions::EditorSurfaceRegistry *registry_{};
        std::uint64_t revision_{};
        std::string locale_;
        std::string title_;
        std::string explanation_;
        std::string close_;
        std::vector<Entry> entries_;
    };
}  // namespace Horo::Editor
