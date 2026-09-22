#include "Horo/Extensions/EditorSurfaceContext.h"

#include "../ExtensionAuthorityIdentityValidation.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <ranges>
#include <string>
#include <utility>

namespace Horo::Extensions {
    struct EditorSurfaceContextState final {
        explicit EditorSurfaceContextState(ExtensionActivationLease activationLease) : activation(std::move(activationLease)) {}

        EditorSurfaceContextDescriptor descriptor;
        ExtensionActivationLease activation;
        std::vector<ExtensionCapabilityHandle> capabilities;
        std::atomic_bool active{true};
    };

    struct EditorSurfaceContextProviderState final {
        std::mutex mutex;
        std::vector<std::shared_ptr<EditorSurfaceContextState>> contexts;
        bool shutdown{};
    };

    namespace {
        template <typename Id> [[nodiscard]] bool IsCanonicalAccessId(const Id &identity, const std::size_t maximumBytes) noexcept {
            return identity.value.size() <= maximumBytes && Detail::IsCanonicalExtensionAuthorityId(identity.value);
        }

        template <typename Id> [[nodiscard]] bool IsCanonicalLocalizationKey(const Id &key, const std::size_t maximumBytes) noexcept {
            return IsCanonicalAccessId(key, maximumBytes) && key.value.find('.') != std::string::npos;
        }

        template <typename Id>
        [[nodiscard]] Result<void> ValidateAccessList(const std::vector<Id> &identities, const std::size_t maximumBytes,
                                                      const std::size_t maximumCount, const bool localization) {
            if (identities.size() > maximumCount)
                return Result<void>::Failure(
                    MakeError(ExtensionErrors::EditorSurfaceContextCapacityExceeded, "An editor surface access list exceeds its bound."));
            for (std::size_t index = 0; index < identities.size(); ++index) {
                if (const bool valid = localization ? IsCanonicalLocalizationKey(identities[index], maximumBytes)
                                                    : IsCanonicalAccessId(identities[index], maximumBytes);
                    !valid)
                    return Result<void>::Failure(
                        MakeError(ExtensionErrors::EditorSurfaceContextInvalid, "An editor surface access identity is malformed."));
                for (std::size_t previous = 0; previous < index; ++previous) {
                    if (identities[previous].value == identities[index].value)
                        return Result<void>::Failure(
                            MakeError(ExtensionErrors::EditorSurfaceContextInvalid, "An editor surface access identity is duplicated."));
                }
            }
            return Result<void>::Success();
        }

        template <typename Id> [[nodiscard]] bool Contains(const std::vector<Id> &identities, const Id &identity) noexcept {
            return std::ranges::find(identities, identity) != identities.end();
        }

        [[nodiscard]] bool Matches(const EditorSurfaceProviderIdentity &expected, const ExtensionActivationIdentity &actual) noexcept {
            return expected.extensionId == actual.ExtensionId() && expected.moduleId == actual.ModuleId() &&
                   expected.activationGeneration == actual.Generation();
        }

        [[nodiscard]] bool Matches(const EditorSurfaceProviderIdentity &expected, const ExtensionCapabilityHandle &actual) noexcept {
            return Matches(expected, actual.Activation());
        }

        [[nodiscard]] Result<std::vector<ExtensionCapabilityHandle>> ValidateApprovedCapabilities(
            const EditorSurfaceDescriptor &descriptor, const EditorSurfaceProviderIdentity &provider,
            const std::span<const ExtensionCapabilityHandle> approvedCapabilities, const std::size_t maximumCapabilities) {
            if (approvedCapabilities.size() > maximumCapabilities)
                return Result<std::vector<ExtensionCapabilityHandle>>::Failure(
                    MakeError(ExtensionErrors::EditorSurfaceContextCapacityExceeded));

            std::vector<ExtensionCapabilityHandle> capabilities;
            capabilities.reserve(approvedCapabilities.size());
            for (const ExtensionCapabilityHandle &capability : approvedCapabilities) {
                if (!capability.IsUsable())
                    return Result<std::vector<ExtensionCapabilityHandle>>::Failure(MakeError(ExtensionErrors::CapabilityRevoked));
                if (!Matches(provider, capability))
                    return Result<std::vector<ExtensionCapabilityHandle>>::Failure(
                        MakeError(ExtensionErrors::EditorSurfaceContextProviderMismatch));
                if (!Contains(descriptor.requiredCapabilities, capability.Capability()))
                    return Result<std::vector<ExtensionCapabilityHandle>>::Failure(
                        MakeError(ExtensionErrors::CapabilityUnavailable, "Capability is not declared by the surface descriptor."));
                if (std::ranges::find(capabilities, capability.Capability(), &ExtensionCapabilityHandle::Capability) != capabilities.end())
                    return Result<std::vector<ExtensionCapabilityHandle>>::Failure(
                        MakeError(ExtensionErrors::EditorSurfaceContextInvalid, "A surface capability grant is duplicated."));
                capabilities.push_back(capability);
            }
            std::ranges::sort(capabilities, {}, [](const ExtensionCapabilityHandle &capability) {
                return capability.Capability().value;
            });
            return Result<std::vector<ExtensionCapabilityHandle>>::Success(std::move(capabilities));
        }

