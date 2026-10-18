#include "EditorInspectorNativeSession.h"

#include "EditorActivityAbiConversion.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <exception>

namespace Horo::Extensions {
    /** @brief Native activation staging retains only copied metadata and revocable host admission, never editor/model pointers. */
    struct EditorInspectorNativeSession final : IExtensionRetirementContribution {
        std::weak_ptr<EditorInspectorHost> host;
        std::weak_ptr<ExtensionRetirement> retirement;
        EditorSurfaceContextProvider contexts;
        std::optional<ExtensionCapabilityAdmission> admission;
        EditorSurfaceContextDescriptor descriptor;
        EditorInspectorDefinition definition;
        std::optional<EditorInspectorRegistration> registration;
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
                descriptor.surface.kind == EditorSurfaceKind::Inspector ? EditorInspectorCapability : EditorPropertyDrawerCapability;
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
        /** @brief Retires the entire staged publication set if any native inspector commit fails. */
        void RevokeEditorInspectors(std::span<const std::shared_ptr<ExtensionModuleLifetime>> lifetimes) noexcept {
            for (const auto &owner : lifetimes)
                for (const auto &published : owner->editorInspectors)
                    published->Revoke();
        }

        /** @brief Preserves native registration failure in the enclosing module transaction. */
        HoroExtensionStatus Reject(AssetImporterRegistrationSession &load, const Error &error) {
            load.failed = true;
            load.error = error;
            return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
        }

        /** @brief Copies exact declared ownership and finite allowlists; descriptors themselves remain inert. */
        Result<void> CopyContext(AssetImporterRegistrationSession &load, const HoroEditorInspectorDescriptor &input,
                                 EditorInspectorNativeSession &session) {
            auto &surface = session.descriptor.surface;
            if (const std::string_view claim = input.kind == HORO_EDITOR_INSPECTOR ? "editor.inspector" : "editor.property_drawer";
                !Detail::CopyActivityText(input.surfaceId, surface.id) ||
                !Detail::CopyActivityText(input.labelKey, surface.labelLocalizationKey, 128) ||
                !std::ranges::any_of(load.manifest->contributions,
                                     [&](const auto &declared) {
                return declared.type == claim && declared.id == surface.id && declared.owningModule == load.extensionModule->id;
            }) ||
                std::ranges::any_of(load.lifetime->editorInspectors, [&](const auto &existing) {
                return existing->descriptor.surface.id == surface.id;
            }))
                return Result<void>::Failure(MakeError(ExtensionErrors::ContributionRejected));
            if (!load.inspectorGeneration)
                load.inspectorGeneration = EditorInspectorHostAccess::Generation(*load.inspectorHost);
            if (!load.inspectorGeneration)
                return Result<void>::Failure(MakeError(ExtensionErrors::CapabilityRevoked));
            surface.provider = {load.manifest->id, load.extensionModule->id, load.inspectorGeneration};
            surface.kind = input.kind == HORO_EDITOR_INSPECTOR ? EditorSurfaceKind::Inspector : EditorSurfaceKind::PropertyDrawer;
            surface.placement = {EditorSurfacePlacementKind::Inspector, {}, 0};
            surface.persistence = EditorSurfacePersistence::None;
            surface.openByDefault = false;
            const auto capability = input.kind == HORO_EDITOR_INSPECTOR ? EditorInspectorCapability : EditorPropertyDrawerCapability;
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
        Result<std::shared_ptr<EditorInspectorNativeSession>> Stage(AssetImporterRegistrationSession &load,
                                                                    const HoroEditorInspectorDescriptor &input) {
            auto session = std::make_shared<EditorInspectorNativeSession>();
            session->host = load.inspectorHost;
            session->retirement = load.retirement;
            if (auto copied = CopyContext(load, input, *session); copied.HasError())
                return Result<std::shared_ptr<EditorInspectorNativeSession>>::Failure(copied.ErrorValue());
            auto definition = CopyInspectorDefinition(input);
            if (definition.HasError())
                return Result<std::shared_ptr<EditorInspectorNativeSession>>::Failure(definition.ErrorValue());
            session->definition = std::move(definition).Value();
            if (auto valid = ValidateEditorInspectorDefinition(session->descriptor.surface, session->definition); valid.HasError())
                return Result<std::shared_ptr<EditorInspectorNativeSession>>::Failure(valid.ErrorValue());
            for (const auto &message : session->definition.messages)
                if (std::ranges::find(session->descriptor.localization, EditorSurfaceLocalizationKey{message.key}) ==
                    session->descriptor.localization.end())
                    session->descriptor.localization.emplace_back(message.key);
            const auto &surface = session->descriptor.surface;
            const auto capability =
                surface.kind == EditorSurfaceKind::Inspector ? EditorInspectorCapability : EditorPropertyDrawerCapability;
            auto admitted = ExtensionCapabilityAdmission::Evaluate({surface.provider.extensionId,
                                                                    surface.provider.moduleId,
                                                                    surface.provider.activationGeneration,
                                                                    {{capability, surface.requiredPermissions}}},
                                                                   load.inspectorHost->AdmissionPolicy());
            if (admitted.HasError())
                return Result<std::shared_ptr<EditorInspectorNativeSession>>::Failure(admitted.ErrorValue());
            session->admission.emplace(std::move(admitted).Value());
            return Result<std::shared_ptr<EditorInspectorNativeSession>>::Success(std::move(session));
        }
    }  // namespace

