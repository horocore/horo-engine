#pragma once
#include "Horo/Extensions/EditorActivityHost.h"
#include "Horo/Runtime/Input.h"
#include "editor/modals/extension_pages/EditorPageThemeCapture.h"

#include <array>
#include <imgui.h>
#include <optional>
#include <string>
#include <utility>
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
        /** @brief Applies the workspace/modal input router decision before drawing or pumping pending actions. */
        void SetInputAdmitted(bool admitted);
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
        /** @brief Borrows live host semantic records until the next owner update; no platform accessibility handle crosses the boundary. */
        [[nodiscard]] std::span<const Extensions::EditorUiAccessibleNode> Accessibility(std::string_view surface) const noexcept;

    private:
        /** @brief Move-only UI-thread release owner; the borrowed renderer outlives the view and all its candidate entries. */
        class OwnedTexture final {
        public:
            OwnedTexture() noexcept = default;
            /** @brief Adopts one successful upload; renderer must outlive this owner. */
            OwnedTexture(IEditorGuiRenderer &renderer, std::uintptr_t id) noexcept;
            /** @brief Releases the owned texture through its renderer on the UI thread. */
            ~OwnedTexture();
            OwnedTexture(const OwnedTexture &) = delete;
            OwnedTexture &operator=(const OwnedTexture &) = delete;
            /** @brief Transfers texture release responsibility without allocation. */
            OwnedTexture(OwnedTexture &&other) noexcept;
            /** @brief Releases the current texture and transfers another owner's responsibility. */
            OwnedTexture &operator=(OwnedTexture &&other) noexcept;
            /** @brief Returns the borrowed draw identity, or zero for an empty owner. */
            [[nodiscard]] std::uintptr_t Id() const noexcept;

        private:
            /** @brief Releases the texture exactly once and clears its renderer borrow. */
            void Reset() noexcept;
            IEditorGuiRenderer *renderer_{};
            std::uintptr_t id_{};
        };

        struct CommandBinding {
            std::string id;
            std::vector<Input::InputBinding> bindings;
        };

        std::vector<CommandBinding> commands_;
        std::uint64_t commandSurfaceRevision_{};
        std::uint64_t commandSettingsRevision_{~std::uint64_t{0}};
        bool commandBindingsValid_{};
        /** @brief Rebuilds command shortcuts only after module publication or committed settings change. */
        void PrepareCommands();

        struct Entry {
            Extensions::EditorActivityProjection projection;
            OwnedTexture texture;
            std::string label;
            std::string tooltip;
            std::vector<std::string> nodeText;
            std::vector<std::pair<std::size_t, Extensions::EditorUiPlotState>> plots;
            std::optional<Extensions::EditorUiRenderSnapshot> render;
            float offeredWidth{320};
            float preparedWidth{};
            bool focusPending{};
            bool requestNativePanelClear{};
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
        /** @brief Retains local plot state only for a matching node in the same provider publication.
         * @param entry Newly prepared entry receiving bounded plot state.
         * @param previous Prior matching provider entry, or null for a fresh publication.
         */
        void PreparePlots(Entry &entry, const Entry *previous) const;
        /** @brief Refreshes copied localization and transfers texture ownership at the owner Update boundary. */
        [[nodiscard]] Entry PrepareEntry(const Extensions::EditorActivityProjection &projection, const std::string &locale);
        /** @brief Prepares candidate resources and opening/focus transitions without changing published texture ownership. */
        void PrepareResources(Entry &entry, const Entry *previous);
        /** @brief Commits prepared entries without allocation, transferring published textures only after all preparation succeeds. */
        void CommitEntries(std::vector<Entry> next) noexcept;
        /** @brief Copies provider-localized node text during the owner update phase. */
        void PrepareNodeText(Entry &entry, const std::string &locale);
        /** @brief Draws the admitted form using retained text and queues typed actions without invoking providers. */
        void DrawForm(Entry &entry) const;
        /** @brief Draws one copied semantic node inside its inherited native input and clipping scopes. */
        void DrawFormNode(Entry &entry, std::size_t index, float width, bool enabled) const;
        /** @brief Allocates layout and semantic snapshots only at owner updates on declaration, width or live-theme change. */
        void PrepareLayout(Entry &entry) const;
        /** @brief Host-owned specialized plot; consumes copied data and current tokens without provider callbacks. */
        void DrawPlot(Entry &entry, const Extensions::EditorUiPlotNode &plot, std::size_t index, bool enabled) const;
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
        EditorPageThemeCapture themeCapture_;
        bool inputAdmitted_{true};
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
