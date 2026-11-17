#pragma once

/** @file RuntimeOverlayPanels.h @brief Target-private log/job/Scene/Network development panels; input ownership stays with the parent debug
 * surface. */
#include "ConsoleSceneOverlayInternal.h"
#include "Horo/Debug/ConsolePresentation.h"
#include "Horo/Debug/OverlayLayout.h"
#include "Horo/Editor/Localization/LocalizedGui.h"
#include "NetworkConsoleOverlay.h"
#include "RuntimeConsoleRecordList.h"

namespace Horo::Editor::Theme {
    struct Fonts;
}

namespace Horo::Editor {
    /** @brief Presentation-only owner over existing log/operation/Scene/Network queries; all calls run on the GUI/input owner. */
    class RuntimeOverlayPanels final : public LocalizedGui {
    public:
        /** @brief Registers actual log/job and explicitly bound Scene/Network adapters and copies finite initial panel preferences without
         * querying a source. */
        RuntimeOverlayPanels(const Log::IStructuredLogQuery &logs, const IOperationQuery &operations, Debug::DebugCommandContext principal,
                             bool visible, std::span<const Debug::OverlaySource> requested, bool sceneInspection = false,
                             std::shared_ptr<const Application::INetworkDebuggerQuery> network = {});
        /** @brief Publishes the explicit host Scene view without querying it; null clears copied display rows.
         * @return Success or typed unbound/owner failure; retains no Scene storage.
         */
        [[nodiscard]] Result<void> PublishScene(const Runtime::RuntimeSceneView *scene);
        /** @brief Opens the availability/control hub or closes every panel and drops projections. */
        void SetVisible(bool visible);
        /** @brief Projects a host-accepted registered panel preference, preserving the independent feature/focus gate.
         * @return Success or typed capability/permission failure without altering the accepted preference on rejection.
         */
        [[nodiscard]] Result<void> SetPanelVisibility(Debug::OverlaySource source, bool visible);
        /** @brief Takes one of at most four coalesced registered panel intents; never writes the session variable. */
        [[nodiscard]] std::optional<Debug::OverlayVisibilityIntent> TakeVisibilityRequest() noexcept;
        /** @brief Displays only the stable admission failure code without changing accepted preferences. */
        void ReportRequestFailure(const Error &error);
        /** @brief Drops all cached source projections while preserving requested session presentation. */
        void Suspend();
        /** @brief Observes requested hub/registered panel visibility without querying process stores. */
        [[nodiscard]] bool AnyVisible() const;
        /** @brief Observes the hub's requested visibility for the mapped host toggle. */
        [[nodiscard]] bool Visible() const noexcept;
        /** @brief Refreshes only actually visible sources at the existing four-Hz cutoff and draws localized shared controls.
         * @return Whether Escape requested closing the whole debug surface; no input context is created here.
         */
        [[nodiscard]] bool Draw(const ILocalizationService &localization, const Theme::Fonts &fonts,
                                std::chrono::steady_clock::time_point now);

    private:
        /** @brief Constructs inert fixed metadata for registered real adapters; failure is a programming invariant violation. */
        [[nodiscard]] static Debug::OverlayLayout MakeLayout(bool scene, bool network);
        /** @brief Retains a real query only when explicitly supplied and the current principal admits its read-only overlay. */
        [[nodiscard]] static std::optional<Application::Internal::NetworkConsoleOverlay> MakeNetwork(
            std::shared_ptr<const Application::INetworkDebuggerQuery> query, const Debug::DebugCommandContext &principal);
        /** @brief Admits an inert Scene projection only for an explicit capability and the actual Developer principal. */
        [[nodiscard]] static std::optional<Debug::Internal::ConsoleSceneOverlay> MakeScene(bool bound,
                                                                                           const Debug::DebugCommandContext &principal);
        /** @brief Maps a registered source identity to its one stable layout ID; other sources are unavailable. */
        [[nodiscard]] std::uint64_t PanelId(Debug::OverlaySource source) const noexcept;
        /** @brief Records only a registered local panel intent for the host's common dispatcher. */
        void RequestPanel(Debug::OverlaySource source, bool visible);
        /** @brief Resolves the complete bounded visible layout through current host grants. */
        [[nodiscard]] std::size_t Panels(std::span<Debug::OverlayPanelSnapshot> output) const;
        /** @brief Mirrors registered panel visibility to the read-only projection adapter, without querying. */
        void SynchronizeSources();
        /** @brief Draws available source toggles and explicit unavailable source states, never fake data panels. */
        void DrawHub(const ILocalizationService &localization, const Theme::Fonts &fonts);
        /** @brief Draws one typed source choice, disabling unbound adapters without activating a substitute. */
        void DrawSourceChoice(std::size_t index, const ILocalizationService &localization, const Theme::Fonts &fonts);
        /** @brief Draws one bounded panel and publishes its user layout intent on release. */
        void DrawPanel(const Debug::OverlayPanelSnapshot &panel, const ILocalizationService &localization, const Theme::Fonts &fonts);
        /** @brief Edits display-only severity/category/text/repeat filtering through the shared controls. */
        void DrawLogFilter(const ILocalizationService &localization, const Theme::Fonts &fonts);
        /** @brief Draws/copies only bounded visible fields from the selected real source row. */
        void DrawDetails(bool logs, const ILocalizationService &localization, const Theme::Fonts &fonts);
        /** @brief Refreshes only visible Scene presentation and formats bounded copied identities outside paint. */
        void RefreshScene(std::chrono::steady_clock::time_point now);
        /** @brief Paints/copies the current bounded Scene projection using the shared virtual record primitive. */
        void DrawScene(const ILocalizationService &localization, const Theme::Fonts &fonts);
        /** @brief Paints the bounded current network summary and retained typed refresh failure, without querying networking. */
        void DrawNetwork(const ILocalizationService &localization, const Theme::Fonts &fonts);
        /** @brief Checks Escape only in the current own window when no popup owns it. */
        [[nodiscard]] static bool WantsClose();
        /** @brief Reads actual window intent into finite contained logical viewport fractions. */
        void CaptureLayout(const Debug::OverlayPanelSnapshot &panel);

        Debug::DebugCommandContext principal_;
        std::optional<Debug::Internal::ConsoleSceneOverlay> scene_;
        std::optional<Application::Internal::NetworkConsoleOverlay> network_;
        Debug::OverlayLayout layout_;
        Debug::ConsoleStoreOverlays stores_;
        std::array<bool, static_cast<std::size_t>(Debug::OverlaySource::Count)> requested_{};
        std::array<std::optional<bool>, static_cast<std::size_t>(Debug::OverlaySource::Count)> intents_{};
        std::array<Ui::RuntimeInspectionRecordRow, 256> sceneRows_{};
        std::array<char, 129> category_{};
        std::array<char, 513> text_{};
        std::optional<std::size_t> selectedLog_, selectedJob_, selectedScene_;
        std::uint64_t logRevision_{}, jobRevision_{};
        std::string errorCode_, requestErrorCode_;
        int severity_{}, categoryMatch_{};
        bool collapse_{}, visible_{}, closeRequested_{};
    };
}  // namespace Horo::Editor
