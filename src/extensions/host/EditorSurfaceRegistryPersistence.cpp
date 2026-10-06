#include "EditorSurfaceRegistryInternal.h"
#include "Horo/Extensions/EditorSurfaceRegistry.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <ranges>
#include <utility>

namespace Horo::Extensions {
    using namespace EditorSurfaceRegistryInternal;

    namespace {
        template <typename Value>
        [[nodiscard]] Result<Value> FailureValue(const ErrorCodeDescriptor &descriptor, const std::string_view detail = {}) {
            return Result<Value>::Failure(detail.empty() ? MakeError(descriptor) : MakeError(descriptor, std::string{detail}));
        }

        struct SurfaceRestoreState final {
            std::shared_ptr<EditorSurfaceState> surface;
            bool activityVisible{true};
            std::optional<EditorActivityPlacement> activityPlacement;
            bool desiredOpen{};
            bool desiredFocused{};
            std::vector<std::uint8_t> opaqueState;
        };

        [[nodiscard]] std::vector<PendingSurfaceState> KeepNonWorkspacePending(const EditorSurfaceRegistryState &state) {
            std::vector<PendingSurfaceState> pending;
            pending.reserve(state.limits.maximumWorkspaceEntries);
            for (const PendingSurfaceState &entry : state.pending) {
                if (entry.persistence != EditorSurfacePersistence::Workspace)
                    pending.push_back(entry);
            }
            return pending;
        }

        [[nodiscard]] const ErrorCodeDescriptor *StageWorkspaceEntry(const EditorSurfaceRegistryState &state,
                                                                     const EditorSurfaceWorkspaceEntry &entry,
                                                                     std::vector<PendingSurfaceState> &pending,
                                                                     EditorSurfaceRestoreReport &report) {
            const std::shared_ptr<EditorSurfaceState> surface = FindSurface(state, entry.surfaceId);
            if (surface == nullptr) {
                if (const PendingSurfaceState *existing = FindPending(state, entry.surfaceId);
                    existing != nullptr && existing->persistence != EditorSurfacePersistence::Workspace)
                    return &ExtensionErrors::EditorSurfaceRegistryStateInvalid;
                if (const auto availablePending = state.limits.maximumWorkspaceEntries - state.surfaces.size();
                    pending.size() >= availablePending)
                    return &ExtensionErrors::EditorSurfaceRegistryCapacityExceeded;
                pending.push_back(PendingSurfaceState{.persistence = EditorSurfacePersistence::Workspace, .entry = entry});
                if (ConfiguredStatus(state, entry.provider) == EditorSurfaceProviderStatus::Disabled)
                    report.disabledProvider.push_back(entry.surfaceId);
                else
                    report.missingProvider.push_back(entry.surfaceId);
                return nullptr;
            }
            if (entry.activityPlacement && !surface->descriptor.activity)
                return &ExtensionErrors::EditorSurfaceRegistryStateInvalid;
            if (surface->descriptor.persistence != EditorSurfacePersistence::Workspace || !SameProvider(surface->provider, entry.provider))
                return &ExtensionErrors::EditorSurfaceRegistryStateInvalid;
            if (const EditorSurfaceProviderStatus status = EffectiveStatus(state, *surface); status == EditorSurfaceProviderStatus::Active)
                report.restored.push_back(entry.surfaceId);
            else if (status == EditorSurfaceProviderStatus::Disabled)
                report.disabledProvider.push_back(entry.surfaceId);
            else
                report.missingProvider.push_back(entry.surfaceId);
            return nullptr;
        }

