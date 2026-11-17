#pragma once

/** @file RuntimeConsoleView.h @brief Non-installed Editor development terminal over the common runtime console authorities. */
#include "Horo/Debug/ConsolePresentation.h"
#include "Horo/Debug/OverlaySource.h"
#include "Horo/Editor/Localization/LocalizedGui.h"

#include <array>

namespace Horo::Runtime {
    class RuntimeSceneView;
}

namespace Horo::Application {
    class INetworkDebuggerQuery;
}

namespace Horo::Editor::Theme {
    struct Fonts;
}

namespace Horo::Editor::Ui {
    enum class InputTextNavigationAction;
}

namespace Horo::Editor {
    class RuntimeOverlayPanels;

    /** @brief Actual process capabilities borrowed through this shorter-lived development surface. */
    struct RuntimeConsoleViewPorts final {
        Input::InputRouter &input;
        Debug::CommandExecutor &executor;
        Debug::ConsoleHistory &history;
        const Debug::CommandRegistry &registry;
        const IOperationQuery &operations;
        const ConfigurationService &configuration;
        const Log::IStructuredLogQuery &logs;
        Debug::DebugCommandContext principal;         /**< Host-issued grants, never inferred from presentation settings. */
        const Debug::ConsoleVariableSet *variables{}; /**< Same optional dispatcher inventory for permission-filtered completion. */
        bool sceneInspection{};                       /**< Explicit host capability; absence keeps the Scene panel unregistered. */
        std::shared_ptr<const Application::INetworkDebuggerQuery> network; /**< Optional actual immutable network query owner. */
    };

    /** @brief Initial local presentation preferences; construction copies panel choices without retaining the span. */
    struct RuntimeConsoleViewPresentation final {
        bool console{}, overlays{};
        std::span<const Debug::OverlaySource> panels;
    };

    /** @brief Transient development GUI surface; stores only editing/selection state and owns no diagnostic authority.
     * All calls and destruction run on the Input/history owner. The dispatcher, history, input, operation and configuration
     * authorities outlive this surface. Construction creates no input token or native resource.
     */
    class RuntimeConsoleView final : public LocalizedGui {
    public:
        /** @brief Binds the actual host capabilities without drawing or acquiring focus.
         * @param ports Actual host capabilities, all outliving the surface.
         * @param presentation Explicit local presentation request; grants no command permissions.
         */
        RuntimeConsoleView(const RuntimeConsoleViewPorts &ports, const RuntimeConsoleViewPresentation &presentation);
        /** @brief Releases the terminal input lease before borrowed host owners are destroyed. */
        ~RuntimeConsoleView();
        /** @brief Copies the actual host's weak Scene view into the admitted read-only panel projection.
         * @param scene Owner-cutoff view or null when absent. @return Success or typed unbound/owner failure.
         * Neither publication nor hidden presentation queries Scene storage.
         */
        [[nodiscard]] Result<void> PublishScene(const Runtime::RuntimeSceneView *scene);
        /** @brief Projects host-accepted visibility; closing clears unsent editing state and releases focus.
         * @param visible Effective host session request, independent of gameplay authority.
         */
        void SetVisible(bool visible);
        /** @brief Takes a terminal close intent once for the host's common dispatcher; drawing changes no session control state.
         * @return Requested visibility, or no UI intent. Rejected admission leaves effective presentation unchanged.
         */
        [[nodiscard]] std::optional<bool> TakeVisibilityRequest() noexcept;
        /** @brief Presents only a rejected request's stable code without changing effective presentation. */
        void ReportRequestFailure(const Error &error);
        /** @brief Presents only a rejected panel intent's stable code in its control hub, independently of source refresh failures. */
        void ReportPanelRequestFailure(const Error &error);
        /** @brief Changes the overlay feature's local presentation; closing drops all read-only projections. */
        void SetOverlaysVisible(bool visible);
        /** @brief Projects a host-accepted registered process-store panel preference; source/focus queries remain visibility-gated. */
        [[nodiscard]] Result<void> SetPanelVisibility(Debug::OverlaySource source, bool visible);
        /** @brief Takes at most two coalesced panel intents for the host's common dispatcher. */
        [[nodiscard]] std::optional<Debug::OverlayVisibilityIntent> TakePanelVisibilityRequest() noexcept;
        /** @brief Reports overlay control-hub visibility for the host's configurable toggle. */
        [[nodiscard]] bool OverlaysVisible() const noexcept;
        /** @brief Reports any requested terminal or overlay surface without querying stores. */
        [[nodiscard]] bool AnyVisible() const;
        /** @brief Synchronizes input before gameplay routing; never steals an existing modal/native focus lease.
         * @param blocked Actual host modal, focus-loss or suspended presentation state.
         */
        void SynchronizeFocus(bool blocked);
        /** @brief Composes shared controls and bounded virtual history over the already-rendered editor/game view.
         * @param localization Current locale authority. @param fonts Current shared typography. @param blocked Actual host barrier.
         * @param now Explicit host monotonic presentation time for bounded source refresh, independent of simulation pause.
         * This development GUI uses the host RenderGui path, not gameplay Runtime UI overlay permissions.
         */
        void Draw(const ILocalizationService &localization, const Theme::Fonts &fonts, bool blocked,
                  std::chrono::steady_clock::time_point now);
        /** @brief Reports requested visibility without acquiring focus. @return Local presentation intent. */
        [[nodiscard]] bool Visible() const noexcept;
        /** @brief Reports central input eligibility, including newer modal/native barriers. @return Whether this terminal can submit. */
        [[nodiscard]] bool Interactive() const noexcept;

