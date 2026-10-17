#include "EditorPageModal.h"

#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/SettingsModal.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/Sha256.h"

#include <format>

namespace Horo::Editor {
    namespace {
        /** @brief Derives a host-local modal identity from the exact provider activation, without persisting a runtime generation. */
        ModalId Identity(const Extensions::EditorSurfaceDescriptor &surface) {
            const auto &p = surface.provider;
            const std::string key = std::format("{}:{}:{}:{}", p.extensionId, p.moduleId, p.activationGeneration, surface.id);
            const auto digest = ComputeSha256(std::as_bytes(std::span{key}));
            std::uint64_t value{};
            for (unsigned i = 0; i < 8; ++i)
                value |= static_cast<std::uint64_t>(digest.bytes[i]) << (8U * i);
            return ModalId{value == 0 ? 1 : value};
        }
    }  // namespace

    /** @copydoc EditorPageModal::EditorPageModal */
    EditorPageModal::EditorPageModal(const EditorGuiContext &context, std::shared_ptr<Extensions::EditorPageHost> host,
                                     const Extensions::EditorSurfaceDescriptor &surface,
                                     std::shared_ptr<const Extensions::EditorThemeFrame> theme)
        : context_(context), host_(std::move(host)), surface_(surface), view_(context, host_, std::move(theme)), id_(Identity(surface)) {
        view_.Select(surface.provider, surface.id);
    }

    /** @copydoc EditorPageModal::Id */
    ModalId EditorPageModal::Id() const {
        return id_;
    }

    /** @copydoc EditorPageModal::Presentation */
    ModalPresentation EditorPageModal::Presentation() const {
        return {.size = ModalSizePolicy::Large, .dimWorkspace = true};
    }

    /** @copydoc EditorPageModal::ClosePolicy */
    ModalClosePolicy EditorPageModal::ClosePolicy() const {
        return {.allowCloseButton = true, .allowEscape = true, .allowOutsideClick = false, .allowApplicationShutdown = true};
    }

    /** @copydoc EditorPageModal::OnOpen */
    Result<void> EditorPageModal::OnOpen(EditorModalContext &context) {
        modals_ = &context.modals;
        if (!view_.Current())
            return Result<void>::Failure(MakeError(Extensions::ExtensionErrors::CapabilityRevoked));
        return Result<void>::Success();
    }

    /** @copydoc EditorPageModal::OnUpdate */
    void EditorPageModal::OnUpdate(float) {
        const auto *page = view_.Current();
        if (modals_ && (!page || page->state == Extensions::EditorPageState::Cancelled ||
                        page->state == Extensions::EditorPageState::Completed || page->state == Extensions::EditorPageState::Revoked))
            static_cast<void>(modals_->RequestClose(id_, page && page->state == Extensions::EditorPageState::Completed
                                                             ? ModalCloseReason::Completed
                                                             : ModalCloseReason::Cancelled));
    }

    /** @copydoc EditorPageModal::Draw */
    ModalFrameResult EditorPageModal::Draw() {
        const auto label = host_->LocalizedText(surface_.provider, surface_.labelLocalizationKey, context_.settings.settings.languageTag);
        Ui::ScopedModalShell shell({.id = surface_.id.c_str(), .title = label.data(), .requestedSize = {640, 520}}, context_.theme.fonts);
        view_.DrawPage();
        for (const auto &child : host_->Prepared())
            if (child.surface.kind == Extensions::EditorSurfaceKind::ModalPage && child.surface.provider == surface_.provider &&
                child.surface.placement.target == surface_.id) {
                const auto childLabel = host_->LocalizedText(child.surface.provider, child.surface.labelLocalizationKey,
                                                             context_.settings.settings.languageTag);
                ImGui::PushID(child.surface.id.c_str());
                if (Ui::Button({.label = childLabel.data()}))
                    static_cast<void>(host_->Begin(child.surface.provider, child.surface.id));
                ImGui::PopID();
            }
        return shell.CloseRequested() ? ModalFrameResult::RequestClose(ModalCloseReason::Cancelled) : ModalFrameResult::None();
    }

    /** @copydoc EditorPageModal::CanClose */
    CloseDecision EditorPageModal::CanClose(ModalCloseReason) {
        return CloseDecision::Allow;
    }

    /** @copydoc EditorPageModal::OnClose */
    void EditorPageModal::OnClose(ModalCloseReason reason) {
        if (reason != ModalCloseReason::Completed)
            view_.Cancel();
        modals_ = nullptr;
    }

    /** @copydoc EditorPageModal::Owns */
    bool EditorPageModal::Owns(const Extensions::EditorSurfaceProviderIdentity &provider, std::string_view id) const noexcept {
        return surface_.provider == provider && surface_.id == id;
    }

    /** @copydoc PumpEditorPageModals */
    void PumpEditorPageModals(const EditorGuiContext &context, const std::shared_ptr<Extensions::EditorPageHost> &pages,
                              std::shared_ptr<const Extensions::EditorThemeFrame> theme, EditorModalHost &modals) {
        if (!pages)
            return;
        for (const auto &page : pages->Prepared()) {
            if (page.state != Extensions::EditorPageState::Editing && page.state != Extensions::EditorPageState::Failed)
                continue;
            if (page.surface.kind == Extensions::EditorSurfaceKind::Modal) {
                if (!modals.HasOpenModal()) {
                    static_cast<void>(modals.OpenRoot(std::make_unique<EditorPageModal>(context, pages, page.surface, theme)));
                    return;
                }
                if (dynamic_cast<SettingsModal *>(modals.TopModal())) {
                    static_cast<void>(modals.PushChild(ModalId{SettingsModal::kModalId},
                                                       std::make_unique<EditorPageModal>(context, pages, page.surface, theme)));
                    return;
                }
            }
            if (page.surface.kind == Extensions::EditorSurfaceKind::ModalPage) {
                const auto *parent = dynamic_cast<EditorPageModal *>(modals.TopModal());
                if (parent && parent->Owns(page.surface.provider, page.surface.placement.target)) {
                    static_cast<void>(
                        modals.PushChild(parent->Id(), std::make_unique<EditorPageModal>(context, pages, page.surface, theme)));
                    return;
                }
            }
        }
    }
}  // namespace Horo::Editor
