#pragma once

/** @file PlatformDiagnosticsPane.h
 * @brief Localized aggregate-only Platform Services diagnostics presentation.
 */

#include "Horo/Editor/IGlobalDockPane.h"
#include "Horo/PlatformServices/PlatformDiagnostics.h"

namespace Horo::Editor {
    /** @brief Presentation client; owns no service state, provider lease, SDK entry point or metric history. */
    class PlatformDiagnosticsPane final : public IGlobalDockPane {
    public:
        static constexpr GuiLocalizationScope kTextScope{"workspace.global_dock.platform"};

        /** @brief Initializes the required localization scope. */
        PlatformDiagnosticsPane() : IGlobalDockPane(kTextScope) {}

        [[nodiscard]] std::string_view Id() const noexcept override {
            return "horo.global_dock.platform";
        }

        [[nodiscard]] std::string_view LabelKey() const noexcept override {
            return "workspace.global_dock.tab.platform";
        }

        /** @copydoc IGlobalDockPane::Attach */
        void Attach(PanelContext &context) override {
            diagnostics_ = context.platformDiagnostics;
            commandFailed_ = false;
            observedProvider_ = {};
            observedQueue_ = {};
        }

        /** @copydoc IGlobalDockPane::Detach */
        void Detach() override {
            diagnostics_ = nullptr;
            commandFailed_ = false;
            observedProvider_ = {};
            observedQueue_ = {};
        }

        /** @copydoc IGlobalDockPane::Draw */
        void Draw(const GlobalDockPaneDrawContext &context) override;

    private:
        PlatformServices::IPlatformDiagnostics *diagnostics_{};
        bool commandFailed_{};
        PlatformServices::PlatformProviderGeneration observedProvider_;
        PlatformServices::PlatformOfflineQueueGeneration observedQueue_;
    };
}  // namespace Horo::Editor
