#include "editor/screens/workspace/panels/global_dock/panes/platform/PlatformDiagnosticsPane.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"

#include <algorithm>
#include <array>
#include <format>

namespace Horo::Editor {
    namespace {
        /** @brief Projects only known Horo product profiles into localizable labels. */
        [[nodiscard]] const char *ProfileKey(const PlatformServices::PlatformServicesProductProfile profile) noexcept {
            using enum PlatformServices::PlatformServicesProductProfile;
            switch (profile) {
                case Editor:
                    return "profile.editor";
                case DevelopmentGame:
                    return "profile.development";
                case ShippingGame:
                    return "profile.shipping";
                case Headless:
                    return "profile.headless";
                case DedicatedServer:
                    return "profile.server";
                case Test:
                    return "profile.test";
                case UnsupportedPlatform:
                case Count:
                    return "profile.unsupported";
            }
            return "profile.unsupported";
        }

        /** @brief Distinguishes explicit Null and absence without implying a provider fallback. */
        [[nodiscard]] const char *ModeKey(const PlatformServices::PlatformServicesCompositionMode mode) noexcept {
            using enum PlatformServices::PlatformServicesCompositionMode;
            switch (mode) {
                case Null:
                    return "mode.null";
                case Test:
                    return "mode.test";
                case ExactProvider:
                    return "mode.exact";
                case Absent:
                    return "mode.absent";
            }
            return "unavailable";
        }
    }  // namespace

    /** @copydoc PlatformDiagnosticsPane::Draw */
    void PlatformDiagnosticsPane::Draw(const GlobalDockPaneDrawContext &context) {
        const auto &fonts = context.gui.theme.fonts;
        const auto text = [&](const char *key) -> const std::string & {
            return kTextScope.Text(context.gui.localization, key);
        };
        const float width = std::max(1.0F, context.contentWidth);
        ImGui::SetCursorScreenPos(context.contentOrigin);
        Ui::SectionTitle(text("title").c_str(), fonts);
        if (diagnostics_ == nullptr) {
            Ui::WrappedText(width, text("unavailable"), Theme::Muted(), fonts);
            return;
        }
        const auto queried = diagnostics_->Query();
        if (queried.HasError()) {
            Ui::WrappedText(width, text("unavailable"), Theme::Muted(), fonts);
            return;
        }
        const auto &snapshot = queried.Value();
        if (snapshot.composition.generation != observedProvider_ || snapshot.queueGeneration != observedQueue_) {
            commandFailed_ = false;
            observedProvider_ = snapshot.composition.generation;
            observedQueue_ = snapshot.queueGeneration;
        }
        Ui::WrappedText(width, text("privacy"), Theme::Muted(), fonts);
        Ui::WrappedText(width, std::format("{}: {}", text("profile"), text(ProfileKey(snapshot.composition.profile))), Theme::Text(),
                        fonts);
        Ui::WrappedText(width, std::format("{}: {}", text("selection"), text(ModeKey(snapshot.composition.mode))), Theme::Text(), fonts);
        constexpr std::array serviceKeys{"service.achievements", "service.stats",   "service.cloud",
                                         "service.presence",     "service.friends", "service.session"};
        static_assert(serviceKeys.size() == static_cast<std::size_t>(PlatformServices::PlatformServiceKind::Count));
        for (std::size_t index = 0; index < serviceKeys.size(); ++index) {
            const char *availability = "service.unavailable";
            if (snapshot.composition.services[index] == PlatformServices::PlatformServiceAvailability::Available)
                availability = "service.available";
            Ui::WrappedText(width, std::format("{}: {}", text(serviceKeys[index]), text(availability)), Theme::Text(), fonts);
        }
        Ui::WrappedText(width, std::format("{}: {}/{}", text("active"), snapshot.offline.active, snapshot.offline.activeCapacity),
                        Theme::Text(), fonts);
        Ui::WrappedText(width, std::format("{}: {}/{}", text("retained"), snapshot.offline.retained, snapshot.offline.retainedCapacity),
                        Theme::Text(), fonts);
        constexpr std::array keys{"pending", "dispatching", "reconciling", "suspended", "succeeded",
                                  "failed",  "expired",     "superseded",  "abandoned"};
        static_assert(keys.size() == static_cast<std::size_t>(PlatformServices::PlatformOfflineIntentState::Abandoned) + 1);
        for (std::size_t index = 0; index < keys.size(); ++index)
            Ui::WrappedText(width, std::format("{}: {}", text(keys[index]), snapshot.offline.receiptsByState[index]), Theme::Text(), fonts);
        Ui::WrappedText(width, text("queued_notice"), Theme::Muted(), fonts);
        if (Ui::Button({.label = text("suspend").c_str(),
                        .size = {width / Theme::GetActiveTokens().sizes.uiScale, 0},
                        .enabled = snapshot.canSuspendPending,
                        .font = fonts.sans})) {
            const auto suspended = diagnostics_->SuspendPending(snapshot.composition.generation, snapshot.queueGeneration);
            commandFailed_ = suspended.HasError();
        }
        if (commandFailed_)
            Ui::WrappedText(width, text("command_failed"), Theme::Muted(), fonts);
    }
}  // namespace Horo::Editor
