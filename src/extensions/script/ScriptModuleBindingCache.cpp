#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/ScriptModuleRuntime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>
#include <thread>

namespace Horo::Extensions {
    struct ScriptModuleBindingCache::Impl final {
        const std::thread::id owner{std::this_thread::get_id()};
        std::size_t maximumEntries, maximumBytes;
        ScriptModuleBindingCacheStats stats;
        std::vector<std::shared_ptr<const ScriptModuleBindingPlan>> plans;
    };

    namespace {
        void Integer(Sha256Builder &hash, std::uint64_t value) {
            std::array<std::byte, 8> bytes{};
            for (unsigned n = 0; n < 8; ++n)
                bytes[n] = std::byte((value >> (n * 8)) & 255U);
            static_cast<void>(hash.Update(bytes));
        }

        void Text(Sha256Builder &hash, std::string_view text) {
            Integer(hash, text.size());
            static_cast<void>(hash.Update(std::as_bytes(std::span{text.data(), text.size()})));
        }

        void Capabilities(Sha256Builder &hash, const std::vector<ExtensionCapabilityId> &input) {
            auto sorted = input;
            std::ranges::sort(sorted, {}, &ExtensionCapabilityId::value);
            Integer(hash, sorted.size());
            for (const auto &v : sorted)
                Text(hash, v.value);
        }

        void Policy(Sha256Builder &hash, const ScriptCapabilityContextDescriptor &policy) {
            Integer(hash, policy.policyRevision);
            Integer(hash, static_cast<unsigned>(policy.profile));
            Integer(hash, policy.packageVerified);
            Integer(hash, policy.packageTrusted);
            Integer(hash, policy.scriptTrusted);
            Integer(hash, policy.runtimeCompatible);
            Capabilities(hash, policy.packageCapabilities);
            Capabilities(hash, policy.scriptCapabilities);
            Capabilities(hash, policy.projectCapabilities);
            Capabilities(hash, policy.contextCapabilities);
        }

        Sha256Digest Key(const ScriptModuleRuntimeIdentity &runtime, const ScriptRuntimeBackendBindingInput &v,
                         const ScriptCapabilityContextDescriptor &policy) {
            Sha256Builder hash;
            Text(hash, "horo.module.binding.plan.v1");
            Integer(hash, static_cast<unsigned>(runtime.kind));
            Text(hash, runtime.version);
            Text(hash, runtime.targetProfile);
            static_cast<void>(hash.Update(std::as_bytes(std::span{runtime.implementation.bytes})));
            static_cast<void>(hash.Update(std::as_bytes(std::span{v.descriptors->Fingerprint().bytes})));
            Text(hash, v.apiId);
            Text(hash, v.service.contractId.value);
            Integer(hash, v.service.version.major);
            Integer(hash, v.service.version.minor);
            Integer(hash, v.service.version.patch);
            auto functions = v.functions;
            std::ranges::sort(functions);
            Integer(hash, functions.size());
            for (const auto &name : functions)
                Text(hash, name);
            Policy(hash, policy);
            return hash.Finalize();
        }

        void Projection(ScriptModuleBindingPlan &plan, const ScriptRuntimeBackendBinding &binding) {
            const auto &api = binding.Descriptor();
            std::string_view path = api.nameSpace;
            while (!path.empty()) {
                const auto end = path.find('.');
                plan.namespaceSegments.emplace_back(path.substr(0, end));
                if (end == std::string_view::npos)
                    break;
                path.remove_prefix(end + 1);
            }
            for (std::size_t index = 0; index < api.functions.size(); ++index)
                if (binding.AllowsFunction(api.functions[index].id))
                    plan.functionIndices.push_back(index);
            for (std::size_t index = 0; index < api.constants.size(); ++index)
                plan.constantIndices.push_back(index);
        }

        std::size_t Retained(const ScriptModuleBindingPlan &plan) {
            auto bytes = sizeof(plan) + plan.runtime.version.capacity() + plan.runtime.targetProfile.capacity() + plan.apiId.capacity();
            bytes += plan.namespaceSegments.capacity() * sizeof(std::string);
            for (const auto &segment : plan.namespaceSegments)
                bytes += segment.capacity();
            bytes += (plan.functionIndices.capacity() + plan.constantIndices.capacity()) * sizeof(std::size_t);
            return bytes;
        }

        bool BoundedCapabilities(const std::vector<ExtensionCapabilityId> &v) {
            return v.size() <= 128 && std::ranges::all_of(v, [](const auto &id) {
                return !id.value.empty() && id.value.size() <= 256;
            });
        }

