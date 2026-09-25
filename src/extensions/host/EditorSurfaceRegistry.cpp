#include "Horo/Extensions/EditorSurfaceRegistry.h"

#include "EditorSurfaceRegistryInternal.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <memory>
#include <ranges>
#include <utility>

namespace Horo::Extensions {
    namespace EditorSurfaceRegistryInternal {
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

        [[nodiscard]] bool ValidLimits(const EditorSurfaceRegistryLimits &limits) noexcept {
            return limits.maximumSurfaces != 0 && limits.maximumWorkspaceEntries != 0 &&
                   limits.maximumWorkspaceEntries <= std::vector<PendingSurfaceState>{}.max_size() && limits.maximumOpaqueStateBytes != 0 &&
                   limits.maximumTotalStateBytes >= limits.maximumOpaqueStateBytes;
        }

        [[nodiscard]] bool EntryStateFits(const EditorSurfaceWorkspaceEntry &entry, const EditorSurfaceRegistryLimits &limits,
                                          std::size_t &totalBytes) noexcept {
            if (!IsCanonicalIdentity(entry.surfaceId) || !IsValidProviderKey(entry.provider) || (entry.focused && !entry.open) ||
                entry.opaqueState.size() > limits.maximumOpaqueStateBytes ||
                totalBytes > limits.maximumTotalStateBytes - entry.opaqueState.size())
                return false;
            totalBytes += entry.opaqueState.size();
            return true;
        }
    }  // namespace EditorSurfaceRegistryInternal

    namespace EditorSurfaceRegistryInternal {
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

        [[nodiscard]] const PendingSurfaceState *FindPending(const EditorSurfaceRegistryState &state, const std::string_view id) {
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
    }  // namespace EditorSurfaceRegistryInternal

    EditorSurfaceState::EditorSurfaceState(EditorSurfaceContextRegistration registration)
        : context(std::move(registration)), descriptor(context.Context().Surface()),
          provider(EditorSurfaceRegistryInternal::ProviderKey(descriptor)) {}

    EditorSurfaceRegistryState::EditorSurfaceRegistryState(const EditorSurfaceRegistryLimits registryLimits)
        : limits(registryLimits), validLimits(EditorSurfaceRegistryInternal::ValidLimits(registryLimits)) {
        if (validLimits)
            pending.reserve(limits.maximumWorkspaceEntries);
    }

    using namespace EditorSurfaceRegistryInternal;

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

}  // namespace Horo::Extensions
