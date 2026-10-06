#include "EditorSurfaceRegistryInternal.h"
#include "Horo/Extensions/EditorSurfaceRegistry.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <memory>
#include <ranges>
#include <tuple>
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

        /** @brief Admits text only through the existing technical-text or context localization policy. */
        [[nodiscard]] bool AllowsText(const EditorSurfaceContext &context, const EditorUiText &text) {
            return text.value.empty() || text.kind == EditorUiTextKind::TechnicalText ||
                   context.Allows(EditorSurfaceLocalizationKey{text.value});
        }

        /** @brief Checks each typed payload's permissions without invoking providers or mutating registry state. */
        [[nodiscard]] bool AllowsPayload(const EditorSurfaceContext &context, const EditorUiNodePayload &payload) {
            return std::visit([&context]<typename T>(const T &typed) {
                bool admitted = true;
                if constexpr (requires { typed.action; })
                    admitted = context.Allows(EditorSurfaceCommandId{typed.action.value});
                if constexpr (requires { typed.binding; })
                    admitted = admitted && context.Allows(EditorSurfaceStateKey{typed.binding.value});
                if constexpr (requires { typed.text; })
                    admitted = admitted && AllowsText(context, typed.text);
                if constexpr (requires { typed.message; })
                    admitted = admitted && AllowsText(context, typed.message) && context.Allows(EditorSurfaceDiagnosticCode{typed.code});
                if constexpr (requires { typed.placeholder; })
                    admitted = admitted && AllowsText(context, typed.placeholder);
                if constexpr (requires { typed.options; }) {
                    for (const auto &option : typed.options)
                        admitted = admitted && AllowsText(context, option.label);
                }
                return admitted;
            }, payload);
        }

        /** @brief Admits form text and interactions only through the exact surface context allowlists. */
        [[nodiscard]] bool AllowsForm(const EditorSurfaceContext &context, const EditorUiForm &form) {
            if (!AllowsText(context, form.title) || !AllowsText(context, form.description))
                return false;
            for (const auto &node : form.nodes) {
                if (const auto &base = EditorUiNodeBaseOf(node); !AllowsText(context, base.label) ||
                                                                 !AllowsText(context, base.description) ||
                                                                 !AllowsText(context, base.accessibleLabel))
                    return false;
                if (!AllowsPayload(context, node.payload))
                    return false;
            }
            return true;
        }

        /** @brief Validates staged paired open/focus and destination exclusivity after drawer ownership admission. */
        [[nodiscard]] const ErrorCodeDescriptor *ActivityInitialStateError(EditorSurfaceRegistryState &state,
                                                                           const EditorSurfaceDescriptor &descriptor,
                                                                           const PendingSurfaceState *pending) {
            if (descriptor.activity.has_value()) {
                const auto drawer = FindSurface(state, descriptor.activity->drawerId);
                const auto side =
                    pending && pending->entry.activityPlacement ? pending->entry.activityPlacement->side : descriptor.activity->side;
                const bool open = pending ? pending->entry.open : descriptor.openByDefault;
                if (const bool focused = pending && pending->entry.focused;
                    pending &&
                    (drawer->desiredOpen != open || drawer->desiredFocused != focused || (open && !pending->entry.activityVisible)))
                    return &ExtensionErrors::EditorSurfaceRegistryStateInvalid;
                if (open && std::ranges::any_of(state.surfaces, [side](const auto &candidate) {
                    return candidate->descriptor.activity.has_value() && candidate->desiredOpen &&
                           ActivityPlacementOf(*candidate).side == side;
                }))
                    return &ExtensionErrors::EditorSurfaceRegistryStateInvalid;
            }
            return nullptr;
        }

        /** @brief Admits a provider-owned pair and its pending persisted state before any registry publication. */
        [[nodiscard]] const ErrorCodeDescriptor *ActivityRegistrationError(EditorSurfaceRegistryState &state,
                                                                           const EditorSurfaceDescriptor &descriptor,
                                                                           const EditorSurfaceProviderKey &provider,
                                                                           const PendingSurfaceState *pending) {
            if (descriptor.activity.has_value()) {
                const auto drawer = FindSurface(state, descriptor.activity->drawerId);
                if (drawer == nullptr || drawer->descriptor.kind != EditorSurfaceKind::Panel ||
                    drawer->descriptor.provider != descriptor.provider ||
                    EffectiveStatus(state, *drawer) != EditorSurfaceProviderStatus::Active ||
                    std::ranges::any_of(state.surfaces, [&descriptor](const auto &candidate) {
                    return candidate->descriptor.activity.has_value() &&
                           candidate->descriptor.activity->drawerId == descriptor.activity->drawerId;
                }))
                    return &ExtensionErrors::EditorSurfaceRegistryInvalid;
            }

            if (pending != nullptr && (!SameProvider(pending->entry.provider, provider) || pending->persistence != descriptor.persistence))
                return &ExtensionErrors::EditorSurfaceRegistryInvalid;
            if (pending && pending->entry.activityPlacement && !descriptor.activity)
                return &ExtensionErrors::EditorSurfaceRegistryStateInvalid;
            return ActivityInitialStateError(state, descriptor, pending);
        }

        /** @brief Applies pending workspace intent or the admitted default to a detached surface before publication. */
        void ApplyInitialSurfaceIntent(EditorSurfaceState &surface, const PendingSurfaceState *pending) {
            if (pending == nullptr) {
                surface.desiredOpen = surface.descriptor.openByDefault;
                return;
            }
            surface.desiredOpen = pending->entry.open;
            surface.desiredFocused = pending->entry.focused;
            surface.opaqueState = pending->entry.opaqueState;
            surface.activityUserVisible = pending->entry.activityVisible;
            surface.activityPlacement = pending->entry.activityPlacement;
        }

        /** @brief Withdraws a paired activity/drawer while the caller holds the registry owner lock. */
        void CloseActivityPair(EditorSurfaceRegistryState &state, EditorSurfaceState &activity) {
            activity.desiredOpen = false;
            activity.desiredFocused = false;
            if (const auto drawer = FindSurface(state, activity.descriptor.activity->drawerId)) {
                drawer->desiredOpen = false;
                drawer->desiredFocused = false;
            }
        }

        /** @brief Enforces destination exclusivity without allocations after placement admission. */
        void CloseActivityPeers(EditorSurfaceRegistryState &state, const EditorActivitySide side,
                                const EditorSurfaceState *retained = nullptr) {
            for (const auto &candidate : state.surfaces) {
                if (candidate.get() == retained || !candidate->descriptor.activity || ActivityPlacementOf(*candidate).side != side)
                    continue;
                CloseActivityPair(state, *candidate);
            }
        }

        /** @brief Copies the bounded peer set in stable insertion order before a placement transaction mutates state. */
        [[nodiscard]] std::vector<std::shared_ptr<EditorSurfaceState>> CollectActivityPeers(const EditorSurfaceRegistryState &state,
                                                                                            const EditorActivitySide side,
                                                                                            const std::uint8_t group) {
            std::vector<std::shared_ptr<EditorSurfaceState>> peers;
            peers.reserve(state.surfaces.size());
            for (const auto &candidate : state.surfaces)
                if (candidate->descriptor.activity && ActivityPlacementOf(*candidate).side == side &&
                    ActivityPlacementOf(*candidate).group == group)
                    peers.push_back(candidate);
            std::ranges::sort(peers, [](const auto &left, const auto &right) {
                const auto a = ActivityPlacementOf(*left).order;
                const auto b = ActivityPlacementOf(*right).order;
                return std::tie(a, left->descriptor.id) < std::tie(b, right->descriptor.id);
            });
            return peers;
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
        auto lock = state_->Lock();
        if (state_->shutdown)
            return FailureValue<EditorSurfaceRegistration>(ExtensionErrors::EditorSurfaceRegistryShutdown);
        if (FindSurface(*state_, surfaceId) != nullptr)
            return FailureValue<EditorSurfaceRegistration>(ExtensionErrors::EditorSurfaceRegistryDuplicate);
        if (state_->surfaces.size() >= state_->limits.maximumSurfaces)
            return FailureValue<EditorSurfaceRegistration>(ExtensionErrors::EditorSurfaceRegistryCapacityExceeded);

        const PendingSurfaceState *pending = FindPending(*state_, surfaceId);
        if (const auto *error = ActivityRegistrationError(*state_, descriptor, provider, pending))
            return FailureValue<EditorSurfaceRegistration>(*error);
        if (!HasPreservationCapacity(*state_) ||
            (pending == nullptr && state_->surfaces.size() >= state_->limits.maximumWorkspaceEntries - state_->pending.size()))
            return FailureValue<EditorSurfaceRegistration>(ExtensionErrors::EditorSurfaceRegistryCapacityExceeded);

        if (const EditorSurfaceProviderStatus configured = ConfiguredStatus(*state_, provider);
            configured == EditorSurfaceProviderStatus::Missing)
            SetProviderConfiguredStatus(*state_, provider, EditorSurfaceProviderStatus::Active);

        auto surface = std::make_shared<EditorSurfaceState>(std::move(context));
        ApplyInitialSurfaceIntent(*surface, pending);
        if (descriptor.activity.has_value()) {
            const auto drawer = FindSurface(*state_, descriptor.activity->drawerId);
            drawer->desiredOpen = surface->desiredOpen;
            drawer->desiredFocused = surface->desiredFocused;
        }
        ++state_->revision;
        state_->surfaces.push_back(surface);
        if (pending != nullptr) {
            const auto found = std::ranges::find_if(state_->pending, [&surfaceId](const PendingSurfaceState &candidate) {
                return candidate.entry.surfaceId == surfaceId;
            });
            state_->pending.erase(found);
        }
        return Result<EditorSurfaceRegistration>::Success(EditorSurfaceRegistration{state_, std::move(surface)});
    }

    /** @copydoc EditorSurfaceRegistry::PublishForm */
    Result<void> EditorSurfaceRegistry::PublishForm(const EditorSurfaceProviderIdentity &provider, const std::string_view surfaceId,
                                                    EditorUiForm form) const {
        if (const auto valid = ValidateEditorUiForm(form); valid.HasError())
            return Result<void>::Failure(valid.ErrorValue());
        if (state_ == nullptr)
            return Failure(ExtensionErrors::EditorSurfaceRegistryShutdown);
        auto lock = state_->Lock();
        if (state_->shutdown)
            return Failure(ExtensionErrors::EditorSurfaceRegistryShutdown);
        const auto surface = FindSurface(*state_, surfaceId);
        if (surface == nullptr || surface->descriptor.provider != provider || surface->descriptor.kind != EditorSurfaceKind::Panel)
            return Failure(ExtensionErrors::EditorSurfaceRegistryInvalid);
        if (EffectiveStatus(*state_, *surface) != EditorSurfaceProviderStatus::Active)
            return Failure(ExtensionErrors::EditorSurfaceContextRevoked);
        if (!AllowsForm(surface->context.Context(), form))
            return Failure(ExtensionErrors::EditorSurfaceRegistryInvalid);
        ++state_->revision;
        surface->form = std::move(form);
        return Result<void>::Success();
    }

    /** @copydoc EditorSurfaceRegistry::PublishActivity */
    Result<void> EditorSurfaceRegistry::PublishActivity(const EditorSurfaceProviderIdentity &provider, const std::string_view surfaceId,
                                                        const EditorActivityPresentation presentation) const {
        if (state_ == nullptr)
            return Failure(ExtensionErrors::EditorSurfaceRegistryShutdown);
        auto lock = state_->Lock();
        if (state_->shutdown)
            return Failure(ExtensionErrors::EditorSurfaceRegistryShutdown);
        const auto surface = FindSurface(*state_, surfaceId);
        if (surface == nullptr || surface->descriptor.provider != provider || !surface->descriptor.activity.has_value())
            return Failure(ExtensionErrors::EditorSurfaceRegistryInvalid);
        if (EffectiveStatus(*state_, *surface) != EditorSurfaceProviderStatus::Active)
            return Failure(ExtensionErrors::EditorSurfaceContextRevoked);
        ++state_->revision;
        surface->activity = presentation;
        if (!presentation.visible || !presentation.enabled) {
            surface->desiredOpen = false;
            surface->desiredFocused = false;
            if (const auto drawer = FindSurface(*state_, surface->descriptor.activity->drawerId); drawer != nullptr) {
                drawer->desiredOpen = false;
                drawer->desiredFocused = false;
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorSurfaceRegistry::SetActivityVisibility */
    Result<void> EditorSurfaceRegistry::SetActivityVisibility(const std::string_view surfaceId, const bool visible) const {
        if (state_ == nullptr)
            return Failure(ExtensionErrors::EditorSurfaceRegistryShutdown);
        auto lock = state_->Lock();
        if (state_->shutdown)
            return Failure(ExtensionErrors::EditorSurfaceRegistryShutdown);
        const auto surface = FindSurface(*state_, surfaceId);
        if (surface == nullptr || !surface->descriptor.activity.has_value())
            return Failure(ExtensionErrors::EditorSurfaceRegistryInvalid);
        ++state_->revision;
        surface->activityUserVisible = visible;
        if (!visible) {
            surface->desiredOpen = false;
            surface->desiredFocused = false;
            if (const auto drawer = FindSurface(*state_, surface->descriptor.activity->drawerId); drawer != nullptr) {
                drawer->desiredOpen = false;
                drawer->desiredFocused = false;
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorSurfaceRegistry::ToggleActivity */
    Result<EditorSurfaceOperation> EditorSurfaceRegistry::ToggleActivity(const EditorSurfaceProviderIdentity &provider,
                                                                         const std::string_view surfaceId) const {
        if (state_ == nullptr)
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryShutdown);
        auto lock = state_->Lock();
        if (state_->shutdown)
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryShutdown);
        const auto surface = FindSurface(*state_, surfaceId);
        if (surface == nullptr || surface->descriptor.provider != provider || !surface->descriptor.activity.has_value())
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryInvalid);
        if (const auto status = EffectiveStatus(*state_, *surface); status != EditorSurfaceProviderStatus::Active)
            return ProviderFailure(status);
        if (!surface->activityUserVisible || !surface->activity.visible || !surface->activity.enabled)
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryInvalid);
        const auto drawer = FindSurface(*state_, surface->descriptor.activity->drawerId);
        ++state_->revision;
        const bool close = surface->desiredOpen && drawer->desiredOpen;
        CloseActivityPeers(*state_, ActivityPlacementOf(*surface).side);
        surface->desiredOpen = !close;
        surface->desiredFocused = !close;
        drawer->desiredOpen = !close;
        drawer->desiredFocused = !close;
        return Result<EditorSurfaceOperation>::Success({close ? EditorSurfaceOperationKind::Closed : EditorSurfaceOperationKind::Opened});
    }

    /** @copydoc EditorSurfaceRegistry::MoveActivity */
    Result<void> EditorSurfaceRegistry::MoveActivity(const EditorSurfaceProviderIdentity &provider, const std::string_view surfaceId,
                                                     const EditorActivityPlacement destination) const {
        if (!state_)
            return Failure(ExtensionErrors::EditorSurfaceRegistryShutdown);
        auto lock = state_->Lock();
        if (state_->shutdown)
            return Failure(ExtensionErrors::EditorSurfaceRegistryShutdown);
        const auto surface = FindSurface(*state_, surfaceId);
        if (!surface || !surface->descriptor.activity || surface->descriptor.provider != provider)
            return Failure(ExtensionErrors::EditorSurfaceRegistryInvalid);
        if (EffectiveStatus(*state_, *surface) != EditorSurfaceProviderStatus::Active)
            return Failure(ExtensionErrors::EditorSurfaceContextRevoked);
        if (destination.side > EditorActivitySide::Bottom || destination.group > 2 || destination.order < 0)
            return Failure(ExtensionErrors::EditorSurfaceRegistryStateInvalid);
        const auto source = ActivityPlacementOf(*surface);
        auto targetPeers = CollectActivityPeers(*state_, destination.side, destination.group);
        if (static_cast<std::size_t>(destination.order) > targetPeers.size())
            return Failure(ExtensionErrors::EditorSurfaceRegistryStateInvalid);
        std::size_t insertion = static_cast<std::size_t>(destination.order);
        if (const auto found = std::ranges::find(targetPeers, surface); found != targetPeers.end()) {
            if (static_cast<std::size_t>(found - targetPeers.begin()) < insertion)
                --insertion;
            targetPeers.erase(found);
        }
        auto sourcePeers = CollectActivityPeers(*state_, source.side, source.group);
        std::erase(sourcePeers, surface);
        targetPeers.insert(targetPeers.begin() + static_cast<std::ptrdiff_t>(insertion), surface);
        // Every allocation and slot check precedes publication; the following bounded mutation cannot fail.
        if (source.side != destination.side || source.group != destination.group)
            for (std::size_t index = 0; index < sourcePeers.size(); ++index)
                sourcePeers[index]->activityPlacement =
                    EditorActivityPlacement{source.side, source.group, static_cast<std::int32_t>(index)};
        for (std::size_t index = 0; index < targetPeers.size(); ++index)
            targetPeers[index]->activityPlacement =
                EditorActivityPlacement{destination.side, destination.group, static_cast<std::int32_t>(index)};
        if (surface->desiredOpen)
            CloseActivityPeers(*state_, destination.side, surface.get());
        ++state_->revision;
        return Result<void>::Success();
    }

    /** @copydoc EditorSurfaceRegistry::Open */
    Result<EditorSurfaceOperation> EditorSurfaceRegistry::Open(const std::string_view surfaceId) const {
        if (state_ == nullptr)
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryShutdown);
        auto lock = state_->Lock();
        if (state_->shutdown)
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryShutdown);
        const auto surface = FindSurface(*state_, surfaceId);
        if (surface == nullptr) {
            if (const PendingSurfaceState *pending = FindPending(*state_, surfaceId); pending != nullptr)
                return ProviderFailure(ConfiguredStatus(*state_, pending->entry.provider));
            return UnknownOperation();
        }
        if (const EditorSurfaceProviderStatus status = EffectiveStatus(*state_, *surface); status != EditorSurfaceProviderStatus::Active)
            return ProviderFailure(status);
        if (surface->descriptor.activity.has_value() || std::ranges::any_of(state_->surfaces, [&surfaceId](const auto &candidate) {
            return candidate->descriptor.activity.has_value() && candidate->descriptor.activity->drawerId == surfaceId;
        }))
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryInvalid);
        if (surface->desiredOpen) {
            if (surface->desiredFocused)
                return Result<EditorSurfaceOperation>::Success({EditorSurfaceOperationKind::AlreadyOpen});
            ++state_->revision;
            surface->desiredFocused = true;
            return Result<EditorSurfaceOperation>::Success({EditorSurfaceOperationKind::Focused});
        }
        surface->desiredOpen = true;
        ++state_->revision;
        surface->desiredFocused = true;
        return Result<EditorSurfaceOperation>::Success({EditorSurfaceOperationKind::Opened});
    }

    /** @copydoc EditorSurfaceRegistry::Focus */
    Result<EditorSurfaceOperation> EditorSurfaceRegistry::Focus(const std::string_view surfaceId) const {
        if (state_ == nullptr)
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryShutdown);
        auto lock = state_->Lock();
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
        if (surface->descriptor.activity.has_value() || std::ranges::any_of(state_->surfaces, [&surfaceId](const auto &candidate) {
            return candidate->descriptor.activity.has_value() && candidate->descriptor.activity->drawerId == surfaceId;
        }))
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryInvalid);
        if (!surface->desiredOpen)
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistrySurfaceClosed);
        if (surface->desiredFocused)
            return Result<EditorSurfaceOperation>::Success({EditorSurfaceOperationKind::AlreadyFocused});
        ++state_->revision;
        surface->desiredFocused = true;
        return Result<EditorSurfaceOperation>::Success({EditorSurfaceOperationKind::Focused});
    }

    /** @copydoc EditorSurfaceRegistry::Close */
    Result<EditorSurfaceOperation> EditorSurfaceRegistry::Close(const std::string_view surfaceId) const {
        using enum EditorSurfaceOperationKind;
        if (state_ == nullptr)
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryShutdown);
        auto lock = state_->Lock();
        if (state_->shutdown)
            return FailureValue<EditorSurfaceOperation>(ExtensionErrors::EditorSurfaceRegistryShutdown);
        if (const std::shared_ptr<EditorSurfaceState> surface = FindSurface(*state_, surfaceId); surface != nullptr) {
            ++state_->revision;
            const bool wasOpen = surface->desiredOpen;
            for (const auto &candidate : state_->surfaces) {
                if (candidate->descriptor.activity.has_value() &&
                    (candidate == surface || candidate->descriptor.activity->drawerId == surfaceId)) {
                    candidate->desiredOpen = false;
                    candidate->desiredFocused = false;
                    if (const auto drawer = FindSurface(*state_, candidate->descriptor.activity->drawerId); drawer != nullptr) {
                        drawer->desiredOpen = false;
                        drawer->desiredFocused = false;
                    }
                }
            }
            if (!wasOpen)
                return Result<EditorSurfaceOperation>::Success({AlreadyClosed});
            surface->desiredOpen = false;
            surface->desiredFocused = false;
            return Result<EditorSurfaceOperation>::Success({Closed});
        }
        if (PendingSurfaceState *pending = FindPending(*state_, surfaceId); pending != nullptr) {
            if (!pending->entry.open)
                return Result<EditorSurfaceOperation>::Success({AlreadyClosed});
            ++state_->revision;
            pending->entry.open = false;
            pending->entry.focused = false;
            return Result<EditorSurfaceOperation>::Success({Closed});
        }
        return UnknownOperation();
    }

    /** @copydoc EditorSurfaceRegistry::SetProviderStatus */
    Result<void> EditorSurfaceRegistry::SetProviderStatus(const EditorSurfaceProviderKey &provider,
                                                          const EditorSurfaceProviderStatus status) const {
        if (!IsValidProviderKey(provider) || !IsValidStatus(status))
            return Failure(ExtensionErrors::EditorSurfaceRegistryInvalid);
        if (state_ == nullptr)
            return Failure(ExtensionErrors::EditorSurfaceRegistryShutdown);
        auto lock = state_->Lock();
        if (state_->shutdown)
            return Failure(ExtensionErrors::EditorSurfaceRegistryShutdown);
        ++state_->revision;
        SetProviderConfiguredStatus(*state_, provider, status);
        return Result<void>::Success();
    }

    /** @copydoc EditorSurfaceRegistry::SetOpaqueState */
    Result<void> EditorSurfaceRegistry::SetOpaqueState(const std::string_view surfaceId, const std::span<const std::uint8_t> state) const {
        if (state_ == nullptr)
            return Failure(ExtensionErrors::EditorSurfaceRegistryShutdown);
        auto lock = state_->Lock();
        if (state_->shutdown)
            return Failure(ExtensionErrors::EditorSurfaceRegistryShutdown);
        if (!state_->validLimits || state.size() > state_->limits.maximumOpaqueStateBytes ||
            !FitsTotalOpaqueState(*state_, surfaceId, state.size()))
            return Failure(ExtensionErrors::EditorSurfaceRegistryStateInvalid);
        const std::shared_ptr<EditorSurfaceState> surface = FindSurface(*state_, surfaceId);
        if (surface == nullptr)
            return Failure(ExtensionErrors::EditorSurfaceRegistryUnknown);
        ++state_->revision;
        surface->opaqueState.assign(state.begin(), state.end());
        return Result<void>::Success();
    }

}  // namespace Horo::Extensions
