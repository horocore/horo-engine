#include "EditorSurfaceRegistryInternal.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <ranges>
#include <type_traits>

namespace Horo::Extensions {
    using namespace EditorSurfaceRegistryInternal;

    /** @copydoc EditorSurfaceRegistry::CloseProviderWorkspace */
    Result<void> EditorSurfaceRegistry::CloseProviderWorkspace(const EditorSurfaceProviderKey &provider) const {
        if (!IsValidProviderKey(provider))
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorSurfaceRegistryInvalid));
        if (!state_)
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorSurfaceRegistryShutdown));
        auto lock = state_->Lock();
        if (state_->shutdown)
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorSurfaceRegistryShutdown));
        for (const auto &surface : state_->surfaces)
            if (surface->descriptor.persistence == EditorSurfacePersistence::Workspace && SameProvider(surface->provider, provider)) {
                surface->desiredOpen = false;
                surface->desiredFocused = false;
            }
        for (auto &pending : state_->pending)
            if (pending.persistence == EditorSurfacePersistence::Workspace && SameProvider(pending.entry.provider, provider)) {
                pending.entry.open = false;
                pending.entry.focused = false;
            }
        ++state_->revision;
        return Result<void>::Success();
    }

    /** @copydoc EditorSurfaceRegistry::SetExtensionProviderStatus */
    Result<void> EditorSurfaceRegistry::SetExtensionProviderStatus(const std::string_view extensionId,
                                                                   const EditorSurfaceProviderStatus status) const {
        if (extensionId.empty() || extensionId.size() > 256 || !IsValidStatus(status))
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorSurfaceRegistryInvalid));
        if (!state_)
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorSurfaceRegistryShutdown));
        auto lock = state_->Lock();
        if (state_->shutdown)
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorSurfaceRegistryShutdown));
        for (auto &entry : state_->providers)
            if (entry.provider.extensionId == extensionId)
                entry.status = status;
        ++state_->revision;
        return Result<void>::Success();
    }

    /** @copydoc EditorSurfaceRegistry::SetProviderStatuses */
    Result<void> EditorSurfaceRegistry::SetProviderStatuses(const std::span<const EditorSurfaceProviderKey> providers,
                                                            const EditorSurfaceProviderStatus status) const {
        if (!IsValidStatus(status) || !std::ranges::all_of(providers, IsValidProviderKey))
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorSurfaceRegistryInvalid));
        if (!state_)
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorSurfaceRegistryShutdown));
        auto lock = state_->Lock();
        if (state_->shutdown)
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorSurfaceRegistryShutdown));
        if (providers.empty())
            return Result<void>::Success();
        std::vector<ProviderStatusEntry> additions;
        for (const auto &provider : providers) {
            if (std::ranges::find(state_->providers, provider, &ProviderStatusEntry::provider) != state_->providers.end() ||
                std::ranges::find(additions, provider, &ProviderStatusEntry::provider) != additions.end())
                continue;
            if (additions.size() >= state_->limits.maximumWorkspaceEntries - state_->providers.size())
                return Result<void>::Failure(MakeError(ExtensionErrors::EditorSurfaceRegistryCapacityExceeded));
            additions.emplace_back(provider, status);
        }
        static_assert(std::is_nothrow_move_constructible_v<ProviderStatusEntry>);
        if (!additions.empty()) {
            state_->providers.reserve(state_->providers.size() + additions.size());
            for (auto &entry : additions)
                state_->providers.push_back(std::move(entry));
        }
        for (auto &entry : state_->providers)
            if (std::ranges::find(providers, entry.provider) != providers.end())
                entry.status = status;
        ++state_->revision;
        return Result<void>::Success();
    }

    /** @copydoc EditorSurfaceRegistry::RecoverySnapshot */
    std::vector<EditorSurfaceRecoverySnapshot> EditorSurfaceRegistry::RecoverySnapshot() const {
        std::vector<EditorSurfaceRecoverySnapshot> result;
        if (!state_)
            return result;
        auto lock = state_->Lock();
        if (state_->shutdown)
            return result;
        result.reserve(state_->surfaces.size() + state_->pending.size());
        for (const auto &surface : state_->surfaces) {
            const auto status = EffectiveStatus(*state_, *surface);
            if (surface->descriptor.persistence == EditorSurfacePersistence::Workspace && status != EditorSurfaceProviderStatus::Active)
                result.emplace_back(MakePending(*surface).entry, status);
        }
        for (const auto &pending : state_->pending) {
            if (pending.persistence != EditorSurfacePersistence::Workspace)
                continue;
            auto status = ConfiguredStatus(*state_, pending.entry.provider);
            // A configured Active policy is not proof that this absent contribution was registered.
            if (status == EditorSurfaceProviderStatus::Active)
                status = EditorSurfaceProviderStatus::Missing;
            result.emplace_back(pending.entry, status);
        }
        std::ranges::sort(result, {}, [](const auto &entry) {
            return entry.intent.surfaceId;
        });
        return result;
    }
}  // namespace Horo::Extensions