    namespace {
        /** @brief Stages copied native inspector metadata inside the owning registration transaction. */
        HoroExtensionStatus RegisterExternalEditorInspectorImpl(void *context, const HoroEditorInspectorDescriptor *descriptor) noexcept {
            if (!context)
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            auto &load = *static_cast<AssetImporterRegistrationSession *>(context);
            try {
                if (!load.inspectorTransportNegotiated || !load.inspectorHost || !load.manifest || !load.extensionModule ||
                    !load.lifetime || !load.retirement || load.failed || load.lifetime->ownerThread != std::this_thread::get_id() ||
                    load.lifetime->editorInspectors.size() >= 64 || !HoroEditorInspectorDescriptorShape(descriptor))
                    return Reject(load, MakeError(ExtensionErrors::ContributionRejected));
                auto staged = Stage(load, *descriptor);
                if (staged.HasError())
                    return Reject(load, staged.ErrorValue());
                auto session = std::move(staged).Value();
                if (!load.retirement->RegisterContribution(load.extensionModule->id, session))
                    return Reject(load, MakeError(ExtensionErrors::CapabilityRevoked));
                load.lifetime->editorInspectors.push_back(std::move(session));
                return HORO_EXTENSION_SUCCESS;
            } catch (const std::exception &) {
                load.failed = true;
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            }
        }

    }  // namespace

    /** @copydoc ValidateEditorInspectorClaims */
    Result<void> ValidateEditorInspectorClaims(const AssetImporterRegistrationSession &load) {
        if (const auto count = std::ranges::count_if(load.manifest->contributions,
                                                     [&](const auto &claim) {
            return claim.owningModule == load.extensionModule->id &&
                   (claim.type == "editor.inspector" || claim.type == "editor.property_drawer");
        });
            static_cast<std::size_t>(count) != load.lifetime->editorInspectors.size())
            return Result<void>::Failure(MakeError(ExtensionErrors::ContributionRejected));
        return Result<void>::Success();
    }

    /** @copydoc CommitEditorInspectors */
    Result<void> CommitEditorInspectors(std::span<const std::shared_ptr<ExtensionModuleLifetime>> lifetimes) {
        for (const auto &lifetime : lifetimes)
            for (const auto &session : lifetime->editorInspectors)
                if (auto committed = session->Commit(); committed.HasError()) {
                    RevokeEditorInspectors(lifetimes);
                    return committed;
                }
        return Result<void>::Success();
    }
}  // namespace Horo::Extensions

/** @copydoc RegisterExternalEditorInspector */
extern "C" HoroExtensionStatus RegisterExternalEditorInspector(void *context, const HoroEditorInspectorDescriptor *descriptor) noexcept {
    return Horo::Extensions::RegisterExternalEditorInspectorImpl(context, descriptor);
}