        bool BoundedPolicy(const ScriptCapabilityContextDescriptor &v) {
            return BoundedCapabilities(v.packageCapabilities) && BoundedCapabilities(v.scriptCapabilities) &&
                   BoundedCapabilities(v.projectCapabilities) && BoundedCapabilities(v.contextCapabilities);
        }

        bool RequestValid(const ScriptModuleRuntimeIdentity &v, const ScriptCapabilityContextDescriptor &policy,
                          const ScriptRuntimeBackendBinding &backend) {
            return (v.kind == ScriptModuleRuntimeKind::Lua54 || v.kind == ScriptModuleRuntimeKind::JavaScriptNg) &&
                   !v.targetProfile.empty() && !v.version.empty() && v.version.size() <= 128 && v.targetProfile.size() <= 128 &&
                   BoundedPolicy(policy) && backend.IsUsable();
        }

        bool Retainable(const auto &state, std::size_t bytes) {
            return state.plans.size() < state.maximumEntries && bytes <= state.maximumBytes - state.stats.retainedBytes;
        }

        std::shared_ptr<const ScriptModuleBindingPlan> Cached(const auto &state, const Sha256Digest &key) {
            for (const auto &plan : state.plans)
                if (plan->key == key)
                    return plan;
            return {};
        }

        void Evict(auto &state) {
            for (auto i = state.plans.begin(); i != state.plans.end();) {
                if (i->use_count() == 1) {
                    state.stats.retainedBytes -= (*i)->retainedBytes;
                    i = state.plans.erase(i);
                } else
                    ++i;
            }
        }
    }  // namespace

    ScriptModuleBindingCache::ScriptModuleBindingCache(std::size_t entries, std::size_t bytes) {
        if (entries == 0 || entries > 1024 || bytes < sizeof(ScriptModuleBindingPlan) || bytes > 64U * 1024U * 1024U)
            throw std::invalid_argument{"Invalid bounded binding cache policy"};
        impl_ = std::make_unique<Impl>();
        impl_->maximumEntries = entries;
        impl_->maximumBytes = bytes;
        impl_->plans.reserve(entries);
    }

    ScriptModuleBindingCache::~ScriptModuleBindingCache() = default;

    ScriptModuleBindingCacheStats ScriptModuleBindingCache::Stats() const noexcept {
        auto result = impl_->stats;
        result.entries = impl_->plans.size();
        return result;
    }

    void ScriptModuleBindingCache::Invalidate(const Sha256Digest &fingerprint) {
        if (impl_->owner != std::this_thread::get_id())
            return;
        std::erase_if(impl_->plans, [&](const auto &plan) {
            if (plan->descriptorFingerprint != fingerprint)
                return false;
            impl_->stats.retainedBytes -= plan->retainedBytes;
            return true;
        });
    }

    void ScriptModuleBindingCache::InvalidateKey(const Sha256Digest &key) {
        if (impl_->owner != std::this_thread::get_id())
            return;
        std::erase_if(impl_->plans, [&](const auto &plan) {
            if (plan->key != key)
                return false;
            impl_->stats.retainedBytes -= plan->retainedBytes;
            return true;
        });
    }

    Result<std::shared_ptr<const ScriptModuleBindingPlan>> ScriptModuleBindingCache::Acquire(
        const ScriptModuleRuntimeIdentity &runtime, const ScriptRuntimeBackendBinding &backend,
        const ScriptCapabilityContextDescriptor &policy) {
        using Output = Result<std::shared_ptr<const ScriptModuleBindingPlan>>;
        if (impl_->owner != std::this_thread::get_id())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (!RequestValid(runtime, policy, backend))
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
        const auto key = Key(runtime, backend.Evidence(), policy);
        if (auto plan = Cached(*impl_, key)) {
            ++impl_->stats.hits;
            return Output::Success(std::move(plan));
        }
        ++impl_->stats.misses;
        auto plan = std::make_shared<ScriptModuleBindingPlan>();
        plan->key = key;
        plan->runtime = runtime;
        plan->descriptors = backend.Evidence().descriptors;
        plan->descriptorFingerprint = backend.Evidence().descriptors->Fingerprint();
        plan->apiId = backend.Evidence().apiId;
        Projection(*plan, backend);
        plan->retainedBytes = Retained(*plan);
        if (!Retainable(*impl_, plan->retainedBytes))
            Evict(*impl_);
        if (Retainable(*impl_, plan->retainedBytes)) {
            impl_->stats.retainedBytes += plan->retainedBytes;
            impl_->plans.push_back(plan);
        }
        return Output::Success(std::move(plan));  // Explicit uncached plan if finite cache admission is unavailable.
    }
}  // namespace Horo::Extensions
