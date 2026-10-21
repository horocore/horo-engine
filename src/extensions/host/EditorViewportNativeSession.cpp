#include "EditorViewportNativeSession.h"

#include "EditorActivityAbiConversion.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <array>
#include <exception>

namespace Horo::Extensions {
    /** @copydoc CopyViewportDefinition */
    Result<EditorViewportDefinition> CopyViewportDefinition(const HoroEditorViewportDescriptor &input) {
        if (!HoroEditorViewportDescriptorShape(&input))
            return Result<EditorViewportDefinition>::Failure(MakeError(ExtensionErrors::ContributionRejected));
        EditorViewportDefinition definition;
        definition.tool = static_cast<EditorViewportTool>(input.tool);
        definition.visible = input.visible != 0;
        if (!Detail::CopyActivityText(input.editableType, definition.editableType, 96))
            return Result<EditorViewportDefinition>::Failure(MakeError(ExtensionErrors::ContributionRejected));
        for (std::uint32_t i = 0; i < input.lineCount; ++i) {
            const auto &line = input.lines[i];
            definition.lines.emplace_back(line.x0, line.y0, line.x1, line.y1, line.thickness, static_cast<EditorViewportTone>(line.tone),
                                          static_cast<std::int8_t>(line.axis));
        }
        return Result<EditorViewportDefinition>::Success(std::move(definition));
    }

    /** @brief Native activation staging retains only copied metadata and revocable host admission, never editor/model pointers. */
    struct EditorViewportNativeSession final : IExtensionRetirementContribution {
        std::weak_ptr<EditorViewportHost> host;
        std::weak_ptr<ExtensionRetirement> retirement;
        EditorSurfaceContextProvider contexts;
        std::optional<ExtensionCapabilityAdmission> admission;
        EditorSurfaceContextDescriptor descriptor;
        EditorViewportDefinition definition;
        std::optional<EditorViewportRegistration> registration;
        bool revoked{};

        /** @brief Revokes admission before releasing publication; repeated rollback/retirement is harmless. */
        void Revoke() noexcept override {
            revoked = true;
            if (admission)
                admission->Revoke();
            if (registration)
                registration->Reset();
            contexts.BeginShutdown();
        }

        /** @brief Publishes one inert schema only inside the already owned whole-native activation transaction. */
        Result<void> Commit() {
            const auto live = host.lock();
            const auto code = retirement.lock();
            if (!live || !code || revoked || !admission)
                return Result<void>::Failure(MakeError(ExtensionErrors::CapabilityRevoked));
            const auto capability =
                descriptor.surface.kind == EditorSurfaceKind::ViewportOverlay ? EditorViewportOverlayCapability : EditorGizmoCapability;
            auto grant = admission->Grant(capability);
            if (grant.HasError())
                return Result<void>::Failure(grant.ErrorValue());
            const std::array capabilities{std::move(grant).Value()};
            auto attached = contexts.Attach(descriptor, admission->ActivationLease(), capabilities);
            if (attached.HasError())
                return Result<void>::Failure(attached.ErrorValue());
            auto published = live->Register(std::move(attached).Value(), definition, code);
            if (published.HasError())
                return Result<void>::Failure(published.ErrorValue());
            registration.emplace(std::move(published).Value());
            return Result<void>::Success();
        }
    };

    namespace {
        /** @brief Retires the entire staged publication set if any native viewport commit fails. */
        void RevokeEditorViewports(std::span<const std::shared_ptr<ExtensionModuleLifetime>> lifetimes) noexcept {
            for (const auto &owner : lifetimes)
                for (const auto &published : owner->editorViewports)
                    published->Revoke();
        }

        /** @brief Preserves native registration failure in the enclosing module transaction. */
        HoroExtensionStatus Reject(AssetImporterRegistrationSession &load, const Error &error) {
            load.failed = true;
            load.error = error;
            return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
        }

        /** @brief Copies exact declared ownership and finite allowlists; descriptors themselves remain inert. */
        Result<void> CopyContext(AssetImporterRegistrationSession &load, const HoroEditorViewportDescriptor &input,
                                 EditorViewportNativeSession &session) {
            auto &surface = session.descriptor.surface;
            if (const std::string_view claim = input.kind == 0 ? "editor.viewport_overlay" : "editor.gizmo";
                !Detail::CopyActivityText(input.surfaceId, surface.id) ||
                !Detail::CopyActivityText(input.labelKey, surface.labelLocalizationKey, 128) ||
                !std::ranges::any_of(load.manifest->contributions,
                                     [&](const auto &declared) {
                return declared.type == claim && declared.id == surface.id && declared.owningModule == load.extensionModule->id;
            }) ||
                std::ranges::any_of(load.lifetime->editorViewports, [&](const auto &existing) {
                return existing->descriptor.surface.id == surface.id;
            }))
                return Result<void>::Failure(MakeError(ExtensionErrors::ContributionRejected));
            if (!load.viewportGeneration)
                load.viewportGeneration = EditorViewportHostAccess::Generation(*load.viewportHost);
            if (!load.viewportGeneration)
                return Result<void>::Failure(MakeError(ExtensionErrors::CapabilityRevoked));
            surface.provider = {load.manifest->id, load.extensionModule->id, load.viewportGeneration};
            surface.kind = input.kind == 0 ? EditorSurfaceKind::ViewportOverlay : EditorSurfaceKind::Gizmo;
            surface.placement = {EditorSurfacePlacementKind::Viewport, {}, 0};
            surface.persistence = EditorSurfacePersistence::None;
            surface.openByDefault = false;
            const auto capability = input.kind == 0 ? EditorViewportOverlayCapability : EditorGizmoCapability;
            surface.requiredCapabilities = {capability};
            for (std::uint32_t i = 0; i < input.permissionCount; ++i) {
                std::string permission;
                if (!Detail::CopyActivityText(input.permissions[i], permission, 128))
                    return Result<void>::Failure(MakeError(ExtensionErrors::ContributionRejected));
                surface.requiredPermissions.emplace_back(std::move(permission));
            }
            return Result<void>::Success();
        }

