#include "Horo/Extensions/EditorSurfaceRegistry.h"

#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <ranges>
#include <type_traits>
#include <utility>

namespace Horo::Extensions {
    namespace {
        constexpr std::size_t kMaximumIdentityBytes = 256;

        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &descriptor, const std::string_view detail = {}) {
            return Result<void>::Failure(detail.empty() ? MakeError(descriptor) : MakeError(descriptor, std::string{detail}));
        }

        template <typename Value>
        [[nodiscard]] Result<Value> FailureValue(const ErrorCodeDescriptor &descriptor, const std::string_view detail = {}) {
            return Result<Value>::Failure(detail.empty() ? MakeError(descriptor) : MakeError(descriptor, std::string{detail}));
        }

        [[nodiscard]] bool IsCanonicalIdentity(const std::string &value) {
            if (value.empty() || value.size() > kMaximumIdentityBytes || value.front() == '.' || value.back() == '.')
                return false;
            bool previousDot = false;
            for (const unsigned char character : value) {
                if (character == '.') {
                    if (previousDot)
                        return false;
                    previousDot = true;
                    continue;
                }
                if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-' ||
                      character == '_'))
                    return false;
                previousDot = false;
            }
            return !previousDot;
        }

        [[nodiscard]] bool IsValidProviderKey(const EditorSurfaceProviderKey &provider) {
            return IsCanonicalIdentity(provider.extensionId) && IsCanonicalIdentity(provider.moduleId);
        }

        [[nodiscard]] EditorSurfaceProviderKey ProviderKey(const EditorSurfaceProviderIdentity &provider) {
            return EditorSurfaceProviderKey{provider.extensionId, provider.moduleId};
        }

        [[nodiscard]] EditorSurfaceProviderKey ProviderKey(const EditorSurfaceDescriptor &descriptor) {
            return ProviderKey(descriptor.provider);
        }

        [[nodiscard]] bool IsSupportedPersistentSurface(const EditorSurfaceDescriptor &descriptor) {
            return (descriptor.kind == EditorSurfaceKind::Panel || descriptor.kind == EditorSurfaceKind::Tab) &&
                   descriptor.persistence != EditorSurfacePersistence::None;
        }

        [[nodiscard]] bool IsValidStatus(const EditorSurfaceProviderStatus status) noexcept {
            return static_cast<std::uint8_t>(status) <= static_cast<std::uint8_t>(EditorSurfaceProviderStatus::Missing);
        }

        [[nodiscard]] bool SameProvider(const EditorSurfaceProviderKey &left, const EditorSurfaceProviderKey &right) noexcept {
            return left == right;
        }

        struct PendingSurfaceState final {
            EditorSurfacePersistence persistence{EditorSurfacePersistence::Workspace};
            EditorSurfaceWorkspaceEntry entry;
        };

        static_assert(std::is_nothrow_move_constructible_v<PendingSurfaceState>);

        [[nodiscard]] bool ValidLimits(const EditorSurfaceRegistryLimits &limits) noexcept {
            return limits.maximumSurfaces != 0 && limits.maximumWorkspaceEntries != 0 &&
                   limits.maximumWorkspaceEntries <= std::vector<PendingSurfaceState>{}.max_size() && limits.maximumOpaqueStateBytes != 0 &&
                   limits.maximumTotalStateBytes >= limits.maximumOpaqueStateBytes;
        }

        struct ProviderStatusEntry final {
            EditorSurfaceProviderKey provider;
            EditorSurfaceProviderStatus status{EditorSurfaceProviderStatus::Active};
        };

        [[nodiscard]] bool EntryStateFits(const EditorSurfaceWorkspaceEntry &entry, const EditorSurfaceRegistryLimits &limits,
                                          std::size_t &totalBytes) noexcept {
            if (!IsCanonicalIdentity(entry.surfaceId) || !IsValidProviderKey(entry.provider) || (entry.focused && !entry.open) ||
                entry.opaqueState.size() > limits.maximumOpaqueStateBytes ||
                totalBytes > limits.maximumTotalStateBytes - entry.opaqueState.size())
                return false;
            totalBytes += entry.opaqueState.size();
            return true;
        }
    }  // namespace

    struct EditorSurfaceState final {
        explicit EditorSurfaceState(EditorSurfaceContextRegistration registration)
            : context(std::move(registration)), descriptor(context.Context().Surface()), provider(ProviderKey(descriptor)) {}

        EditorSurfaceContextRegistration context;
        EditorSurfaceDescriptor descriptor;
        EditorSurfaceProviderKey provider;
        std::vector<std::uint8_t> opaqueState;
        bool desiredOpen{};
        bool desiredFocused{};
        std::atomic_bool registered{true};
    };

    struct EditorSurfaceRegistryState final {
        explicit EditorSurfaceRegistryState(const EditorSurfaceRegistryLimits registryLimits)
            : limits(registryLimits), validLimits(ValidLimits(registryLimits)) {
            if (validLimits)
                pending.reserve(limits.maximumWorkspaceEntries);
        }

        EditorSurfaceRegistryLimits limits;
        bool validLimits{};
        mutable std::mutex mutex;
        std::vector<std::shared_ptr<EditorSurfaceState>> surfaces;
        std::vector<PendingSurfaceState> pending;
        std::vector<ProviderStatusEntry> providers;
        bool shutdown{};
    };

    namespace {
        [[nodiscard]] bool HasPreservationCapacity(const EditorSurfaceRegistryState &state) noexcept {
            const std::size_t maximumEntries = state.limits.maximumWorkspaceEntries;
            return state.pending.size() <= maximumEntries && state.surfaces.size() <= maximumEntries - state.pending.size();
        }

        [[nodiscard]] PendingSurfaceState TakePending(EditorSurfaceState &surface) noexcept {
            return PendingSurfaceState{
                .persistence = surface.descriptor.persistence,
                .entry =
                    EditorSurfaceWorkspaceEntry{
                        .surfaceId = std::move(surface.descriptor.id),
                        .provider = std::move(surface.provider),
                        .open = surface.desiredOpen,
                        .focused = surface.desiredOpen && surface.desiredFocused,
                        .opaqueState = std::move(surface.opaqueState),
                    },
            };
        }

        [[nodiscard]] EditorSurfaceProviderStatus ConfiguredStatus(const EditorSurfaceRegistryState &state,
                                                                   const EditorSurfaceProviderKey &provider) {
            const auto found = std::ranges::find_if(state.providers, [&provider](const ProviderStatusEntry &entry) {
                return SameProvider(entry.provider, provider);
            });
            return found == state.providers.end() ? EditorSurfaceProviderStatus::Active : found->status;
        }

        [[nodiscard]] EditorSurfaceProviderStatus EffectiveStatus(const EditorSurfaceRegistryState &state,
                                                                  const EditorSurfaceState &surface) {
            if (const EditorSurfaceProviderStatus configured = ConfiguredStatus(state, surface.provider);
                configured != EditorSurfaceProviderStatus::Active)
                return configured;
            return surface.context.IsRegistered() ? EditorSurfaceProviderStatus::Active : EditorSurfaceProviderStatus::Missing;
        }

        [[nodiscard]] std::shared_ptr<EditorSurfaceState> FindSurface(const EditorSurfaceRegistryState &state, const std::string_view id) {
            const auto found = std::ranges::find_if(state.surfaces, [id](const std::shared_ptr<EditorSurfaceState> &surface) {
                return surface->registered.load(std::memory_order_acquire) && surface->descriptor.id == id;
            });
            return found == state.surfaces.end() ? nullptr : *found;
        }

        [[nodiscard]] PendingSurfaceState MakePending(const EditorSurfaceState &surface) {
            return PendingSurfaceState{
                .persistence = surface.descriptor.persistence,
                .entry =
                    EditorSurfaceWorkspaceEntry{
                        .surfaceId = surface.descriptor.id,
                        .provider = surface.provider,
                        .open = surface.desiredOpen,
                        .focused = surface.desiredOpen && surface.desiredFocused,
                        .opaqueState = surface.opaqueState,
                    },
            };
        }

        [[nodiscard]] PendingSurfaceState *FindPending(EditorSurfaceRegistryState &state, const std::string_view id) {
            const auto found = std::ranges::find_if(state.pending, [id](const PendingSurfaceState &pending) {
                return pending.entry.surfaceId == id;
            });
            return found == state.pending.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] bool FitsTotalOpaqueState(const EditorSurfaceRegistryState &state, const std::string_view replacementId,
                                                const std::size_t replacementBytes) noexcept {
            if (replacementBytes > state.limits.maximumTotalStateBytes)
                return false;
            std::size_t total = 0;
            const auto add = [&total, maximum = state.limits.maximumTotalStateBytes](const std::size_t bytes) {
                if (bytes > maximum - total)
                    return false;
                total += bytes;
                return true;
            };
            for (const std::shared_ptr<EditorSurfaceState> &surface : state.surfaces) {
                const std::size_t bytes = surface->descriptor.id == replacementId ? replacementBytes : surface->opaqueState.size();
                if (!add(bytes))
                    return false;
            }
            for (const PendingSurfaceState &pending : state.pending) {
                if (!add(pending.entry.opaqueState.size()))
                    return false;
            }
            return true;
        }

        void SetProviderConfiguredStatus(EditorSurfaceRegistryState &state, const EditorSurfaceProviderKey &provider,
                                         const EditorSurfaceProviderStatus status) {
            const auto found = std::ranges::find_if(state.providers, [&provider](const ProviderStatusEntry &entry) {
                return SameProvider(entry.provider, provider);
            });
            if (found == state.providers.end())
                state.providers.push_back(ProviderStatusEntry{provider, status});
            else
                found->status = status;
        }

        [[nodiscard]] Result<void> ValidateWorkspaceState(const EditorSurfaceWorkspaceState &workspace,
                                                          const EditorSurfaceRegistryLimits &limits) {
            if (workspace.schemaVersion != EditorSurfaceRegistry::WorkspaceSchemaVersion ||
                workspace.surfaces.size() > limits.maximumWorkspaceEntries)
                return Failure(ExtensionErrors::EditorSurfaceRegistryStateInvalid);

            std::size_t totalBytes = 0;
            for (std::size_t index = 0; index < workspace.surfaces.size(); ++index) {
                const EditorSurfaceWorkspaceEntry &entry = workspace.surfaces[index];
                if (!EntryStateFits(entry, limits, totalBytes))
                    return Failure(ExtensionErrors::EditorSurfaceRegistryStateInvalid);
                for (std::size_t previous = 0; previous < index; ++previous) {
                    if (workspace.surfaces[previous].surfaceId == entry.surfaceId)
                        return Failure(ExtensionErrors::EditorSurfaceRegistryStateInvalid);
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<EditorSurfaceOperation> ProviderFailure(const EditorSurfaceProviderStatus status) {
            if (status == EditorSurfaceProviderStatus::Disabled)
                return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryProviderDisabled);
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryProviderMissing);
        }

        [[nodiscard]] Result<EditorSurfaceOperation> UnknownOperation() {
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryUnknown);
        }
    }  // namespace

    EditorSurfaceRegistration::EditorSurfaceRegistration(std::weak_ptr<EditorSurfaceRegistryState> registry,
                                                         std::shared_ptr<EditorSurfaceState> surface) noexcept
        : registry_(std::move(registry)), surface_(std::move(surface)) {}

    EditorSurfaceRegistration::~EditorSurfaceRegistration() noexcept {
        Reset();
    }

    EditorSurfaceRegistration::EditorSurfaceRegistration(EditorSurfaceRegistration &&other) noexcept
        : registry_(std::move(other.registry_)), surface_(std::move(other.surface_)) {}

    EditorSurfaceRegistration &EditorSurfaceRegistration::operator=(EditorSurfaceRegistration &&other) noexcept {
        if (this == &other)
            return *this;
        Reset();
        registry_ = std::move(other.registry_);
        surface_ = std::move(other.surface_);
        return *this;
    }

    /** @copydoc EditorSurfaceRegistration::Reset */
    void EditorSurfaceRegistration::Reset() const noexcept {
        if (surface_ == nullptr)
            return;
        const std::shared_ptr<EditorSurfaceState> surface = surface_;
        if (const auto registry = registry_.lock())
            EditorSurfaceRegistry::Remove(registry, surface);
        else
            surface->registered.store(false, std::memory_order_release);
        surface_.reset();
        registry_.reset();
    }

    /** @copydoc EditorSurfaceRegistration::IsRegistered */
    bool EditorSurfaceRegistration::IsRegistered() const noexcept {
        return surface_ != nullptr && surface_->registered.load(std::memory_order_acquire);
    }

    /** @copydoc EditorSurfaceRegistration::Id */
    std::string_view EditorSurfaceRegistration::Id() const noexcept {
        return surface_ == nullptr ? std::string_view{} : std::string_view{surface_->descriptor.id};
    }

    /** @copydoc EditorSurfaceRegistration::Descriptor */
    const EditorSurfaceDescriptor &EditorSurfaceRegistration::Descriptor() const noexcept {
        return surface_->descriptor;
    }

    EditorSurfaceRegistry::EditorSurfaceRegistry(const EditorSurfaceRegistryLimits limits)
        : state_(std::make_shared<EditorSurfaceRegistryState>(limits)) {}

    EditorSurfaceRegistry::~EditorSurfaceRegistry() noexcept {
        BeginShutdown();
    }

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

    /** @copydoc EditorSurfaceRegistry::Restore */
    Result<EditorSurfaceRestoreReport> EditorSurfaceRegistry::Restore(const EditorSurfaceWorkspaceState &workspace) const {
        if (state_ == nullptr)
            return FailureValue<EditorSurfaceRestoreReport>(ExtensionErrors::EditorSurfaceRegistryShutdown);
        if (!state_->validLimits)
            return FailureValue<EditorSurfaceRestoreReport>(ExtensionErrors::EditorSurfaceRegistryInvalid);
        if (const Result<void> valid = ValidateWorkspaceState(workspace, state_->limits); valid.HasError())
            return FailureValue<EditorSurfaceRestoreReport>(ExtensionErrors::EditorSurfaceRegistryStateInvalid);

        std::scoped_lock lock{state_->mutex};
        if (state_->shutdown)
            return FailureValue<EditorSurfaceRestoreReport>(ExtensionErrors::EditorSurfaceRegistryShutdown);
        if (!HasPreservationCapacity(*state_))
            return FailureValue<EditorSurfaceRestoreReport>(ExtensionErrors::EditorSurfaceRegistryCapacityExceeded);

        EditorSurfaceRestoreReport report;
        std::vector<PendingSurfaceState> pending;
        pending.reserve(state_->limits.maximumWorkspaceEntries);
        for (const PendingSurfaceState &entry : state_->pending) {
            if (entry.persistence != EditorSurfacePersistence::Workspace)
                pending.push_back(entry);
        }

        for (const EditorSurfaceWorkspaceEntry &entry : workspace.surfaces) {
            const std::shared_ptr<EditorSurfaceState> surface = FindSurface(*state_, entry.surfaceId);
            if (surface == nullptr) {
                if (const PendingSurfaceState *existing = FindPending(*state_, entry.surfaceId);
                    existing != nullptr && existing->persistence != EditorSurfacePersistence::Workspace)
                    return FailureValue<EditorSurfaceRestoreReport>(ExtensionErrors::EditorSurfaceRegistryStateInvalid);
                const std::size_t availablePending = state_->limits.maximumWorkspaceEntries - state_->surfaces.size();
                if (pending.size() >= availablePending)
                    return FailureValue<EditorSurfaceRestoreReport>(ExtensionErrors::EditorSurfaceRegistryCapacityExceeded);
                pending.push_back(PendingSurfaceState{.persistence = EditorSurfacePersistence::Workspace, .entry = entry});
                if (ConfiguredStatus(*state_, entry.provider) == EditorSurfaceProviderStatus::Disabled)
                    report.disabledProvider.push_back(entry.surfaceId);
                else
                    report.missingProvider.push_back(entry.surfaceId);
                continue;
            }
            if (surface->descriptor.persistence != EditorSurfacePersistence::Workspace || !SameProvider(surface->provider, entry.provider))
                return FailureValue<EditorSurfaceRestoreReport>(ExtensionErrors::EditorSurfaceRegistryStateInvalid);
            const EditorSurfaceProviderStatus status = EffectiveStatus(*state_, *surface);
            if (status == EditorSurfaceProviderStatus::Active)
                report.restored.push_back(entry.surfaceId);
            else if (status == EditorSurfaceProviderStatus::Disabled)
                report.disabledProvider.push_back(entry.surfaceId);
            else
                report.missingProvider.push_back(entry.surfaceId);
        }

        struct SurfaceRestoreState final {
            std::shared_ptr<EditorSurfaceState> surface;
            bool desiredOpen{};
            bool desiredFocused{};
            std::vector<std::uint8_t> opaqueState;
        };

        std::vector<SurfaceRestoreState> restoredSurfaces;
        restoredSurfaces.reserve(state_->surfaces.size());
        for (const std::shared_ptr<EditorSurfaceState> &surface : state_->surfaces) {
            if (surface->descriptor.persistence != EditorSurfacePersistence::Workspace)
                continue;
            SurfaceRestoreState restored{
                .surface = surface,
                .desiredOpen = surface->descriptor.openByDefault,
            };
            const auto entry = std::ranges::find_if(workspace.surfaces, [&surface](const EditorSurfaceWorkspaceEntry &candidate) {
                return candidate.surfaceId == surface->descriptor.id;
            });
            if (entry != workspace.surfaces.end()) {
                restored.desiredOpen = entry->open;
                restored.desiredFocused = entry->focused;
                restored.opaqueState = entry->opaqueState;
            }
            restoredSurfaces.push_back(std::move(restored));
        }

        std::size_t totalOpaqueStateBytes = 0;
        const auto addOpaqueState = [&totalOpaqueStateBytes, maximum = state_->limits.maximumTotalStateBytes](const std::size_t bytes) {
            if (bytes > maximum - totalOpaqueStateBytes)
                return false;
            totalOpaqueStateBytes += bytes;
            return true;
        };
        for (const SurfaceRestoreState &restored : restoredSurfaces) {
            if (!addOpaqueState(restored.opaqueState.size()))
                return FailureValue<EditorSurfaceRestoreReport>(ExtensionErrors::EditorSurfaceRegistryStateInvalid);
        }
        for (const std::shared_ptr<EditorSurfaceState> &surface : state_->surfaces) {
            if (surface->descriptor.persistence != EditorSurfacePersistence::Workspace && !addOpaqueState(surface->opaqueState.size()))
                return FailureValue<EditorSurfaceRestoreReport>(ExtensionErrors::EditorSurfaceRegistryStateInvalid);
        }
        for (const PendingSurfaceState &entry : pending) {
            if (!addOpaqueState(entry.entry.opaqueState.size()))
                return FailureValue<EditorSurfaceRestoreReport>(ExtensionErrors::EditorSurfaceRegistryStateInvalid);
        }

        state_->pending = std::move(pending);
        for (SurfaceRestoreState &restored : restoredSurfaces) {
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
        std::scoped_lock lock{state_->mutex};
        const std::size_t registeredWorkspaceSurfaces =
            static_cast<std::size_t>(std::ranges::count_if(state_->surfaces, [](const std::shared_ptr<EditorSurfaceState> &surface) {
            return surface->descriptor.persistence == EditorSurfacePersistence::Workspace;
        }));
        const std::size_t pendingWorkspaceSurfaces =
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
        std::scoped_lock lock{state_->mutex};
        snapshots.reserve(state_->surfaces.size());
        for (const std::shared_ptr<EditorSurfaceState> &surface : state_->surfaces) {
            const EditorSurfaceProviderStatus status = EffectiveStatus(*state_, *surface);
            snapshots.push_back(EditorSurfaceSnapshot{
                .descriptor = surface->descriptor,
                .providerStatus = status,
                .open = status == EditorSurfaceProviderStatus::Active && surface->desiredOpen,
                .focused = status == EditorSurfaceProviderStatus::Active && surface->desiredOpen && surface->desiredFocused,
                .opaqueState = surface->opaqueState,
            });
        }
        std::ranges::sort(snapshots, {}, [](const EditorSurfaceSnapshot &snapshot) {
            return snapshot.descriptor.id;
        });
        return snapshots;
    }

    /** @copydoc EditorSurfaceRegistry::BeginShutdown */
    void EditorSurfaceRegistry::BeginShutdown() const noexcept {
        if (state_ == nullptr)
            return;
        std::scoped_lock lock{state_->mutex};
        if (state_->shutdown)
            return;
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
        std::scoped_lock lock{state_->mutex};
        return state_->shutdown;
    }

    /** @copydoc EditorSurfaceRegistry::Remove */
    void EditorSurfaceRegistry::Remove(const std::shared_ptr<EditorSurfaceRegistryState> &registry,
                                       const std::shared_ptr<EditorSurfaceState> &surface) noexcept {
        if (registry == nullptr || surface == nullptr)
            return;
        std::scoped_lock lock{registry->mutex};
        if (!surface->registered.exchange(false, std::memory_order_acq_rel))
            return;
        const auto found = std::ranges::find(registry->surfaces, surface);
        if (found == registry->surfaces.end())
            return;
        if (!registry->shutdown) {
            assert(HasPreservationCapacity(*registry));
            assert(registry->pending.size() < registry->limits.maximumWorkspaceEntries);
            assert(registry->pending.capacity() >= registry->limits.maximumWorkspaceEntries);
            registry->pending.push_back(TakePending(*surface));
        }
        registry->surfaces.erase(found);
    }
}  // namespace Horo::Extensions
