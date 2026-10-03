#include "Horo/Extensions/ExtensionErrors.h"
#include "ScriptCapabilityContextInternal.h"

#include <algorithm>
#include <utility>

namespace Horo::Extensions {
    namespace {
        /** @brief Accepts bounded canonical identifiers, excluding free text and private paths. */
        bool Identity(std::string_view value) {
            return !value.empty() && value.size() <= 256 && std::ranges::all_of(value, [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
            });
        }

        /** @brief Checks a finite canonical allowlist before using its membership. */
        bool ValidCapabilities(const std::vector<ExtensionCapabilityId> &values) {
            return values.size() <= 128 && std::ranges::all_of(values, [](const auto &v) {
                return Identity(v.value);
            });
        }

        /** @brief Checks required lifecycle boundaries without inferring gameplay access. */
        bool ValidScopes(const ScriptCapabilityContextDescriptor &descriptor) {
            using enum ScriptCapabilityScopeKind;
            bool project = false;
            bool runtime = false;
            bool scene = false;
            for (const auto &scope : descriptor.scopes) {
                if (!scope.IsUsable() || scope.Kind() >= Count)
                    return false;
                project |= scope.Kind() == Project;
                runtime |= scope.Kind() == Runtime;
                scene |= scope.Kind() == Scene;
            }
            return project && (descriptor.profile != ScriptCapabilityProfile::Gameplay || (runtime && scene));
        }

        /** @brief Validates one required import against every policy layer and exact provider owner. */
        bool ValidImport(const ScriptCapabilityImport &importedApi, const ScriptCapabilityContextDescriptor &context) {
            const auto *api = importedApi.descriptors ? importedApi.descriptors->Find(importedApi.apiId) : nullptr;
            if (!api || importedApi.profile != context.profile || !importedApi.versions.minimum.IsValid() ||
                importedApi.versions.minimum.major != importedApi.versions.maximum.major ||
                importedApi.versions.minimum > importedApi.versions.maximum || api->version < importedApi.versions.minimum ||
                api->version > importedApi.versions.maximum)
                return false;
            const auto &grant = importedApi.capability;
            if (const auto &owner = grant.Activation();
                !grant.IsUsable() || !importedApi.provider.IsUsable() || importedApi.provider.Generation() != owner.Generation() ||
                !Identity(owner.ExtensionId()) || owner.ModuleId() != api->moduleId || grant.Capability() != api->service.capability)
                return false;
            if (importedApi.functions.size() > 256)
                return false;
            for (std::size_t index = 0; index < importedApi.functions.size(); ++index) {
                const auto &function = importedApi.functions[index];
                if (!Identity(function) ||
                    std::ranges::find(api->functions, function, &ScriptExportFunctionDescriptor::id) == api->functions.end())
                    return false;
                for (std::size_t prior = 0; prior < index; ++prior)
                    if (importedApi.functions[prior] == function)
                        return false;
            }
            const auto permits = [&](const auto &layer) {
                return std::ranges::find(layer, grant.Capability()) != layer.end();
            };
            return permits(context.packageCapabilities) && permits(context.scriptCapabilities) && permits(context.projectCapabilities) &&
                   permits(context.contextCapabilities);
        }

        /** @brief Returns a bounded stable denial without reflecting supplied text. */
        template <typename T> Result<T> Denied() {
            return Result<T>::Failure(
                MakeError(ExtensionErrors::ScriptInvocationUnavailable, "Script import authority denied or revoked."));
        }
    }  // namespace

    ScriptCapabilityScopeLease::ScriptCapabilityScopeLease(ScriptCapabilityScopeKind kind, std::shared_ptr<const std::atomic<bool>> active)
        : kind_(kind), active_(std::move(active)) {}

    /** @copydoc ScriptCapabilityScopeLease::IsUsable */
    bool ScriptCapabilityScopeLease::IsUsable() const noexcept {
        return active_ && active_->load();
    }

    /** @copydoc ScriptCapabilityScopeLease::Kind */
    ScriptCapabilityScopeKind ScriptCapabilityScopeLease::Kind() const noexcept {
        return kind_;
    }