        [[nodiscard]] std::vector<SurfaceRestoreState> StageRegisteredWorkspaceSurfaces(const EditorSurfaceRegistryState &state,
                                                                                        const EditorSurfaceWorkspaceState &workspace) {
            std::vector<SurfaceRestoreState> restoredSurfaces;
            restoredSurfaces.reserve(state.surfaces.size());
            for (const std::shared_ptr<EditorSurfaceState> &surface : state.surfaces) {
                if (surface->descriptor.persistence != EditorSurfacePersistence::Workspace)
                    continue;
                SurfaceRestoreState restored{
                    .surface = surface,
                    .desiredOpen = surface->descriptor.openByDefault,
                };
                if (const auto entry = std::ranges::find_if(workspace.surfaces,
                                                            [&surface](const EditorSurfaceWorkspaceEntry &candidate) {
                    return candidate.surfaceId == surface->descriptor.id;
                });
                    entry != workspace.surfaces.end()) {
                    restored.desiredOpen = entry->open;
                    restored.desiredFocused = entry->focused;
                    restored.opaqueState = entry->opaqueState;
                    restored.activityVisible = entry->activityVisible;
                    restored.activityPlacement = entry->activityPlacement;
                }
                restoredSurfaces.push_back(std::move(restored));
            }
            return restoredSurfaces;
        }

        [[nodiscard]] bool RestoredOpaqueStateFits(const EditorSurfaceRegistryState &state, const std::vector<PendingSurfaceState> &pending,
                                                   const std::vector<SurfaceRestoreState> &restoredSurfaces) noexcept {
            std::size_t totalOpaqueStateBytes = 0;
            const auto addOpaqueState = [&totalOpaqueStateBytes, maximum = state.limits.maximumTotalStateBytes](const std::size_t bytes) {
                if (bytes > maximum - totalOpaqueStateBytes)
                    return false;
                totalOpaqueStateBytes += bytes;
                return true;
            };
            for (const SurfaceRestoreState &restored : restoredSurfaces) {
                if (!addOpaqueState(restored.opaqueState.size()))
                    return false;
            }
            for (const std::shared_ptr<EditorSurfaceState> &surface : state.surfaces) {
                if (surface->descriptor.persistence != EditorSurfacePersistence::Workspace && !addOpaqueState(surface->opaqueState.size()))
                    return false;
            }
            return std::ranges::all_of(pending, [&addOpaqueState](const PendingSurfaceState &entry) {
                return addOpaqueState(entry.entry.opaqueState.size());
            });
        }

        /** @brief Proves visibility, paired focus and same-side exclusivity for the staged atomic restore. */
        [[nodiscard]] bool ValidRestoredActivities(const std::vector<SurfaceRestoreState> &restoredSurfaces) {
            for (std::size_t index = 0; index < restoredSurfaces.size(); ++index) {
                const auto &candidate = restoredSurfaces[index];
                if (!candidate.surface->descriptor.activity.has_value())
                    continue;
                if (candidate.desiredOpen &&
                    (!candidate.activityVisible || !candidate.surface->activity.visible || !candidate.surface->activity.enabled))
                    return false;
                const auto drawer = std::ranges::find_if(restoredSurfaces, [&candidate](const SurfaceRestoreState &entry) {
                    return entry.surface->descriptor.id == candidate.surface->descriptor.activity->drawerId;
                });
                if (drawer == restoredSurfaces.end() || drawer->desiredOpen != candidate.desiredOpen ||
                    drawer->surface->descriptor.provider != candidate.surface->descriptor.provider ||
                    candidate.desiredFocused != drawer->desiredFocused)
                    return false;
                for (std::size_t previous = 0; previous < index; ++previous) {
                    const auto &other = restoredSurfaces[previous];
                    if (candidate.desiredOpen && other.desiredOpen && other.surface->descriptor.activity.has_value() &&
                        other.activityPlacement
                                .value_or(EditorActivityPlacement{other.surface->descriptor.activity->side,
                                                                  other.surface->descriptor.activity->group,
                                                                  other.surface->descriptor.placement.order})
                                .side == candidate.activityPlacement
                                             .value_or(EditorActivityPlacement{candidate.surface->descriptor.activity->side,
                                                                               candidate.surface->descriptor.activity->group,
                                                                               candidate.surface->descriptor.placement.order})
                                             .side)
                        return false;
                }
            }
            return true;
        }
    }  // namespace