        [[nodiscard]] const EditorSurfaceDescriptor &EmptySurface() noexcept {
            static const EditorSurfaceDescriptor empty;
            return empty;
        }

        [[nodiscard]] const EditorSurfaceProviderIdentity &EmptyProvider() noexcept {
            static const EditorSurfaceProviderIdentity empty;
            return empty;
        }

        void RemoveContext(const std::shared_ptr<EditorSurfaceContextProviderState> &provider,
                           const std::shared_ptr<EditorSurfaceContextState> &context) noexcept {
            std::scoped_lock lock{provider->mutex};
            context->active.store(false, std::memory_order_release);
            std::erase(provider->contexts, context);
        }
    }  // namespace

    /** @copydoc ValidateEditorSurfaceContextDescriptor */
    Result<void> ValidateEditorSurfaceContextDescriptor(const EditorSurfaceContextDescriptor &descriptor,
                                                        const EditorSurfaceContextLimits &limits) {
        if (const Result<void> surface = ValidateEditorSurfaceDescriptor(descriptor.surface); surface.HasError())
            return Result<void>::Failure(surface.ErrorValue());
        if (const Result<void> commands =
                ValidateAccessList(descriptor.commands, limits.maximumIdentityBytes, limits.maximumCommands, false);
            commands.HasError())
            return commands;
        if (const Result<void> state = ValidateAccessList(descriptor.state, limits.maximumIdentityBytes, limits.maximumStateKeys, false);
            state.HasError())
            return state;
        if (const Result<void> services =
                ValidateAccessList(descriptor.services, limits.maximumIdentityBytes, limits.maximumServices, false);
            services.HasError())
            return services;
        if (const Result<void> localization =
                ValidateAccessList(descriptor.localization, limits.maximumLocalizationKeyBytes, limits.maximumLocalizationKeys, true);
            localization.HasError())
            return localization;
        return ValidateAccessList(descriptor.diagnostics, limits.maximumIdentityBytes, limits.maximumDiagnostics, false);
    }

    EditorSurfaceContext::EditorSurfaceContext(std::shared_ptr<const EditorSurfaceContextState> state) noexcept
        : state_(std::move(state)) {}

    /** @copydoc EditorSurfaceContext::IsUsable */
    bool EditorSurfaceContext::IsUsable() const noexcept {
        return state_ != nullptr && state_->active.load(std::memory_order_acquire) && state_->activation.IsUsable();
    }

    /** @copydoc EditorSurfaceContext::Surface */
    const EditorSurfaceDescriptor &EditorSurfaceContext::Surface() const noexcept {
        return state_ == nullptr ? EmptySurface() : state_->descriptor.surface;
    }

    /** @copydoc EditorSurfaceContext::Provider */
    const EditorSurfaceProviderIdentity &EditorSurfaceContext::Provider() const noexcept {
        return state_ == nullptr ? EmptyProvider() : state_->descriptor.surface.provider;
    }

    /** @copydoc EditorSurfaceContext::Commands */
    std::span<const EditorSurfaceCommandId> EditorSurfaceContext::Commands() const noexcept {
        return state_ == nullptr ? std::span<const EditorSurfaceCommandId>{} : state_->descriptor.commands;
    }

    /** @copydoc EditorSurfaceContext::State */
    std::span<const EditorSurfaceStateKey> EditorSurfaceContext::State() const noexcept {
        return state_ == nullptr ? std::span<const EditorSurfaceStateKey>{} : state_->descriptor.state;
    }

