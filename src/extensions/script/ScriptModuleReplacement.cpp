#include "ScriptModuleContextInternal.h"
#include "ScriptModuleRebindInternal.h"

#include <algorithm>

namespace Horo::Extensions {
    namespace {
        bool ExactlyMatches(const auto &scope, const auto &set) {
            if (!scope.IsUsable())
                return false;
            return std::ranges::count_if(set, [&](const auto &other) {
                return other.IsUsable() && scope.Matches(other);
            }) == 1;
        }

        bool ScopeSet(const auto &a, const auto &b) {
            if (a.size() != b.size())
                return false;
            return std::ranges::all_of(a, [&](const auto &scope) {
                return ExactlyMatches(scope, b);
            }) && std::ranges::all_of(b, [&](const auto &scope) {
                return ExactlyMatches(scope, a);
            });
        }

        bool HostOpen(const auto &state) {
            if (state.closed.load(std::memory_order_acquire) || state.cancellation.IsCancellationRequested() ||
                state.registry->IsShutdown())
                return false;
            return std::ranges::all_of(state.bound, [](const auto &bound) {
                const auto &input = ScriptModuleContextAccess::Evidence(*bound.backend);
                return !input.hostClosed || !input.hostClosed->load(std::memory_order_acquire);
            });
        }

        bool SourceIdentity(const auto &a, const auto &b) {
            if (a.has_value() != b.has_value())
                return false;
            if (!a)
                return true;
            if (!a->capability.IsUsable() || !b->capability.IsUsable())
                return false;
            const auto &x = a->capability.Activation();
            const auto &y = b->capability.Activation();
            // Activation() borrows the immutable identity object in the actual retained admission state.
            // Equal text/generation from a separately issued admission is not the same source owner.
            return &x == &y && a->capability.Capability() == b->capability.Capability();
        }

        bool OperationIdentity(const auto &a, const auto &b) {
            return a.runtime == b.runtime && a.registry == b.registry && ScopeSet(a.scopes, b.scopes) &&
                   SourceIdentity(a.executionOwner, b.executionOwner);
        }

        bool SameIdentity(const ResolvedExtensionServiceImport &a, const ResolvedExtensionServiceImport &b) {
            return a.ConsumerExtensionId() == b.ConsumerExtensionId() && a.ConsumerModuleId() == b.ConsumerModuleId() &&
                   a.ImportId() == b.ImportId() && a.ServiceId() == b.ServiceId() && a.ContractId() == b.ContractId();
        }

        bool RequiredRetained(const ResolvedExtensionServiceImport &a, const ScriptModuleImport &next) {
            return !a.IsRequired() || (next.Resolution().IsRequired() && next.Backend());
        }

        bool ReplacementOpen(const auto &before, const auto &after, bool nextUsable) {
            return !before.sealed.load(std::memory_order_acquire) && HostOpen(before) && nextUsable && OperationIdentity(before, after) &&
                   SameScriptModuleLimits(before.limits, after.limits);
        }

        Result<void> Compatible(const ScriptModuleImport &old, const ScriptModuleImport &next) {
            const auto &a = old.Resolution();
            const auto &b = next.Resolution();
            if (!SameIdentity(a, b))
                return Result<void>::Failure(
                    MakeError(ExtensionErrors::ScriptInvocationInvalid, "Replacement changes the actual module import identity."));
            if (!RequiredRetained(a, next))
                return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationUnavailable,
                                                       "A required import cannot lose or weaken its concrete binding."));
            if (!old.Backend() || !next.Backend())
                return Result<void>::Success();
            const auto &previous = ScriptModuleContextAccess::Evidence(*old.Backend());
            const auto &candidate = ScriptModuleContextAccess::Evidence(*next.Backend());
            if (previous.capabilities != candidate.capabilities || previous.services != candidate.services)
                return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid,
                                                       "Replacement belongs to another actual capability/backend registry."));
            auto checked = BuildScriptExportDescriptorReplacement(previous.descriptors, candidate.descriptors->Descriptors());
            if (checked.HasError())
                return Result<void>::Failure(checked.ErrorValue());
            for (const auto &function : previous.functions)
                if (!next.Backend()->AllowsFunction(function))
                    return Result<void>::Failure(
                        MakeError(ExtensionErrors::ScriptInvocationUnavailable, "Replacement removes an admitted function."));
            return Result<void>::Success();
        }
    }  // namespace

    Result<void> ScriptModuleContext::ValidateReplacement(const ScriptModuleContext &next) const {
        if (impl_->owner != std::this_thread::get_id() || next.impl_->owner != impl_->owner)
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (!ReplacementOpen(*impl_, *next.impl_, next.IsUsable()))
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationContextRevoked));
        for (const auto &old : impl_->imports) {
            const auto found = std::ranges::find(next.impl_->imports, old.Alias(), &ScriptModuleImport::Alias);
            if (found == next.impl_->imports.end())
                return Result<void>::Failure(
                    MakeError(ExtensionErrors::ScriptInvocationUnavailable, "Replacement removes a declared import alias."));
            auto compatible = Compatible(old, *found);
            if (compatible.HasError())
                return compatible;
        }
        return Result<void>::Success();
    }

    Result<void> ScriptModuleContext::CommitReplacement(const ScriptModuleContext &next) {
        auto valid = ValidateReplacement(next);
        if (valid.HasError())
            return valid;
        if (!CommitPreparedReplacement(next))
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        return Result<void>::Success();
    }

    bool ScriptModuleContext::CommitPreparedReplacement(const ScriptModuleContext &next) noexcept {
        if (impl_->owner != std::this_thread::get_id() || next.impl_->owner != impl_->owner)
            return false;
        for (const auto &bound : impl_->bound) {
            const bool retained = std::ranges::any_of(next.impl_->bound, [&](const auto &candidate) {
                return candidate.plan->key == bound.plan->key;
            });
            if (!retained)
                impl_->cache->InvalidateKey(bound.plan->key);
        }
        return SealNewCalls();
    }
}  // namespace Horo::Extensions