    /** @copydoc EditorSurfaceRegistry::Restore */
    Result<EditorSurfaceRestoreReport> EditorSurfaceRegistry::Restore(const EditorSurfaceWorkspaceState &workspace) const {
        if (state_ == nullptr)
            return FailureValue<EditorSurfaceRestoreReport>(ExtensionErrors::EditorSurfaceRegistryShutdown);
        if (!state_->validLimits)
            return FailureValue<EditorSurfaceRestoreReport>(ExtensionErrors::EditorSurfaceRegistryInvalid);
        if (const Result<void> valid = ValidateWorkspaceState(workspace, state_->limits); valid.HasError())
            return FailureValue<EditorSurfaceRestoreReport>(ExtensionErrors::EditorSurfaceRegistryStateInvalid);

        auto lock = state_->Lock();
        if (state_->shutdown)
            return FailureValue<EditorSurfaceRestoreReport>(ExtensionErrors::EditorSurfaceRegistryShutdown);
        if (!HasPreservationCapacity(*state_))
            return FailureValue<EditorSurfaceRestoreReport>(ExtensionErrors::EditorSurfaceRegistryCapacityExceeded);

        EditorSurfaceRestoreReport report;
        std::vector<PendingSurfaceState> pending = KeepNonWorkspacePending(*state_);
        for (const EditorSurfaceWorkspaceEntry &entry : workspace.surfaces) {
            if (const ErrorCodeDescriptor *error = StageWorkspaceEntry(*state_, entry, pending, report); error != nullptr)
                return FailureValue<EditorSurfaceRestoreReport>(*error);
        }

        std::vector<SurfaceRestoreState> restoredSurfaces = StageRegisteredWorkspaceSurfaces(*state_, workspace);
        if (!RestoredOpaqueStateFits(*state_, pending, restoredSurfaces))
            return FailureValue<EditorSurfaceRestoreReport>(ExtensionErrors::EditorSurfaceRegistryStateInvalid);

        if (!ValidRestoredActivities(restoredSurfaces))
            return FailureValue<EditorSurfaceRestoreReport>(ExtensionErrors::EditorSurfaceRegistryStateInvalid);

        ++state_->revision;
        state_->pending = std::move(pending);
        for (SurfaceRestoreState &restored : restoredSurfaces) {
            restored.surface->activityUserVisible = restored.activityVisible;
            restored.surface->activityPlacement = restored.activityPlacement;
            restored.surface->desiredOpen = restored.desiredOpen;
            restored.surface->desiredFocused = restored.desiredFocused;
            restored.surface->opaqueState = std::move(restored.opaqueState);
        }
        return Result<EditorSurfaceRestoreReport>::Success(std::move(report));
    }

    /** @copydoc EditorSurfaceRegistry::Save */
    EditorSurfaceWorkspaceState EditorSurfaceRegistry::Save() const {
        EditorSurfaceWorkspaceState workspace{.schemaVersion = WorkspaceSchemaVersion};
        if (state_ == nullptr)
            return workspace;
        auto lock = state_->Lock();
        const auto registeredWorkspaceSurfaces =
            static_cast<std::size_t>(std::ranges::count_if(state_->surfaces, [](const std::shared_ptr<EditorSurfaceState> &surface) {
            return surface->descriptor.persistence == EditorSurfacePersistence::Workspace;
        }));
        const auto pendingWorkspaceSurfaces =
            static_cast<std::size_t>(std::ranges::count_if(state_->pending, [](const PendingSurfaceState &pending) {
            return pending.persistence == EditorSurfacePersistence::Workspace;
        }));
        workspace.surfaces.reserve(registeredWorkspaceSurfaces + pendingWorkspaceSurfaces);
        for (const std::shared_ptr<EditorSurfaceState> &surface : state_->surfaces) {
            if (surface->descriptor.persistence != EditorSurfacePersistence::Workspace)
                continue;
            PendingSurfaceState saved = MakePending(*surface);
            workspace.surfaces.push_back(std::move(saved.entry));
        }
        for (const PendingSurfaceState &pending : state_->pending)
            if (pending.persistence == EditorSurfacePersistence::Workspace)
                workspace.surfaces.push_back(pending.entry);
        std::ranges::sort(workspace.surfaces, {}, &EditorSurfaceWorkspaceEntry::surfaceId);
        return workspace;
    }