    /** @copydoc EditorSurfaceContext::Services */
    std::span<const EditorSurfaceServiceId> EditorSurfaceContext::Services() const noexcept {
        return state_ == nullptr ? std::span<const EditorSurfaceServiceId>{} : state_->descriptor.services;
    }

    /** @copydoc EditorSurfaceContext::Localization */
    std::span<const EditorSurfaceLocalizationKey> EditorSurfaceContext::Localization() const noexcept {
        return state_ == nullptr ? std::span<const EditorSurfaceLocalizationKey>{} : state_->descriptor.localization;
    }

    /** @copydoc EditorSurfaceContext::Diagnostics */
    std::span<const EditorSurfaceDiagnosticCode> EditorSurfaceContext::Diagnostics() const noexcept {
        return state_ == nullptr ? std::span<const EditorSurfaceDiagnosticCode>{} : state_->descriptor.diagnostics;
    }

    /** @copydoc EditorSurfaceContext::AcquireCapabilityUse */
    Result<ExtensionCapabilityUseLease> EditorSurfaceContext::AcquireCapabilityUse(const ExtensionCapabilityId &capability) const {
        if (!IsUsable())
            return Result<ExtensionCapabilityUseLease>::Failure(MakeError(ExtensionErrors::EditorSurfaceContextRevoked));
        const auto found = std::ranges::find(state_->capabilities, capability, &ExtensionCapabilityHandle::Capability);
        if (found == state_->capabilities.end())
            return Result<ExtensionCapabilityUseLease>::Failure(
                MakeError(ExtensionErrors::CapabilityUnavailable, "Capability is not approved for this editor surface."));
        const EditorSurfaceProviderIdentity &provider = state_->descriptor.surface.provider;
        return found->AcquireUse(provider.extensionId, provider.moduleId, provider.activationGeneration);
    }

    /** @copydoc EditorSurfaceContext::Allows(const EditorSurfaceCommandId &) */
    bool EditorSurfaceContext::Allows(const EditorSurfaceCommandId &command) const noexcept {
        return std::ranges::find(Commands(), command) != Commands().end();
    }

    /** @copydoc EditorSurfaceContext::Allows(const EditorSurfaceStateKey &) */
    bool EditorSurfaceContext::Allows(const EditorSurfaceStateKey &state) const noexcept {
        return std::ranges::find(State(), state) != State().end();
    }

    /** @copydoc EditorSurfaceContext::Allows(const EditorSurfaceServiceId &) */
    bool EditorSurfaceContext::Allows(const EditorSurfaceServiceId &service) const noexcept {
        return std::ranges::find(Services(), service) != Services().end();
    }

    /** @copydoc EditorSurfaceContext::Allows(const EditorSurfaceLocalizationKey &) */
    bool EditorSurfaceContext::Allows(const EditorSurfaceLocalizationKey &key) const noexcept {
        return std::ranges::find(Localization(), key) != Localization().end();
    }

    /** @copydoc EditorSurfaceContext::Allows(const EditorSurfaceDiagnosticCode &) */
    bool EditorSurfaceContext::Allows(const EditorSurfaceDiagnosticCode &diagnostic) const noexcept {
        return std::ranges::find(Diagnostics(), diagnostic) != Diagnostics().end();
    }

    EditorSurfaceContextRegistration::EditorSurfaceContextRegistration(std::weak_ptr<EditorSurfaceContextProviderState> provider,
                                                                       std::shared_ptr<EditorSurfaceContextState> context,
                                                                       EditorSurfaceContext view) noexcept
        : provider_(std::move(provider)), context_(std::move(context)), view_(std::move(view)) {}

    EditorSurfaceContextRegistration::~EditorSurfaceContextRegistration() noexcept {
        Reset();
    }

    EditorSurfaceContextRegistration::EditorSurfaceContextRegistration(EditorSurfaceContextRegistration &&other) noexcept
        : provider_(std::move(other.provider_)), context_(std::move(other.context_)), view_(std::move(other.view_)) {}

    EditorSurfaceContextRegistration &EditorSurfaceContextRegistration::operator=(EditorSurfaceContextRegistration &&other) noexcept {
        if (this == &other)
            return *this;
        Reset();
        provider_ = std::move(other.provider_);
        context_ = std::move(other.context_);
        view_ = std::move(other.view_);
        return *this;
    }

