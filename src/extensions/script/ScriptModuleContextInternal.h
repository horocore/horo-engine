#pragma once
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/ScriptModuleRuntime.h"

#include <algorithm>
#include <atomic>
#include <thread>

namespace Horo::Extensions {
    struct ScriptModuleContext::Impl final {
        struct Bound final {
            std::shared_ptr<const ScriptRuntimeBackendBinding> backend;
            ScriptCapabilityBinding capability;
            std::shared_ptr<const ScriptModuleBindingPlan> plan;
        };

        struct Work final {
            JobHandle job;
            ScriptInvocationHandle invocation;
            std::shared_ptr<ScriptInvocationController> controller;
        };

        ScriptCapabilityContext context;
        std::shared_ptr<ScriptInvocationRegistry> registry;
        ScriptModuleBindingCache *cache{};
        const JobSystem &jobs;
        ScriptModuleRuntimeIdentity runtime;
        ScriptModuleRuntimeLimits limits;
        std::vector<ScriptModuleImport> imports;
        std::vector<ScriptCapabilityScopeLease> scopes;
        ScriptCapabilityContextDescriptor policy;
        CancellationToken cancellation;
        std::optional<ScriptModuleExecutionOwner> executionOwner;
        std::vector<Bound> bound;
        std::vector<Work> work;
        const std::thread::id owner{std::this_thread::get_id()};
        std::atomic_bool closed{};
        std::atomic_bool sealed{};

        Impl(ScriptCapabilityContext value, const JobSystem &system, ScriptModuleRuntimeIdentity identity, ScriptModuleRuntimeLimits policy,
             std::vector<ScriptModuleImport> declarations)
            : context(std::move(value)), jobs(system), runtime(std::move(identity)), limits(policy), imports(std::move(declarations)) {}
    };

    /** Read-only owner/capability liveness, separate from the new-admission seal used during compatible replacement. */
    bool ScriptModuleContextOwnersUsable(const auto &state) noexcept {
        if (state.closed.load(std::memory_order_acquire) || state.cancellation.IsCancellationRequested() || state.registry->IsShutdown())
            return false;
        if (state.executionOwner && !state.executionOwner->capability.IsUsable())
            return false;
        if (!std::ranges::all_of(state.scopes, [](const auto &scope) {
            return scope.IsUsable();
        }))
            return false;
        return std::ranges::all_of(state.bound, [](const auto &bound) {
            return bound.backend->IsUsable() && bound.capability.IsUsable();
        });
    }

    struct ScriptModuleContextAccess final {
        struct ResolvedCall {
            ScriptModuleContext::Impl::Bound *bound;
            const ScriptExportFunctionDescriptor *function;
        };

        static Result<ResolvedCall> Resolve(ScriptModuleContext::Impl &, std::string_view alias, std::string_view function);

        static const ScriptRuntimeBackendBindingInput &Evidence(const ScriptRuntimeBackendBinding &backend) {
            return backend.Evidence();
        }

        static ScriptCapabilityImport Import(const ScriptRuntimeBackendBinding &backend) {
            return backend.Import();
        }

        static Result<ScriptInvocationController> Begin(const ScriptRuntimeBackendBinding &backend, const ScriptCapabilityBinding &binding,
                                                        ScriptInvocationRequest request) {
            return backend.Begin(binding, std::move(request));
        }

        static Result<ScriptCallResult> Invoke(const ScriptRuntimeBackendBinding &backend, const ScriptInvocationRequest &request,
                                               const CancellationToken &token) {
            return backend.Invoke(request, token);
        }

        static Result<ScriptHandle> Issue(const ScriptRuntimeBackendBinding &backend, const ScriptCapabilityBinding &binding,
                                          std::string type) {
            return backend.IssueHandle(binding, std::move(type));
        }
    };

    Result<void> ValidateScriptModuleLimits(const ScriptModuleRuntimeIdentity &, const ScriptModuleRuntimeLimits &);
    Result<void> PrepareScriptModuleImports(ScriptCapabilityContextDescriptor &, const std::vector<ScriptModuleImport> &);

    void ReapScriptModuleWork(auto &state) {
        for (auto &work : state.work) {
            const auto job = work.job.Snapshot();
            if (job && job->state == JobState::Cancelled)
                static_cast<void>(work.controller->Handle().RequestCancellation());
            static_cast<void>(work.controller->ObserveCancellation());
        }
        std::erase_if(state.work, [](const auto &work) {
            const auto snapshot = work.job.Snapshot();
            return snapshot && snapshot->state != JobState::Queued && snapshot->state != JobState::Running;
        });
    }
}  // namespace Horo::Extensions
