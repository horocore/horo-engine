#pragma once
#include "EditorPageHostInternal.h"
#include "EditorPageNativeSession.h"

namespace Horo::Extensions {
    /** @brief Staged native adapter owns copied inert data and revocable capability evidence, never a library or an implicit host service.
     */
    struct EditorPageNativeSession final : IExtensionRetirementContribution {
        std::weak_ptr<EditorPageHost> host;
        std::weak_ptr<ExtensionRetirement> retirement;
        EditorSurfaceContextProvider contexts;
        std::optional<ExtensionCapabilityAdmission> admission;
        EditorSurfaceContextDescriptor descriptor;
        EditorPageDefinition definition;
        std::shared_ptr<IEditorPageOperations> operations;
        std::optional<EditorPageRegistration> registration;
        const std::thread::id owner{std::this_thread::get_id()};
        bool revoked{};
        bool committed{};

        /** @brief Closes input/context and cancels work before provider native teardown. */
        void Revoke() noexcept override {
            revoked = true;
            committed = false;
            if (admission)
                admission->Revoke();
            if (registration)
                registration->Reset();
            contexts.BeginShutdown();
            operations.reset();
        }

        /** @brief Publishes the restricted context and controller using the already admitted real host authorities. */
        Result<void> Commit() {
            const auto live = host.lock();
            const auto code = retirement.lock();
            if (!live || !code || revoked || !admission || owner != std::this_thread::get_id())
                return EditorPageDetail::Failure();
            const auto capability =
                descriptor.surface.kind == EditorSurfaceKind::SettingsPage ? EditorSettingsPageCapability : EditorModalPageCapability;
            auto granted = admission->Grant(capability);
            if (granted.HasError())
                return Result<void>::Failure(granted.ErrorValue());
            const std::array capabilities{std::move(granted).Value()};
            auto attached = contexts.Attach(descriptor, admission->ActivationLease(), capabilities);
            if (attached.HasError())
                return Result<void>::Failure(attached.ErrorValue());
            auto registered = live->Register(std::move(attached).Value(), definition, operations, code);
            if (registered.HasError())
                return Result<void>::Failure(registered.ErrorValue());
            registration.emplace(std::move(registered).Value());
            committed = true;
            return Result<void>::Success();
        }
    };
}  // namespace Horo::Extensions
