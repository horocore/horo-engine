#include "ExtensionRecoveryView.h"

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"

#include <imgui.h>

namespace Horo::Editor {
    namespace {
        /** @brief Maps only host-owned availability evidence to localized recovery copy. */
        std::string_view ReasonKey(const Extensions::EditorSurfaceProviderStatus status) noexcept {
            using enum Extensions::EditorSurfaceProviderStatus;
            switch (status) {
                case Disabled:
                    return "workspace.extensions.disabled";
                case Incompatible:
                    return "workspace.extensions.incompatible";
                case Failed:
                    return "workspace.extensions.failed";
                default:
                    return "workspace.extensions.missing";
            }
        }
    }  // namespace

    /** @copydoc ExtensionRecoveryView::ExtensionRecoveryView */
    ExtensionRecoveryView::ExtensionRecoveryView(const EditorGuiContext &context, Extensions::EditorSurfaceRegistry *registry) noexcept
        : context_(context), registry_(registry) {}

    /** @copydoc ExtensionRecoveryView::Update */
    void ExtensionRecoveryView::Update() {
        if (!registry_)
            return;
        const auto revision = registry_->Revision();
        const auto &locale = context_.settings.settings.languageTag;
        if (revision_ == revision && locale_ == locale)
            return;
        std::vector<Entry> next;
        for (const auto &surface : registry_->RecoverySnapshot()) {
            if (!surface.intent.open)
                continue;
            next.push_back({surface.intent.surfaceId, surface.intent.provider.extensionId + " / " + surface.intent.provider.moduleId,
                            surface.intent.provider, std::string{context_.localization.Get("editor", ReasonKey(surface.status))}});
        }
        title_ = context_.localization.Get("editor", "workspace.extensions.recovery") + "###ExtensionRecovery";
        explanation_ = context_.localization.Get("editor", "workspace.extensions.retained");
        close_ = context_.localization.Get("editor", "workspace.extensions.close");
        entries_ = std::move(next);
        locale_ = locale;
        revision_ = revision;
    }

    /** @copydoc ExtensionRecoveryView::Draw */
    void ExtensionRecoveryView::Draw() const {
        if (entries_.empty() || !registry_ || registry_->Revision() != revision_)
            return;
        ImGui::SetNextWindowSize({Ui::ScaledLayoutValue(440.0F), Ui::ScaledLayoutValue(280.0F)}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin(title_.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, explanation_, Theme::Muted(), context_.theme.fonts);
            for (const auto &entry : entries_) {
                ImGui::PushID(entry.id.c_str());
                Ui::SectionTitle(entry.id.c_str(), context_.theme.fonts);
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, entry.provider, Theme::Muted(), context_.theme.fonts);
                Ui::WrappedText(ImGui::GetContentRegionAvail().x, entry.reason, Theme::Muted(), context_.theme.fonts);
                if (Ui::Button({.label = close_.c_str(), .font = context_.theme.fonts.sans}))
                    static_cast<void>(registry_->CloseProviderWorkspace(entry.providerKey));
                ImGui::PopID();
            }
        }
        ImGui::End();
    }
}  // namespace Horo::Editor
