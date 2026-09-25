#pragma once

#include "Horo/Extensions/EditorSurfaceRegistry.h"

#include <mutex>
#include <type_traits>

namespace Horo::Extensions {
    struct PendingSurfaceState final {
        EditorSurfacePersistence persistence{EditorSurfacePersistence::Workspace};
        EditorSurfaceWorkspaceEntry entry;
    };

    static_assert(std::is_nothrow_move_constructible_v<PendingSurfaceState>);

    struct ProviderStatusEntry final {
        EditorSurfaceProviderKey provider;
        EditorSurfaceProviderStatus status{EditorSurfaceProviderStatus::Active};
    };

    struct EditorSurfaceState final {
        explicit EditorSurfaceState(EditorSurfaceContextRegistration registration);

        EditorSurfaceContextRegistration context;
        EditorSurfaceDescriptor descriptor;
        EditorSurfaceProviderKey provider;
        std::vector<std::uint8_t> opaqueState;
        bool desiredOpen{};
        bool desiredFocused{};
        std::atomic_bool registered{true};
    };

    struct EditorSurfaceRegistryState final {
        explicit EditorSurfaceRegistryState(const EditorSurfaceRegistryLimits &registryLimits);

        EditorSurfaceRegistryLimits limits;
        bool validLimits{};
        std::vector<std::shared_ptr<EditorSurfaceState>> surfaces;
        std::vector<PendingSurfaceState> pending;
        std::vector<ProviderStatusEntry> providers;
        bool shutdown{};

        [[nodiscard]] std::unique_lock<std::mutex> Lock() const {
            return std::unique_lock{mutex};
        }

    private:
        mutable std::mutex mutex;
    };

    namespace EditorSurfaceRegistryInternal {
        [[nodiscard]] bool IsValidProviderKey(const EditorSurfaceProviderKey &provider);
        [[nodiscard]] EditorSurfaceProviderKey ProviderKey(const EditorSurfaceProviderIdentity &provider);
        [[nodiscard]] EditorSurfaceProviderKey ProviderKey(const EditorSurfaceDescriptor &descriptor);
        [[nodiscard]] bool IsSupportedPersistentSurface(const EditorSurfaceDescriptor &descriptor);
        [[nodiscard]] bool IsValidStatus(EditorSurfaceProviderStatus status) noexcept;
        [[nodiscard]] bool SameProvider(const EditorSurfaceProviderKey &left, const EditorSurfaceProviderKey &right) noexcept;
        [[nodiscard]] bool ValidLimits(const EditorSurfaceRegistryLimits &limits) noexcept;
        [[nodiscard]] bool HasPreservationCapacity(const EditorSurfaceRegistryState &state) noexcept;
        [[nodiscard]] PendingSurfaceState TakePending(EditorSurfaceState &surface) noexcept;
        [[nodiscard]] EditorSurfaceProviderStatus ConfiguredStatus(const EditorSurfaceRegistryState &state,
                                                                   const EditorSurfaceProviderKey &provider);
        [[nodiscard]] EditorSurfaceProviderStatus EffectiveStatus(const EditorSurfaceRegistryState &state,
                                                                  const EditorSurfaceState &surface);
        [[nodiscard]] std::shared_ptr<EditorSurfaceState> FindSurface(const EditorSurfaceRegistryState &state, std::string_view id);
        [[nodiscard]] PendingSurfaceState MakePending(const EditorSurfaceState &surface);
        [[nodiscard]] PendingSurfaceState *FindPending(EditorSurfaceRegistryState &state, std::string_view id);
        [[nodiscard]] const PendingSurfaceState *FindPending(const EditorSurfaceRegistryState &state, std::string_view id);
        [[nodiscard]] bool FitsTotalOpaqueState(const EditorSurfaceRegistryState &state, std::string_view replacementId,
                                                std::size_t replacementBytes) noexcept;
        void SetProviderConfiguredStatus(EditorSurfaceRegistryState &state, const EditorSurfaceProviderKey &provider,
                                         EditorSurfaceProviderStatus status);
        [[nodiscard]] Result<void> ValidateWorkspaceState(const EditorSurfaceWorkspaceState &workspace,
                                                          const EditorSurfaceRegistryLimits &limits);
        [[nodiscard]] Result<EditorSurfaceOperation> ProviderFailure(EditorSurfaceProviderStatus status);
        [[nodiscard]] Result<EditorSurfaceOperation> UnknownOperation();
    }  // namespace EditorSurfaceRegistryInternal
}  // namespace Horo::Extensions