    /** @copydoc EditorSurfaceContextRegistration::Reset */
    void EditorSurfaceContextRegistration::Reset() const noexcept {
        if (context_ == nullptr)
            return;
        const std::shared_ptr<EditorSurfaceContextState> context = context_;
        if (auto provider = provider_.lock())
            RemoveContext(provider, context);
        else
            context->active.store(false, std::memory_order_release);
        provider_.reset();
        context_.reset();
        view_ = EditorSurfaceContext{std::shared_ptr<const EditorSurfaceContextState>{}};
    }

    /** @copydoc EditorSurfaceContextRegistration::IsRegistered */
    bool EditorSurfaceContextRegistration::IsRegistered() const noexcept {
        return context_ != nullptr && context_->active.load(std::memory_order_acquire) && context_->activation.IsUsable();
    }

    /** @copydoc EditorSurfaceContextRegistration::Context */
    const EditorSurfaceContext &EditorSurfaceContextRegistration::Context() const noexcept {
        return view_;
    }

    EditorSurfaceContextProvider::EditorSurfaceContextProvider(const EditorSurfaceContextLimits &limits)
        : state_(std::make_shared<EditorSurfaceContextProviderState>()), limits_(limits) {
        state_->contexts.reserve(limits_.maximumContexts);
    }

    EditorSurfaceContextProvider::~EditorSurfaceContextProvider() noexcept {
        BeginShutdown();
    }

    /** @copydoc EditorSurfaceContextProvider::Attach */
    // Attachment mutates provider-owned shared lifecycle state.
    Result<EditorSurfaceContextRegistration> EditorSurfaceContextProvider::Attach(  // NOSONAR(cpp:S5817)
        EditorSurfaceContextDescriptor descriptor, ExtensionActivationLease activation,
        const std::span<const ExtensionCapabilityHandle> approvedCapabilities) {
        if (const Result<void> valid = ValidateEditorSurfaceContextDescriptor(descriptor, limits_); valid.HasError())
            return Result<EditorSurfaceContextRegistration>::Failure(valid.ErrorValue());
        if (!activation.IsUsable())
            return Result<EditorSurfaceContextRegistration>::Failure(MakeError(ExtensionErrors::EditorSurfaceContextRevoked));
        const EditorSurfaceProviderIdentity &provider = descriptor.surface.provider;
        if (!Matches(provider, activation.Activation()))
            return Result<EditorSurfaceContextRegistration>::Failure(MakeError(ExtensionErrors::EditorSurfaceContextProviderMismatch));
        auto validatedCapabilities =
            ValidateApprovedCapabilities(descriptor.surface, provider, approvedCapabilities, limits_.maximumCapabilities);
        if (validatedCapabilities.HasError())
            return Result<EditorSurfaceContextRegistration>::Failure(validatedCapabilities.ErrorValue());

        if (state_ == nullptr)
            return Result<EditorSurfaceContextRegistration>::Failure(MakeError(ExtensionErrors::EditorSurfaceContextShutdown));
        auto context = std::make_shared<EditorSurfaceContextState>(std::move(activation));
        context->descriptor = std::move(descriptor);
        context->capabilities = std::move(validatedCapabilities).Value();

        {
            std::scoped_lock lock{state_->mutex};
            if (state_->shutdown)
                return Result<EditorSurfaceContextRegistration>::Failure(MakeError(ExtensionErrors::EditorSurfaceContextShutdown));
            if (state_->contexts.size() >= limits_.maximumContexts)
                return Result<EditorSurfaceContextRegistration>::Failure(MakeError(ExtensionErrors::EditorSurfaceContextCapacityExceeded));
            state_->contexts.push_back(context);
        }
        return Result<EditorSurfaceContextRegistration>::Success(
            EditorSurfaceContextRegistration{state_, context, EditorSurfaceContext{context}});
    }

    /** @copydoc EditorSurfaceContextProvider::BeginShutdown */
    // Shutdown mutates provider-owned shared lifecycle state.
    void EditorSurfaceContextProvider::BeginShutdown() noexcept {  // NOSONAR(cpp:S5817)
        if (state_ == nullptr)
            return;
        std::scoped_lock lock{state_->mutex};
        state_->shutdown = true;
        for (const auto &context : state_->contexts)
            context->active.store(false, std::memory_order_release);
        state_->contexts.clear();
    }

    /** @copydoc EditorSurfaceContextProvider::IsShutdown */
    bool EditorSurfaceContextProvider::IsShutdown() const noexcept {
        if (state_ == nullptr)
            return true;
        std::scoped_lock lock{state_->mutex};
        return state_->shutdown;
    }
}  // namespace Horo::Extensions
