#pragma once
#include "Horo/Extensions/EditorActivityHost.h"

#include <array>
#include <imgui.h>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Editor {
    struct EditorGuiContext;
    class IEditorGuiRenderer;

    /** @brief GUI-private retained projection and generation-bound texture cache, refreshed only at Update. */
    class ExtensionActivityView final {
    public:
        ExtensionActivityView(const EditorGuiContext &context, Extensions::EditorActivityHost *host, IEditorGuiRenderer *renderer);
        ~ExtensionActivityView();
        ExtensionActivityView(const ExtensionActivityView &) = delete;
        ExtensionActivityView &operator=(const ExtensionActivityView &) = delete;
        ExtensionActivityView(ExtensionActivityView &&) = delete;
        ExtensionActivityView &operator=(ExtensionActivityView &&) = delete;
        void Update();
        /** @brief Queues a generation-bound placement command; publication occurs at the next owner Update. */
        [[nodiscard]] bool QueueMove(const Extensions::EditorSurfaceProviderIdentity &provider, std::string_view id,
                                     Extensions::EditorActivityPlacement placement) noexcept;
        /** @brief Returns a destination-side native panel withdrawal queued by a click or an open move. */
        [[nodiscard]] std::optional<Extensions::EditorActivitySide> TakeNativePanelClear() noexcept;
        [[nodiscard]] std::size_t Count(bool right, std::size_t group) const noexcept;
        [[nodiscard]] bool DrawItems(bool right, std::size_t group, ImVec2 origin, float width, float bottom);
        [[nodiscard]] bool HasDrawer(Extensions::EditorActivitySide side) const noexcept;
        void DrawDrawer(Extensions::EditorActivitySide side, ImVec2 position, ImVec2 size);
        void Close(Extensions::EditorActivitySide side);

    private:
        struct Entry {
            Extensions::EditorActivityProjection projection;
            std::uintptr_t texture{};
            std::string label;
            std::string tooltip;
            std::vector<std::string> nodeText;
            bool focusPending{};
            std::uint64_t token{};
            std::int32_t ordinal{};
        };

        struct DragPayload {
            std::uintptr_t owner{};
            std::uint64_t token{};
            std::uint64_t revision{};
        };

        struct PendingMove {
            DragPayload source;
            Extensions::EditorActivityPlacement target;
        };

        /** @brief Applies a queued move only while its exact retained projection and provider remain live. */
        void ApplyPendingMove();
        /** @brief Refreshes copied localization and transfers texture ownership at the owner Update boundary. */
        [[nodiscard]] Entry PrepareEntry(const Extensions::EditorActivityProjection &projection, const std::string &locale);
        /** @brief Transfers unique texture ownership and tracks opening/focus transitions before retiring the old projection. */
        void PrepareResources(Entry &entry, Entry *previous);
        /** @brief Copies provider-localized node text during the owner update phase. */
        void PrepareNodeText(Entry &entry, const std::string &locale);
        /** @brief Draws the admitted form using retained text and queues typed actions without invoking providers. */
        void DrawForm(const Entry &entry);
        /** @brief Presents localized destination and insertion actions for one retained control. */
        void DrawPlacementMenu(const Entry &entry, std::size_t group);
        /** @brief Presents durable user visibility choices for the current activity rail. */
        void DrawVisibilityMenu(bool right);
        /** @brief Applies a typed activity toggle and queues native-panel withdrawal only after successful admission. */
        [[nodiscard]] bool ActivateEntry(const Entry &entry);
        void AcceptMove(Extensions::EditorActivityPlacement placement);
        [[nodiscard]] std::int32_t GroupSize(Extensions::EditorActivitySide side, std::uint8_t group) const noexcept;

        const EditorGuiContext &context_;
        Extensions::EditorActivityHost *host_{};
        IEditorGuiRenderer *renderer_{};
        std::uint64_t revision_{};
        std::string locale_;
        std::vector<Entry> entries_;
        std::uint64_t nextToken_{1};
        std::optional<PendingMove> pendingMove_;
        std::array<std::optional<std::uint64_t>, 3> nativePanelClear_{};
        std::array<std::string, 3> moveLabels_;
        std::string moveEarlierLabel_;
        std::string moveLaterLabel_;
    };
}  // namespace Horo::Editor
