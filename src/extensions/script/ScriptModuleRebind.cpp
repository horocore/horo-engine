#include "ScriptModuleRebindInternal.h"

namespace Horo::Extensions {
    ScriptModulePreparedRebind::ScriptModulePreparedRebind(std::unique_ptr<Impl> value) : impl_(std::move(value)) {}

    ScriptModulePreparedRebind::~ScriptModulePreparedRebind() = default;

    Result<void> ScriptModulePreparedRebind::Validate() {
        return impl_->Validate();
    }

    bool ScriptModulePreparedRebind::Commit() noexcept {
        return impl_->Commit();
    }

    std::unique_ptr<ScriptModulePreparedRebind> ScriptModuleRebindAccess::Create(std::unique_ptr<Implementation> value) {
        return std::unique_ptr<ScriptModulePreparedRebind>{new ScriptModulePreparedRebind{std::move(value)}};
    }

    Result<std::vector<std::shared_ptr<const ScriptModuleBindingPlan>>> PrepareScriptModulePlans(const ScriptModuleContext &context) {
        using Output = Result<std::vector<std::shared_ptr<const ScriptModuleBindingPlan>>>;
        std::vector<std::shared_ptr<const ScriptModuleBindingPlan>> plans;
        plans.reserve(context.Imports().size());
        for (const auto &imported : context.Imports()) {
            if (!imported.Backend()) {
                plans.push_back({});
                continue;
            }
            auto plan = context.BindingPlan(imported.Alias());
            if (plan.HasError())
                return Output::Failure(plan.ErrorValue());
            plans.push_back(std::move(plan).Value());
        }
        return Output::Success(std::move(plans));
    }

}  // namespace Horo::Extensions
