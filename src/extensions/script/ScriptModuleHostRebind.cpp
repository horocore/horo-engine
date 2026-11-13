#include "ScriptModuleHostServicesInternal.h"

#include <algorithm>
#include <cassert>

namespace Horo::Extensions {
    struct ScriptModuleHostRebindCandidate final {
        ScriptModuleOperation *operation{};
        std::shared_ptr<ScriptModuleContext> context;
        std::unique_ptr<ScriptModulePreparedRebind> prepared;
        ScriptModuleUpdateReport report;
    };

    namespace {
        Result<void> UniqueOperations(std::span<const ScriptModuleOperationBindings> updates) {
            for (std::size_t i = 0; i < updates.size(); ++i) {
                if (!updates[i].operation)
                    return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
                for (std::size_t j = 0; j < i; ++j)
                    if (updates[i].operation == updates[j].operation)
                        return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid,
                                                               "An operation can occur only once in one atomic rebind batch."));
            }
            return Result<void>::Success();
        }

        bool SameAliases(std::span<const ScriptModuleImport> before, std::span<const ScriptModuleImport> after) {
            return before.size() == after.size() && std::ranges::all_of(before, [&](const auto &old) {
                return std::ranges::any_of(after, [&](const auto &next) {
                    return old.Alias() == next.Alias();
                });
            });
        }

        Result<void> ValidateCandidates(std::span<ScriptModuleHostRebindCandidate> candidates) {
            for (auto &candidate : candidates) {
                auto valid = candidate.prepared->Validate();
                if (valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }
    }  // namespace

    Result<std::vector<ScriptModuleUpdateReport>> ScriptModuleHostServices::Rebind(std::vector<ScriptModuleOperationBindings> updates) {
        using Output = Result<std::vector<ScriptModuleUpdateReport>>;
        if (impl_->owner != std::this_thread::get_id())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (updates.empty() || updates.size() > Impl::MaximumOperations)
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationCapacityExceeded));
        if (auto unique = UniqueOperations(updates); unique.HasError())
            return Output::Failure(unique.ErrorValue());
        try {
            return RebindOwned(std::move(updates));
        } catch (...) {
            return Output::Failure(
                MakeError(ExtensionErrors::ScriptInvocationAbandoned, "Rebind preparation failed; prior bindings remain active."));
        }
    }

    Result<std::vector<ScriptModuleUpdateReport>> ScriptModuleHostServices::RebindOwned(
        std::vector<ScriptModuleOperationBindings> updates) {
        using Output = Result<std::vector<ScriptModuleUpdateReport>>;
        std::vector<ScriptModuleHostRebindCandidate> candidates;
        candidates.reserve(updates.size());
        for (auto &update : updates) {
            auto &operation = *update.operation->impl_;
            if (operation.owner.get() != this || operation.result)
                return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationContextRevoked));
            if (!SameAliases(operation.context->Imports(), update.imports))
                return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid,
                                                 "An in-place rebind cannot change the signed operation import aliases."));
            if (auto capture = ValidateScriptModuleActiveCapture(*impl_->authority, *operation.program); capture.HasError())
                return Output::Failure(capture.ErrorValue());
            auto next = operation.context->PrepareReplacement(std::move(update.imports));
            if (next.HasError())
                return Output::Failure(next.ErrorValue());
            auto report = operation.context->InspectReplacement(*next.Value());
            if (report.HasError())
                return Output::Failure(report.ErrorValue());
            auto prepared = operation.runtime->PrepareRebind(next.Value());
            if (prepared.HasError())
                return Output::Failure(prepared.ErrorValue());
            candidates.emplace_back(update.operation, std::move(next).Value(), std::move(prepared).Value(), std::move(report).Value());
        }
        return CommitOwned(candidates);
    }

    Result<std::vector<ScriptModuleUpdateReport>> ScriptModuleHostServices::CommitOwned(
        std::vector<ScriptModuleHostRebindCandidate> &candidates) {
        using Output = Result<std::vector<ScriptModuleUpdateReport>>;
        std::vector<ScriptModuleUpdateReport> reports;
        reports.reserve(candidates.size());
        for (auto &candidate : candidates)
            reports.push_back(std::move(candidate.report));
        if (auto valid = ValidateCandidates(candidates); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        // No callback, VM evaluation, allocation, or owner-lane interleaving occurs between validation and publication.
        // Concurrent Close only latches cancellation; each prepared commit publishes a closed candidate in that race.
        for (auto &candidate : candidates) {
            const bool committed = candidate.prepared->Commit();
            assert(committed);
            if (!committed)
                std::terminate();
            candidate.operation->impl_->context = std::move(candidate.context);
        }
        return Output::Success(std::move(reports));
    }
}  // namespace Horo::Extensions
