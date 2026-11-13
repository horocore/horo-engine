#include "ScriptModuleHostServicesInternal.h"

#include <algorithm>

namespace Horo::Extensions {
    namespace {
        bool AdditiveReferenceVersion(ScriptExportVersion old, ScriptExportVersion next) noexcept {
            return old == ScriptExportVersion{1, 0, 0} && next == ScriptExportVersion{1, 1, 0};
        }
    }  // namespace

    Result<std::vector<ScriptModuleOperationBindings>> ScriptModuleHostServices::ReferenceUpdates(
        const ScriptModuleReferenceInstallation &candidate) {
        using Output = Result<std::vector<ScriptModuleOperationBindings>>;
        std::vector<ScriptModuleOperationBindings> updates;
        updates.reserve(impl_->activeOperations);
        for (auto *operation : impl_->operations) {
            if (!operation || operation->ResultSnapshot() || !operation->impl_->program->RequestsReference())
                continue;
            ScriptModuleOperationBindings update{.operation = operation};
            for (const auto &old : operation->impl_->context->Imports()) {
                if (old.Alias() != "horo.reference") {
                    update.imports.push_back(old);
                    continue;
                }
                auto imported = candidate.Bind(old.Alias());
                if (imported.HasError())
                    return Output::Failure(imported.ErrorValue());
                update.imports.push_back(std::move(imported).Value());
            }
            updates.push_back(std::move(update));
        }
        return Output::Success(std::move(updates));
    }

    Result<bool> ScriptModuleHostServices::PollProviderRetirement() {
        if (impl_->owner != std::this_thread::get_id())
            return Result<bool>::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (!impl_->predecessorReference)
            return Result<bool>::Success(true);
        bool done = true;
        for (auto *operation : impl_->operations) {
            if (!operation)
                continue;
            auto drained = operation->impl_->runtime->PollRebindRetirement();
            if (drained.HasError()) {
                if (drained.ErrorValue().code.Value() == ExtensionErrors::ScriptInvocationReentrant.code.Value())
                    return Result<bool>::Success(false);
                return Result<bool>::Failure(drained.ErrorValue());
            }
            done = drained.Value() && done;
        }
        if (done)
            impl_->predecessorReference.reset();
        return Result<bool>::Success(done);
    }

    Result<std::vector<ScriptModuleUpdateReport>> ScriptModuleHostServices::RefreshReference(ScriptExportVersion version) {
        using Output = Result<std::vector<ScriptModuleUpdateReport>>;
        if (impl_->owner != std::this_thread::get_id())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        try {
            return RefreshReferenceOwned(version);
        } catch (...) {
            return Output::Failure(
                MakeError(ExtensionErrors::ScriptInvocationAbandoned, "Provider preparation failed; last valid provider remains active."));
        }
    }

    Result<std::vector<ScriptModuleUpdateReport>> ScriptModuleHostServices::RefreshReferenceOwned(ScriptExportVersion version) {
        using Output = Result<std::vector<ScriptModuleUpdateReport>>;
        auto retired = PollProviderRetirement();
        if (retired.HasError())
            return Output::Failure(retired.ErrorValue());
        if (!retired.Value())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationCapacityExceeded,
                                             "An accepted predecessor invocation still owns the single draining provider slot."));
        if (!impl_->reference)
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationUnavailable));
        if (version == impl_->reference->Version())
            return Output::Success({});
        if (!AdditiveReferenceVersion(impl_->reference->Version(), version))
            return Output::Failure(MakeError(ExtensionErrors::ScriptExportDescriptorIncompatible,
                                             "Builtin replacement is not the supported additive 1.0.0 to 1.1.0 transition."));
        auto candidate = ScriptModuleReferenceInstallation::Create(impl_->authority, impl_->profile, version);
        if (candidate.HasError())
            return Output::Failure(candidate.ErrorValue());
        auto updates = ReferenceUpdates(*candidate.Value());
        if (updates.HasError())
            return Output::Failure(updates.ErrorValue());
        auto reports = updates.Value().empty() ? Output::Success({}) : Rebind(std::move(updates).Value());
        if (reports.HasError())
            return reports;
        impl_->predecessorReference = std::move(impl_->reference);
        impl_->reference = std::move(candidate).Value();
        return reports;
    }

    Result<ScriptModuleUpdateReport> ScriptModuleHostServices::RequestNativeUpdate(const ScriptModuleProgram &candidate) {
        using Output = Result<ScriptModuleUpdateReport>;
        if (auto lane = impl_->authority->RequireOpenOwner(); lane.HasError())
            return Output::Failure(lane.ErrorValue());
        if (auto current = impl_->authority->Source(candidate.Manifest().id, candidate.Module().id); current.HasError())
            return Output::Failure(current.ErrorValue());
        else {
            if (auto verified = candidate.VerifyCurrent(); verified.HasError())
                return Output::Failure(verified.ErrorValue());
            return Output::Success(
                {.kind = ScriptModuleUpdateKind::RestartRequired,
                 .diagnostic = MakeError(ExtensionErrors::ScriptInvocationUnavailable,
                                         "Live native package replacement has no proven unload/quiescence contract; restart "
                                         "is required. The existing provider and bindings remain active.")});
        }
    }
}  // namespace Horo::Extensions