        /** @brief Stages complete validated data and real sealed permission evidence without publishing discovery. */
        Result<std::shared_ptr<EditorViewportNativeSession>> Stage(AssetImporterRegistrationSession &load,
                                                                   const HoroEditorViewportDescriptor &input) {
            auto session = std::make_shared<EditorViewportNativeSession>();
            session->host = load.viewportHost;
            session->retirement = load.retirement;
            if (auto copied = CopyContext(load, input, *session); copied.HasError())
                return Result<std::shared_ptr<EditorViewportNativeSession>>::Failure(copied.ErrorValue());
            auto definition = CopyViewportDefinition(input);
            if (definition.HasError())
                return Result<std::shared_ptr<EditorViewportNativeSession>>::Failure(definition.ErrorValue());
            session->definition = std::move(definition).Value();
            if (auto valid = ValidateEditorViewportDefinition(session->descriptor.surface, session->definition); valid.HasError())
                return Result<std::shared_ptr<EditorViewportNativeSession>>::Failure(valid.ErrorValue());
            const auto &surface = session->descriptor.surface;
            const auto capability =
                surface.kind == EditorSurfaceKind::ViewportOverlay ? EditorViewportOverlayCapability : EditorGizmoCapability;
            auto admitted = ExtensionCapabilityAdmission::Evaluate({surface.provider.extensionId,
                                                                    surface.provider.moduleId,
                                                                    surface.provider.activationGeneration,
                                                                    {{capability, surface.requiredPermissions}}},
                                                                   load.viewportHost->AdmissionPolicy());
            if (admitted.HasError())
                return Result<std::shared_ptr<EditorViewportNativeSession>>::Failure(admitted.ErrorValue());
            session->admission.emplace(std::move(admitted).Value());
            return Result<std::shared_ptr<EditorViewportNativeSession>>::Success(std::move(session));
        }
    }  // namespace

    namespace {
        /** @brief Stages copied native viewport metadata inside the owning registration transaction. */
        HoroExtensionStatus RegisterExternalEditorViewportImpl(AssetImporterRegistrationSession &load,
                                                               const HoroEditorViewportDescriptor *descriptor) noexcept {
            try {
                if (!load.viewportTransportNegotiated || !load.viewportHost || !load.manifest || !load.extensionModule || !load.lifetime ||
                    !load.retirement || load.failed || load.lifetime->ownerThread != std::this_thread::get_id() ||
                    load.lifetime->editorViewports.size() >= 64 || !HoroEditorViewportDescriptorShape(descriptor))
                    return Reject(load, MakeError(ExtensionErrors::ContributionRejected));
                auto staged = Stage(load, *descriptor);
                if (staged.HasError())
                    return Reject(load, staged.ErrorValue());
                auto session = std::move(staged).Value();
                if (!load.retirement->RegisterContribution(load.extensionModule->id, session))
                    return Reject(load, MakeError(ExtensionErrors::CapabilityRevoked));
                load.lifetime->editorViewports.push_back(std::move(session));
                return HORO_EXTENSION_SUCCESS;
            } catch (const std::exception &) {
                load.failed = true;
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
        }

    }  // namespace

    /** @copydoc ValidateEditorViewportClaims */
    Result<void> ValidateEditorViewportClaims(const AssetImporterRegistrationSession &load) {
        if (const auto count = std::ranges::count_if(load.manifest->contributions,
                                                     [&](const auto &claim) {
            return claim.owningModule == load.extensionModule->id &&
                   (claim.type == "editor.viewport_overlay" || claim.type == "editor.gizmo");
        });
            static_cast<std::size_t>(count) != load.lifetime->editorViewports.size())
            return Result<void>::Failure(MakeError(ExtensionErrors::ContributionRejected));
        return Result<void>::Success();
    }

    /** @copydoc CommitEditorViewports */
    Result<void> CommitEditorViewports(std::span<const std::shared_ptr<ExtensionModuleLifetime>> lifetimes) {
        for (const auto &lifetime : lifetimes)
            for (const auto &session : lifetime->editorViewports)
                if (auto committed = session->Commit(); committed.HasError()) {
                    RevokeEditorViewports(lifetimes);
                    return committed;
                }
        return Result<void>::Success();
    }
}  // namespace Horo::Extensions

/** @copydoc RegisterExternalEditorViewport */
extern "C" HoroExtensionStatus RegisterExternalEditorViewport(void *context, const HoroEditorViewportDescriptor *descriptor) noexcept {
    if (!context)
        return HORO_EXTENSION_ERROR_INVALID_ARGS;
    return Horo::Extensions::RegisterExternalEditorViewportImpl(*static_cast<Horo::Extensions::AssetImporterRegistrationSession *>(context),
                                                                descriptor);
}
