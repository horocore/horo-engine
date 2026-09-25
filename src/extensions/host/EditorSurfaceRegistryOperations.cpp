#include "EditorSurfaceRegistryInternal.h"
#include "Horo/Extensions/EditorSurfaceRegistry.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <memory>
#include <ranges>
#include <utility>

namespace Horo::Extensions {
    using namespace EditorSurfaceRegistryInternal;

    namespace {
        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &descriptor, const std::string_view detail = {}) {
            return Result<void>::Failure(detail.empty() ? MakeError(descriptor) : MakeError(descriptor, std::string{detail}));
        }

        template <typename Value>
        [[nodiscard]] Result<Value> FailureValue(const ErrorCodeDescriptor &descriptor, const std::string_view detail = {}) {
            return Result<Value>::Failure(detail.empty() ? MakeError(descriptor) : MakeError(descriptor, std::string{detail}));
        }
    }  // namespace

    /** @copydoc EditorSurfaceRegistry::Register */
    Result<EditorSurfaceRegistration> EditorSurfaceRegistry::Register(EditorSurfaceContextRegistration context) const {
        if (state_ == nullptr || !state_->validLimits)
            return FailureValue<EditorSurfaceRegistration>(ExtensionErrors::EditorSurfaceRegistryInvalid);
        if (!context.IsRegistered())
            return FailureValue<EditorSurfaceRegistration>(ExtensionErrors::EditorSurfaceContextRevoked);

        const EditorSurfaceDescriptor &descriptor = context.Context().Surface();
        if (const Result<void> valid = ValidateEditorSurfaceDescriptor(descriptor); valid.HasError())
            return FailureValue<EditorSurfaceRegistration>(ExtensionErrors::EditorSurfaceRegistryInvalid);
        if (!IsSupportedPersistentSurface(descriptor))
            return FailureValue<EditorSurfaceRegistration>(ExtensionErrors::EditorSurfaceRegistryInvalid);

        const std::string surfaceId = descriptor.id;
        const EditorSurfaceProviderKey provider = ProviderKey(descriptor);
        std::scoped_lock lock{state_->mutex};
        if (state_->shutdown)
            return FailureValue<EditorSurfaceRegistration>(ExtensionErrors::EditorSurfaceRegistryShutdown);
        if (FindSurface(*state_, surfaceId) != nullptr)
            return FailureValue<EditorSurfaceRegistration>(ExtensionErrors::EditorSurfaceRegistryDuplicate);
        if (state_->surfaces.size() >= state_->limits.maximumSurfaces)
            return FailureValue<EditorSurfaceRegistration>(ExtensionErrors::EditorSurfaceRegistryCapacityExceeded);

        const PendingSurfaceState *pending = FindPending(*state_, surfaceId);
        if (pending != nullptr && (!SameProvider(pending->entry.provider, provider) || pending->persistence != descriptor.persistence))
            return FailureValue<EditorSurfaceRegistration>(ExtensionErrors::EditorSurfaceRegistryInvalid);
        if (!HasPreservationCapacity(*state_) ||
            (pending == nullptr && state_->surfaces.size() >= state_->limits.maximumWorkspaceEntries - state_->pending.size()))
            return FailureValue<EditorSurfaceRegistration>(ExtensionErrors::EditorSurfaceRegistryCapacityExceeded);

        if (const EditorSurfaceProviderStatus configured = ConfiguredStatus(*state_, provider);
            configured == EditorSurfaceProviderStatus::Missing)
            SetProviderConfiguredStatus(*state_, provider, EditorSurfaceProviderStatus::Active);

        auto surface = std::make_shared<EditorSurfaceState>(std::move(context));
        if (pending != nullptr) {
            surface->desiredOpen = pending->entry.open;
            surface->desiredFocused = pending->entry.focused;
            surface->opaqueState = pending->entry.opaqueState;
        } else {
            surface->desiredOpen = descriptor.openByDefault;
        }
        state_->surfaces.push_back(surface);
        if (pending != nullptr) {
            const auto found = std::ranges::find_if(state_->pending, [&surfaceId](const PendingSurfaceState &candidate) {
                return candidate.entry.surfaceId == surfaceId;
            });
            state_->pending.erase(found);
        }
        return Result<EditorSurfaceRegistration>::Success(EditorSurfaceRegistration{state_, std::move(surface)});
    }

    /** @copydoc EditorSurfaceRegistry::Open */
    Result<EditorSurfaceOperation> EditorSurfaceRegistry::Open(const std::string_view surfaceId) const {
        if (state_ == nullptr)
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryShutdown);
        std::scoped_lock lock{state_->mutex};
        if (state_->shutdown)
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryShutdown);
        const std::shared_ptr<EditorSurfaceState> surface = FindSurface(*state_, surfaceId);
        if (surface == nullptr) {
            if (const PendingSurfaceState *pending = FindPending(*state_, surfaceId); pending != nullptr)
                return ProviderFailure(ConfiguredStatus(*state_, pending->entry.provider));
            return UnknownOperation();
        }
        if (const EditorSurfaceProviderStatus status = EffectiveStatus(*state_, *surface); status != EditorSurfaceProviderStatus::Active)
            return ProviderFailure(status);
        if (surface->desiredOpen) {
            if (surface->desiredFocused)
                return Result<EditorSurfaceOperation>::Success({EditorSurfaceOperationKind::AlreadyOpen});
            surface->desiredFocused = true;
            return Result<EditorSurfaceOperation>::Success({EditorSurfaceOperationKind::Focused});
        }
        surface->desiredOpen = true;
        surface->desiredFocused = true;
        return Result<EditorSurfaceOperation>::Success({EditorSurfaceOperationKind::Opened});
    }

    /** @copydoc EditorSurfaceRegistry::Focus */
    Result<EditorSurfaceOperation> EditorSurfaceRegistry::Focus(const std::string_view surfaceId) const {
        if (state_ == nullptr)
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryShutdown);
        std::scoped_lock lock{state_->mutex};
        if (state_->shutdown)
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryShutdown);
        const std::shared_ptr<EditorSurfaceState> surface = FindSurface(*state_, surfaceId);
        if (surface == nullptr) {
            if (const PendingSurfaceState *pending = FindPending(*state_, surfaceId); pending != nullptr)
                return ProviderFailure(ConfiguredStatus(*state_, pending->entry.provider));
            return UnknownOperation();
        }
        if (const EditorSurfaceProviderStatus status = EffectiveStatus(*state_, *surface); status != EditorSurfaceProviderStatus::Active)
            return ProviderFailure(status);
        if (!surface->desiredOpen)
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistrySurfaceClosed);
        if (surface->desiredFocused)
            return Result<EditorSurfaceOperation>::Success({EditorSurfaceOperationKind::AlreadyFocused});
        surface->desiredFocused = true;
        return Result<EditorSurfaceOperation>::Success({EditorSurfaceOperationKind::Focused});
    }

    /** @copydoc EditorSurfaceRegistry::Close */
    Result<EditorSurfaceOperation> EditorSurfaceRegistry::Close(const std::string_view surfaceId) const {
        if (state_ == nullptr)
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryShutdown);
        std::scoped_lock lock{state_->mutex};
        if (state_->shutdown)
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryShutdown);
        if (const std::shared_ptr<EditorSurfaceState> surface = FindSurface(*state_, surfaceId); surface != nullptr) {
            if (!surface->desiredOpen)
                return Result<EditorSurfaceOperation>::Success({EditorSurfaceOperationKind::AlreadyClosed});
            surface->desiredOpen = false;
            surface->desiredFocused = false;
            return Result<EditorSurfaceOperation>::Success({EditorSurfaceOperationKind::Closed});
        }
        if (PendingSurfaceState *pending = FindPending(*state_, surfaceId); pending != nullptr) {
            if (!pending->entry.open)
                return Result<EditorSurfaceOperation>::Success({EditorSurfaceOperationKind::AlreadyClosed});
            pending->entry.open = false;
            pending->entry.focused = false;
            return Result<EditorSurfaceOperation>::Success({EditorSurfaceOperationKind::Closed});
        }
        return UnknownOperation();
    }

    /** @copydoc EditorSurfaceRegistry::SetProviderStatus */
    Result<void> EditorSurfaceRegistry::SetProviderStatus(EditorSurfaceProviderKey provider,
                                                          const EditorSurfaceProviderStatus status) const {
        if (!IsValidProviderKey(provider) || !IsValidStatus(status))
            return Failure(ExtensionErrors::EditorSurfaceRegistryInvalid);
        if (state_ == nullptr)
            return Failure(ExtensionErrors::EditorSurfaceRegistryShutdown);
        std::scoped_lock lock{state_->mutex};
        if (state_->shutdown)
            return Failure(ExtensionErrors::EditorSurfaceRegistryShutdown);
        SetProviderConfiguredStatus(*state_, provider, status);
        return Result<void>::Success();
    }

    /** @copydoc EditorSurfaceRegistry::SetOpaqueState */
    Result<void> EditorSurfaceRegistry::SetOpaqueState(const std::string_view surfaceId, const std::span<const std::uint8_t> state) const {
        if (state_ == nullptr)
            return Failure(ExtensionErrors::EditorSurfaceRegistryShutdown);
        std::scoped_lock lock{state_->mutex};
        if (state_->shutdown)
            return Failure(ExtensionErrors::EditorSurfaceRegistryShutdown);
        if (!state_->validLimits || state.size() > state_->limits.maximumOpaqueStateBytes ||
            !FitsTotalOpaqueState(*state_, surfaceId, state.size()))
            return Failure(ExtensionErrors::EditorSurfaceRegistryStateInvalid);
        const std::shared_ptr<EditorSurfaceState> surface = FindSurface(*state_, surfaceId);
        if (surface == nullptr)
            return Failure(ExtensionErrors::EditorSurfaceRegistryUnknown);
        surface->opaqueState.assign(state.begin(), state.end());
        return Result<void>::Success();
    }

}  // namespace Horo::Extensions