    private:
        /** @brief Records a local close intent without changing effective presentation or variable authority. */
        void RequestVisible(bool visible);
        /** @brief Handles only synchronous native completion/history requests; replacement bytes belong to this surface. */
        static std::optional<std::string_view> Navigate(void *context, Ui::InputTextNavigationAction action, std::string_view text,
                                                        std::size_t cursor);
        /** @brief Copies one complete bounded recalled line; never replays redacted placeholders or truncates a command. */
        [[nodiscard]] std::optional<std::string_view> Recall(std::string_view command);
        /** @brief Uses filtered immutable discovery only on explicit Tab; completes names at the end of the first token. */
        [[nodiscard]] std::optional<std::string_view> Complete(std::string_view text, std::size_t cursor);
        /** @brief Saves the current draft once and finds an older complete non-redacted command. */
        [[nodiscard]] std::optional<std::string_view> Previous(std::string_view text);
        /** @brief Finds a newer non-redacted command or restores the unsent draft at the end of history navigation. */
        [[nodiscard]] std::optional<std::string_view> Next();
        /** @brief Draws the themed viewport dim without taking a second input lease. */
        static void DrawBackdrop();
        /** @brief Draws one localized editor/control frame after the host admitted presentation. */
        void DrawContents(const ILocalizationService &localization, const Theme::Fonts &fonts);
        /** @brief Draws bounded reverse-search controls without retaining source row pointers. */
        void DrawSearch(const ILocalizationService &localization, const Theme::Fonts &fonts);
        /** @brief Draws/copies the selected safe result, invalidated on history mutation. */
        void DrawDetails(const ILocalizationService &localization, const Theme::Fonts &fonts);
        /** @brief Submits only through ConsoleTerminal and retains only safe stable error identifiers on rejection. */
        void Submit();

        Input::InputRouter &input_;
        Debug::ConsoleHistory &history_;
        const Debug::CommandRegistry &registry_;
        const Debug::ConsoleVariableSet *variables_{};
        Debug::DebugCommandContext principal_;
        Debug::ConsoleTerminal terminal_;
        std::unique_ptr<RuntimeOverlayPanels> panels_;
        std::array<char, 4097> line_{}, draft_{}, replacement_{};
        std::array<char, 513> search_{};
        std::size_t capacity_{};
        std::optional<std::size_t> selected_, recallOffset_;
        std::optional<bool> visibilityRequest_;
        std::uint64_t revision_{};
        std::string errorCode_, completion_;
        bool visible_{}, lease_{}, requestFocus_{}, navigating_{}, searchVisible_{};
    };
}  // namespace Horo::Editor
