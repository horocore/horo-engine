#include "EditorActivityAbiConversion.h"
#include "EditorPageNativeSessionState.h"

#include <algorithm>
#include <ranges>

namespace Horo::Extensions {
    namespace {
        /** @brief Admits only owner-lane operations on a committed exact native activation. */
        HoroExtensionStatus Open(void *context) noexcept {
            if (!context)
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            auto &session = *static_cast<EditorPageNativeSession *>(context);
            try {
                const auto host = session.host.lock();
                if (!host || session.revoked || !session.committed || session.owner != std::this_thread::get_id())
                    return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
                return host->Begin(session.descriptor.surface.provider, session.descriptor.surface.id).HasValue()
                           ? HORO_EXTENSION_SUCCESS
                           : HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            } catch (...) {
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
        }

        /** @brief Cancels only the exact admitted native contribution, preserving uncertain persistence recovery state. */
        HoroExtensionStatus Close(void *context) noexcept {
            if (!context)
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            auto &session = *static_cast<EditorPageNativeSession *>(context);
            try {
                const auto host = session.host.lock();
                if (!host || session.revoked || !session.committed || session.owner != std::this_thread::get_id())
                    return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
                return host->Cancel(session.descriptor.surface.provider, session.descriptor.surface.id).HasValue()
                           ? HORO_EXTENSION_SUCCESS
                           : HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            } catch (...) {
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
        }

        /** @brief Maps the closed native page vocabulary to actual manifest contribution identities. */
        std::string_view Claim(uint32_t kind) noexcept {
            switch (kind) {
                case HORO_EDITOR_PAGE_SETTINGS:
                    return "editor.settings_page";
                case HORO_EDITOR_PAGE_MODAL:
                    return "editor.modal";
                case HORO_EDITOR_PAGE_MODAL_CHILD:
                    return "editor.modal_page";
                default:
                    return {};
            }
        }

        /** @brief Copies exact inert surface ownership and rejects undeclared or duplicate native page requests. */
        Result<void> Descriptor(AssetImporterRegistrationSession &load, const HoroEditorPageDescriptor &input,
                                EditorPageNativeSession &session) {
            auto &surface = session.descriptor.surface;
            if (!Detail::CopyActivityText(input.surfaceId, surface.id) ||
                !Detail::CopyActivityText(input.labelKey, surface.labelLocalizationKey, 128) || Claim(input.kind).empty() ||
                !std::ranges::any_of(load.manifest->contributions,
                                     [&](const auto &claim) {
                return claim.type == Claim(input.kind) && claim.id == surface.id && claim.owningModule == load.extensionModule->id;
            }) ||
                std::ranges::any_of(load.lifetime->editorPages, [&](const auto &existing) {
                return existing->descriptor.surface.id == surface.id;
            }))
                return EditorPageDetail::Failure();
            if (load.pageGeneration == 0)
                load.pageGeneration = EditorPageHostAccess::Generation(*load.pageHost);
            if (load.pageGeneration == 0)
                return EditorPageDetail::Failure();
            surface.provider = {load.manifest->id, load.extensionModule->id, load.pageGeneration};
            surface.kind = input.kind == HORO_EDITOR_PAGE_SETTINGS ? EditorSurfaceKind::SettingsPage
                           : input.kind == HORO_EDITOR_PAGE_MODAL  ? EditorSurfaceKind::Modal
                                                                   : EditorSurfaceKind::ModalPage;
            surface.placement = {input.kind == HORO_EDITOR_PAGE_SETTINGS ? EditorSurfacePlacementKind::Settings
                                                                         : EditorSurfacePlacementKind::Modal,
                                 {},
                                 0};
            if (!Detail::CopyActivityText(input.parentSurfaceId, surface.placement.target) ||
                (input.kind == HORO_EDITOR_PAGE_MODAL_CHILD) != !surface.placement.target.empty())
                return EditorPageDetail::Failure();
            surface.persistence = EditorSurfacePersistence::None;
            surface.openByDefault = false;
            return Detail::CopyPagePermissions(input, session.descriptor);
        }

        /** @brief Prepares only copied inert data and capability evidence; the surface registry remains unchanged until Commit. */
        Result<std::shared_ptr<EditorPageNativeSession>> Stage(AssetImporterRegistrationSession &load,
                                                               const HoroEditorPageDescriptor &input) {
            auto session = std::make_shared<EditorPageNativeSession>();
            session->host = load.pageHost;
            session->retirement = load.retirement;
            if (auto copied = Descriptor(load, input, *session); copied.HasError())
                return Result<std::shared_ptr<EditorPageNativeSession>>::Failure(copied.ErrorValue());
            auto definition =
                Detail::CopyPageDefinition(input, session->descriptor.surface.id, session->descriptor.surface.labelLocalizationKey);
            if (definition.HasError())
                return Result<std::shared_ptr<EditorPageNativeSession>>::Failure(definition.ErrorValue());
            session->definition = std::move(definition).Value();
            if (auto valid = ValidateEditorPageDefinition(session->descriptor.surface, session->definition); valid.HasError())
                return Result<std::shared_ptr<EditorPageNativeSession>>::Failure(valid.ErrorValue());
            for (const auto &field : session->definition.fields)
                session->descriptor.state.push_back({field.binding.value});
            for (const auto &message : session->definition.messages)
                session->descriptor.localization.push_back({message.key});
            for (const auto &node : session->definition.form.nodes)
                if (const auto *action = std::get_if<EditorUiActionNode>(&node.payload))
                    session->descriptor.commands.push_back({action->action.value});
            auto &keys = session->descriptor.localization;
            std::ranges::sort(keys, {}, &EditorSurfaceLocalizationKey::value);
            keys.erase(std::ranges::unique(keys).begin(), keys.end());
            const auto capability = input.kind == HORO_EDITOR_PAGE_SETTINGS ? EditorSettingsPageCapability : EditorModalPageCapability;
            session->descriptor.surface.requiredCapabilities = {capability};
            auto admission = ExtensionCapabilityAdmission::Evaluate({load.manifest->id,
                                                                     load.extensionModule->id,
                                                                     load.pageGeneration,
                                                                     {{capability, session->descriptor.surface.requiredPermissions}}},
                                                                    load.pageHost->AdmissionPolicy());
            if (admission.HasError())
                return Result<std::shared_ptr<EditorPageNativeSession>>::Failure(admission.ErrorValue());
            session->admission.emplace(std::move(admission).Value());
            session->operations = Detail::MakeNativePageOperations(input);
            return Result<std::shared_ptr<EditorPageNativeSession>>::Success(std::move(session));
        }

        /** @brief Rolls back all staged/committed pages in the current module batch before native teardown can begin. */
        void Revoke(std::span<const std::shared_ptr<ExtensionModuleLifetime>> lifetimes) noexcept {
            for (const auto &lifetime : lifetimes)
                for (const auto &page : lifetime->editorPages)
                    page->Revoke();
        }
    }  // namespace

    /** @copydoc RegisterExternalEditorPage */
    HoroExtensionStatus RegisterExternalEditorPage(void *context, const HoroEditorPageDescriptor *input,
                                                   HoroEditorPageSessionApi *api) noexcept {
        if (!context || !input || !api || input->structSize < sizeof(*input) || api->structSize < sizeof(*api))
            return HORO_EXTENSION_ERROR_INVALID_ARGS;
        auto &load = *static_cast<AssetImporterRegistrationSession *>(context);
        try {
            if (!load.pageTransportNegotiated || !load.pageHost || !load.manifest || !load.extensionModule || !load.lifetime ||
                !load.retirement || !input->invoke || load.lifetime->ownerThread != std::this_thread::get_id() ||
                load.lifetime->editorPages.size() >= 64 ||
                std::ranges::find(load.extensionModule->requiredCapabilities, HORO_EDITOR_PAGE_HOST_CAPABILITY) ==
                    load.extensionModule->requiredCapabilities.end())
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            auto staged = Stage(load, *input);
            if (staged.HasError()) {
                load.failed = true;
                load.error = staged.ErrorValue();
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
            const auto page = std::move(staged).Value();
            if (!load.retirement->RegisterContribution(load.extensionModule->id, page))
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            load.lifetime->editorPages.push_back(page);
            *api = {sizeof(*api), HORO_EDITOR_PAGE_SESSION_VERSION, page.get(), Open, Close};
            return HORO_EXTENSION_SUCCESS;
        } catch (...) {
            load.failed = true;
            return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
        }
    }

    /** @copydoc CommitEditorPages */
    Result<void> CommitEditorPages(std::span<const std::shared_ptr<ExtensionModuleLifetime>> lifetimes) {
        try {
            for (const auto &lifetime : lifetimes)
                for (const auto &page : lifetime->editorPages) {
                    if (auto committed = page->Commit(); committed.HasError()) {
                        Revoke(lifetimes);
                        return committed;
                    }
                }
        } catch (...) {
            Revoke(lifetimes);
            return EditorPageDetail::Failure(ExtensionErrors::ContributionRejected);
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidateEditorPageClaims */
    Result<void> ValidateEditorPageClaims(const AssetImporterRegistrationSession &registration) {
        const auto count = std::ranges::count_if(registration.manifest->contributions, [&](const auto &claim) {
            return claim.owningModule == registration.extensionModule->id &&
                   (claim.type == "editor.settings_page" || claim.type == "editor.modal" || claim.type == "editor.modal_page");
        });
        if (static_cast<std::size_t>(count) != registration.lifetime->editorPages.size())
            return EditorPageDetail::Failure(ExtensionErrors::ContributionRejected);
        return Result<void>::Success();
    }
}  // namespace Horo::Extensions