    /** @copydoc ScriptCapabilityScope::ScriptCapabilityScope */
    ScriptCapabilityScope::ScriptCapabilityScope(ScriptCapabilityScopeKind kind)
        : kind_(kind), active_(std::make_shared<std::atomic<bool>>(kind < ScriptCapabilityScopeKind::Count)) {}

    /** @copydoc ScriptCapabilityScope::~ScriptCapabilityScope */
    ScriptCapabilityScope::~ScriptCapabilityScope() noexcept {
        Revoke();
    }

    /** @copydoc ScriptCapabilityScope::Revoke */
    void ScriptCapabilityScope::Revoke() const noexcept {
        active_->store(false);
    }

    /** @copydoc ScriptCapabilityScope::Lease */
    ScriptCapabilityScopeLease ScriptCapabilityScope::Lease() const {
        return {kind_, active_};
    }

    ScriptCapabilityContextState::ScriptCapabilityContextState(std::shared_ptr<ScriptInvocationRegistry> registryIn,
                                                               ScriptInvocationContextRegistration registrationIn,
                                                               ScriptCapabilityContextDescriptor descriptorIn, CancellationToken parent)
        : registry(std::move(registryIn)), registration(std::move(registrationIn)), descriptor(std::move(descriptorIn)),
          parentCancellation(std::move(parent)) {}

    bool ScriptCapabilityContextState::IsUsable() const noexcept {
        return active.load() && !parentCancellation.IsCancellationRequested() &&
               std::ranges::all_of(descriptor.scopes, [](const auto &scope) {
            return scope.IsUsable();
        });
    }

    bool ScriptCapabilityBindingState::MatchesProvider(const std::shared_ptr<ScriptInvocationProviderState> &provider) const noexcept {
        return context && context->descriptor.imports[importIndex].provider.provider_.get() == provider.get();
    }

    bool ScriptCapabilityBindingState::IsUsable() const noexcept {
        return context && context->IsUsable() && context->descriptor.imports[importIndex].capability.IsUsable();
    }

    ScriptCapabilityBinding::ScriptCapabilityBinding(std::shared_ptr<const ScriptCapabilityBindingState> state)
        : state_(std::move(state)) {}

    /** @copydoc ScriptCapabilityBinding::IsUsable */
    bool ScriptCapabilityBinding::IsUsable() const noexcept {
        return state_ && state_->IsUsable() && state_->context->descriptor.imports[state_->importIndex].provider.IsUsable() &&
               state_->context->registration.IsRegistered() && !state_->context->registry->IsShutdown();
    }

    /** @copydoc ScriptCapabilityBinding::Begin */
    Result<ScriptInvocationController> ScriptCapabilityBinding::Begin(const ScriptInvocationProviderRegistration &provider,
                                                                      ScriptInvocationRequest request, std::uint64_t operationId) const {
        if (!IsUsable())
            return Denied<ScriptInvocationController>();
        const auto &importedApi = state_->context->descriptor.imports[state_->importIndex];
        const auto &owner = importedApi.capability.Activation();
        if (std::ranges::find(importedApi.functions, request.functionId) == importedApi.functions.end())
            return Denied<ScriptInvocationController>();
        if (!importedApi.provider.Matches(provider) || provider.Generation() != owner.Generation())
            return Denied<ScriptInvocationController>();
        if (auto use = importedApi.capability.AcquireUse(owner.ExtensionId(), owner.ModuleId(), owner.Generation()); use.HasError()) {
            return Result<ScriptInvocationController>::Failure(use.ErrorValue());
        } else {
            // Retain the capability-use lease through registry admission.
            request.descriptors = importedApi.descriptors;
            request.apiId = importedApi.apiId;
            request.authority = state_;
            request.diagnostics = {.packageId = owner.ExtensionId(),
                                   .moduleId = owner.ModuleId(),
                                   .scriptId = state_->context->descriptor.scriptId,
                                   .apiId = importedApi.apiId,
                                   .policyRevision = state_->context->descriptor.policyRevision,
                                   .operationId = operationId};
            return state_->context->registry->Begin(state_->context->registration, provider, std::move(request));
        }
    }

    ScriptCapabilityContext::ScriptCapabilityContext(std::shared_ptr<ScriptCapabilityContextState> state) : state_(std::move(state)) {}

