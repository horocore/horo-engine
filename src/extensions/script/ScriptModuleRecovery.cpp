#include "ScriptModuleContextInternal.h"
#include "ScriptModuleRebindInternal.h"

#include <algorithm>
#include <tuple>

namespace Horo::Extensions {
    namespace {
        auto Values(const ScriptValueLimits &v) noexcept {
            return std::tie(v.maximumDepth, v.maximumElements, v.maximumArrayElements, v.maximumMapEntries, v.maximumStructFields,
                            v.maximumStringBytes, v.maximumBytes, v.maximumEncodedBytes, v.maximumWorkUnits, v.maximumTypeIdentityBytes,
                            v.maximumErrorDetails);
        }

        auto Resources(const ScriptModuleRuntimeLimits &v) noexcept {
            return std::tie(v.maximumSourceBytes, v.maximumMemoryBytes, v.maximumStackBytes, v.maximumPendingCalls, v.maximumBridgeBytes,
                            v.maximumBridgeValues, v.maximumEventsPerPoll, v.maximumVmJobsPerPoll, v.executionDeadline,
                            v.luaInstructionBudget);
        }

        std::vector<ScriptModuleApiFingerprint> Fingerprints(std::span<const ScriptModuleImport> imports) {
            std::vector<ScriptModuleApiFingerprint> result;
            result.reserve(imports.size());
            for (const auto &imported : imports) {
                ScriptModuleApiFingerprint value{imported.Alias(), {}};
                if (imported.Backend())
                    value.fingerprint = ScriptModuleContextAccess::Evidence(*imported.Backend()).descriptors->Fingerprint();
                result.push_back(std::move(value));
            }
            std::ranges::sort(result, {}, &ScriptModuleApiFingerprint::alias);
            return result;
        }
    }  // namespace

    bool SameScriptModuleLimits(const ScriptModuleRuntimeLimits &a, const ScriptModuleRuntimeLimits &b) noexcept {
        return Resources(a) == Resources(b) && Values(a.values) == Values(b.values);
    }

    Result<std::shared_ptr<ScriptModuleContext>> ScriptModuleContext::PrepareReplacement(std::vector<ScriptModuleImport> imports) const {
        try {
            using Output = Result<std::shared_ptr<ScriptModuleContext>>;
            if (impl_->owner != std::this_thread::get_id())
                return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
            if (impl_->sealed.load(std::memory_order_acquire) || impl_->closed.load(std::memory_order_acquire))
                return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationContextRevoked));
            auto policy = impl_->policy;
            policy.imports.clear();
            auto candidate = Create(impl_->registry, impl_->jobs, *impl_->cache, impl_->runtime, std::move(policy), std::move(imports),
                                    impl_->limits, impl_->cancellation, impl_->executionOwner);
            if (candidate.HasError())
                return candidate;
            if (auto valid = ValidateReplacement(*candidate.Value()); valid.HasError())
                return Output::Failure(valid.ErrorValue());
            return candidate;
        } catch (...) {
            return Result<std::shared_ptr<ScriptModuleContext>>::Failure(MakeError(ExtensionErrors::ScriptInvocationAbandoned));
        }
    }

    Result<ScriptModuleUpdateReport> ScriptModuleContext::InspectReplacement(const ScriptModuleContext &next) const {
        try {
            using Output = Result<ScriptModuleUpdateReport>;
            if (impl_->owner != std::this_thread::get_id() || next.impl_->owner != impl_->owner)
                return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
            ScriptModuleUpdateReport report;
            report.previousFingerprints = Fingerprints(impl_->imports);
            report.candidateFingerprints = Fingerprints(next.impl_->imports);
            if (auto valid = ValidateReplacement(next); valid.HasError()) {
                report.diagnostic = valid.ErrorValue();
                report.kind = valid.ErrorValue().code.Value() == ExtensionErrors::ScriptExportDescriptorIncompatible.code.Value()
                                  ? ScriptModuleUpdateKind::Breaking
                                  : ScriptModuleUpdateKind::Unavailable;
            } else
                report.kind = report.previousFingerprints == report.candidateFingerprints ? ScriptModuleUpdateKind::Unchanged
                                                                                          : ScriptModuleUpdateKind::Compatible;
            return Output::Success(std::move(report));
        } catch (...) {
            return Result<ScriptModuleUpdateReport>::Failure(MakeError(ExtensionErrors::ScriptInvocationAbandoned));
        }
    }

    bool ScriptModuleContext::SealNewCalls() noexcept {
        if (impl_->owner != std::this_thread::get_id())
            return false;
        impl_->sealed.store(true, std::memory_order_release);
        return true;
    }

    Result<bool> ScriptModuleContext::PollRetirement() {
        using Output = Result<bool>;
        if (impl_->owner != std::this_thread::get_id())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (!impl_->sealed.load(std::memory_order_acquire))
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid,
                                             "A live admitting context cannot be treated as a draining predecessor."));
        ReapScriptModuleWork(*impl_);
        if (impl_->closed.load(std::memory_order_acquire))
            return Output::Success(impl_->work.empty());
        if (auto events = impl_->context.Drain(impl_->limits.maximumEventsPerPoll); events.HasError()) {
            // Real scope/registry revocation is explicit cancellation, unlike an ordinary additive update's admission seal.
            Close();
            return Output::Failure(events.ErrorValue());
        }
        return Output::Success(impl_->work.empty());
    }
}  // namespace Horo::Extensions