    /** @copydoc EditorSurfaceRegistry::Snapshot */
    std::vector<EditorSurfaceSnapshot> EditorSurfaceRegistry::Snapshot() const {
        std::vector<EditorSurfaceSnapshot> snapshots;
        if (state_ == nullptr)
            return snapshots;
        auto lock = state_->Lock();
        snapshots.reserve(state_->surfaces.size());
        for (const std::shared_ptr<EditorSurfaceState> &surface : state_->surfaces) {
            const EditorSurfaceProviderStatus status = EffectiveStatus(*state_, *surface);
            auto descriptor = surface->descriptor;
            if (descriptor.activity && surface->activityPlacement) {
                descriptor.activity->side = surface->activityPlacement->side;
                descriptor.activity->group = surface->activityPlacement->group;
                descriptor.placement.order = surface->activityPlacement->order;
            }
            snapshots.push_back(EditorSurfaceSnapshot{
                .descriptor = std::move(descriptor),
                .providerStatus = status,
                .open = status == EditorSurfaceProviderStatus::Active && surface->desiredOpen,
                .focused = status == EditorSurfaceProviderStatus::Active && surface->desiredOpen && surface->desiredFocused,
                .opaqueState = surface->opaqueState,
                .activity = EditorActivityPresentation{surface->activityUserVisible && surface->activity.visible, surface->activity.enabled,
                                                       surface->activity.badgeCount},
                .form = status == EditorSurfaceProviderStatus::Active ? surface->form : std::nullopt,
            });
        }
        std::ranges::sort(snapshots, {}, [](const EditorSurfaceSnapshot &snapshot) {
            return snapshot.descriptor.id;
        });
        return snapshots;
    }

    /** @copydoc EditorSurfaceRegistry::Revision */
    std::uint64_t EditorSurfaceRegistry::Revision() const noexcept {
        if (state_ == nullptr)
            return 0;
        auto lock = state_->Lock();
        return state_->revision;
    }

    /** @copydoc EditorSurfaceRegistry::BeginShutdown */
    void EditorSurfaceRegistry::BeginShutdown() const noexcept {
        if (state_ == nullptr)
            return;
        auto lock = state_->Lock();
        if (state_->shutdown)
            return;
        ++state_->revision;
        state_->shutdown = true;
        for (const std::shared_ptr<EditorSurfaceState> &surface : state_->surfaces)
            surface->registered.store(false, std::memory_order_release);
        state_->surfaces.clear();
        state_->pending.clear();
        state_->providers.clear();
    }

    /** @copydoc EditorSurfaceRegistry::IsShutdown */
    bool EditorSurfaceRegistry::IsShutdown() const noexcept {
        if (state_ == nullptr)
            return true;
        auto lock = state_->Lock();
        return state_->shutdown;
    }

    /** @copydoc EditorSurfaceRegistry::Remove */
    void EditorSurfaceRegistry::Remove(const std::shared_ptr<EditorSurfaceRegistryState> &registry,
                                       const std::shared_ptr<EditorSurfaceState> &surface) noexcept {
        if (registry == nullptr || surface == nullptr)
            return;
        auto lock = registry->Lock();
        if (!surface->registered.exchange(false, std::memory_order_acq_rel))
            return;
        const auto found = std::ranges::find(registry->surfaces, surface);
        if (found == registry->surfaces.end())
            return;
        ++registry->revision;
        if (!registry->shutdown) {
            assert(HasPreservationCapacity(*registry));
            assert(registry->pending.size() < registry->limits.maximumWorkspaceEntries);
            assert(registry->pending.capacity() >= registry->limits.maximumWorkspaceEntries);
            registry->pending.push_back(TakePending(*surface));
        }
        registry->surfaces.erase(found);
    }
}  // namespace Horo::Extensions