    /** @copydoc ScriptCapabilityContext::~ScriptCapabilityContext */
    ScriptCapabilityContext::~ScriptCapabilityContext() noexcept {
        Reset();
    }

    /** @copydoc ScriptCapabilityContext::ScriptCapabilityContext */
    ScriptCapabilityContext::ScriptCapabilityContext(ScriptCapabilityContext &&other) noexcept : state_(std::move(other.state_)) {}

    /** @copydoc ScriptCapabilityContext::operator= */
    ScriptCapabilityContext &ScriptCapabilityContext::operator=(ScriptCapabilityContext &&other) noexcept {
        if (this != &other) {
            Reset();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    /** @copydoc ScriptCapabilityContext::Create */
    Result<ScriptCapabilityContext> ScriptCapabilityContext::Create(std::shared_ptr<ScriptInvocationRegistry> registry,
                                                                    ScriptCapabilityContextDescriptor descriptor,
                                                                    ScriptInvocationContextDescriptor invocation) {
        if (!registry || !Identity(descriptor.scriptId) || !descriptor.policyRevision ||
            descriptor.profile >= ScriptCapabilityProfile::Count || !descriptor.packageVerified || !descriptor.packageTrusted ||
            !descriptor.scriptTrusted || !descriptor.runtimeCompatible || descriptor.imports.size() > 128 || descriptor.scopes.size() > 4 ||
            !ValidScopes(descriptor))
            return Denied<ScriptCapabilityContext>();
        if (!ValidCapabilities(descriptor.packageCapabilities) || !ValidCapabilities(descriptor.scriptCapabilities) ||
            !ValidCapabilities(descriptor.projectCapabilities) || !ValidCapabilities(descriptor.contextCapabilities))
            return Denied<ScriptCapabilityContext>();
        for (std::size_t index = 0; index < descriptor.imports.size(); ++index) {
            const auto &importedApi = descriptor.imports[index];
            if (!ValidImport(importedApi, descriptor))
                return Denied<ScriptCapabilityContext>();
            for (std::size_t prior = 0; prior < index; ++prior)
                if (descriptor.imports[prior].apiId == importedApi.apiId)
                    return Denied<ScriptCapabilityContext>();
        }
        const auto parent = invocation.parentCancellation;
        auto registered = registry->RegisterContext(std::move(invocation));
        if (registered.HasError())
            return Result<ScriptCapabilityContext>::Failure(registered.ErrorValue());
        return Result<ScriptCapabilityContext>::Success(
            ScriptCapabilityContext{std::make_shared<ScriptCapabilityContextState>(std::move(registry), std::move(registered).Value(),
                                                                                   std::move(descriptor), parent)});
    }

    /** @copydoc ScriptCapabilityContext::Bind */
    Result<ScriptCapabilityBinding> ScriptCapabilityContext::Bind(std::string_view apiId) const {
        if (!state_ || !state_->IsUsable() || !state_->registration.IsRegistered() || state_->registry->IsShutdown())
            return Denied<ScriptCapabilityBinding>();
        for (std::size_t index = 0; index < state_->descriptor.imports.size(); ++index) {
            if (state_->descriptor.imports[index].apiId != apiId)
                continue;
            auto binding = std::make_shared<ScriptCapabilityBindingState>();
            binding->context = state_;
            binding->importIndex = index;
            if (!binding->IsUsable() || !state_->descriptor.imports[index].provider.IsUsable())
                return Denied<ScriptCapabilityBinding>();
            return Result<ScriptCapabilityBinding>::Success(ScriptCapabilityBinding{std::move(binding)});
        }
        return Denied<ScriptCapabilityBinding>();
    }

    /** @copydoc ScriptCapabilityContext::Reset */
    void ScriptCapabilityContext::Reset() const noexcept {
        if (state_) {
            state_->active.store(false);
            state_->registration.Reset();
        }
    }

    /** @copydoc ScriptCapabilityContext::Drain */
    Result<std::vector<ScriptInvocationEvent>> ScriptCapabilityContext::Drain(std::size_t maximumEvents) const {
        if (!state_ || !state_->IsUsable()) {
            Reset();
            return Denied<std::vector<ScriptInvocationEvent>>();
        }
        return state_->registry->Drain(state_->registration, maximumEvents);
    }
}  // namespace Horo::Extensions
