#include "Horo/Gameplay/GameplayAiScriptBridge.h"

#include "GameplayAiScriptValue.h"

#include <algorithm>
#include <new>

namespace Horo::Gameplay {
    namespace {
        /** @brief Returns the common recoverable typed capability failure without presentation strings. */
        Error Missing() {
            return MakeError(AI::AIErrors::CapabilityUnavailable);
        }
    }  // namespace

    /** @copydoc GameplayAiScriptBridge::GameplayAiScriptBridge */
    GameplayAiScriptBridge::GameplayAiScriptBridge(std::shared_ptr<const GameplayAiContext> context) : context_(std::move(context)) {}

    GameplayAiScriptBridge::~GameplayAiScriptBridge() {
        for (auto &record : resources_)
            if (record)
                RollbackResource(*record);
    }

    /** @copydoc GameplayAiScriptBridge::Find */
    Result<GameplayAiScriptBridge::Resource *> GameplayAiScriptBridge::Find(const Extensions::ScriptCapabilityBinding &binding,
                                                                            const Extensions::ScriptValue &value) {
        const auto *handle = value.AsHandle();
        if (!handle)
            return Result<Resource *>::Failure(MakeError(AI::AIErrors::HandleInvalid));
        if (auto valid = binding.ValidateHandle(*handle); valid.HasError())
            return Result<Resource *>::Failure(valid.ErrorValue());
        for (auto &resource : resources_)
            if (resource && resource->script == *handle)
                return Result<Resource *>::Success(&*resource);
        return Result<Resource *>::Failure(MakeError(AI::AIErrors::HandleInvalid));
    }

    /** @copydoc GameplayAiScriptBridge::ReapRevokedAtSafePoint */
    void GameplayAiScriptBridge::ReapRevokedAtSafePoint() {
        if (std::this_thread::get_id() != owner_)
            return;
        for (auto &record : resources_)
            if (record && !record->binding.IsUsable())
                RollbackResource(*record);
    }

    /** @copydoc GameplayAiScriptBridge::RollbackResource */
    void GameplayAiScriptBridge::RollbackResource(Resource &record) noexcept {
        try {
            if (context_ && record.task.IsValid()) {
                static_cast<void>(context_->CancelTask(record.agent, record.task));
                static_cast<void>(context_->ReleaseTask(record.agent, record.task));
            }
            static_cast<void>(record.binding.ReleaseHandle(record.script));
        } catch (const std::bad_alloc &) {
            return;
        }
        for (auto &slot : resources_)
            if (slot && &*slot == &record) {
                slot.reset();
                break;
            }
    }

    /** @copydoc GameplayAiScriptBridge::InvokeAtSafePoint */
    Result<Extensions::ScriptInvocationHandle> GameplayAiScriptBridge::InvokeAtSafePoint(
        const Extensions::ScriptCapabilityBinding &binding, const Extensions::ScriptInvocationProviderRegistration &provider,
        Extensions::ScriptInvocationRequest request) {
        if (std::this_thread::get_id() != owner_ || invoking_)
            return Result<Extensions::ScriptInvocationHandle>::Failure(Missing());
        invoking_ = true;

        struct InvocationExit final {
            GameplayAiScriptBridge &owner;

            ~InvocationExit() {
                if (owner.pendingPublication_)
                    owner.RollbackResource(*owner.pendingPublication_);
                owner.pendingPublication_ = nullptr;
                owner.invoking_ = false;
            }
        } exit{*this};

        auto admitted = binding.Begin(provider, std::move(request));
        if (admitted.HasError())
            return Result<Extensions::ScriptInvocationHandle>::Failure(admitted.ErrorValue());
        auto controller = std::move(admitted).Value();
        const auto handle = controller.Handle();
        auto cancellation = controller.ObserveCancellation();
        if (cancellation.HasError())
            return Result<Extensions::ScriptInvocationHandle>::Failure(cancellation.ErrorValue());
        if (controller.Cancellation().IsCancellationRequested())
            return Result<Extensions::ScriptInvocationHandle>::Success(handle);
        ReapRevokedAtSafePoint();
        try {
            const auto *input = controller.Request();
            Telemetry::ScopedOperationContext operation(input->diagnostics.CaptureOperationContext());
            auto result = Execute(binding, provider, *input);
            if (result.HasError())
                static_cast<void>(controller.Fail(result.ErrorValue()));
            else {
                auto completed = controller.Complete(std::move(result).Value());
                if (completed.HasValue() && completed.Value() == Extensions::ScriptInvocationTransitionResult::Applied)
                    pendingPublication_ = nullptr;
            }
        } catch (const std::bad_alloc &) {
            static_cast<void>(controller.Fail(MakeError(AI::AIErrors::TaskCapacityExceeded)));
        }
        return Result<Extensions::ScriptInvocationHandle>::Success(handle);
    }

    /** @copydoc GameplayAiScriptBridge::Execute */
    Result<Extensions::ScriptCallResult> GameplayAiScriptBridge::Execute(const Extensions::ScriptCapabilityBinding &binding,
                                                                         const Extensions::ScriptInvocationProviderRegistration &provider,
                                                                         const Extensions::ScriptInvocationRequest &request) {
        using Extensions::ScriptCallResult;
        using Extensions::ScriptValue;
        if (!context_ || request.apiId != GameplayAiScriptApiId)
            return Result<ScriptCallResult>::Failure(Missing());
        if (request.functionId == "resolve_agent")
            return ResolveAgent(binding, provider, request);
        if (request.arguments.empty())
            return Result<ScriptCallResult>::Failure(Missing());
        auto resource = Find(binding, request.arguments[0]);
        if (resource.HasError())
            return Result<ScriptCallResult>::Failure(resource.ErrorValue());
        auto &record = *resource.Value();
        if (request.functionId == "start_task")
            return StartTask(record, binding, provider, request);
        if (request.functionId == "read_blackboard")
            return ReadBlackboard(record, request);
        if (request.functionId == "write_blackboard")
            return WriteBlackboard(record, request);
        if (request.functionId == "task_state") {
            auto state = context_->InspectTask(record.agent, record.task);
            if (state.HasError())
                return Result<ScriptCallResult>::Failure(state.ErrorValue());
            return Result<ScriptCallResult>::Success(
                ScriptCallResult::Success({ScriptValue{static_cast<std::uint64_t>(state.Value().state)}}));
        }
        if (request.functionId == "cancel_task" || request.functionId == "complete_task") {
            auto transition = request.functionId == "cancel_task" ? context_->CancelTask(record.agent, record.task)
                                                                  : context_->CompleteTask(record.agent, record.task);
            if (transition.HasError())
                return Result<ScriptCallResult>::Failure(transition.ErrorValue());
            return Result<ScriptCallResult>::Success(ScriptCallResult::Success());
        }
        if (request.functionId == "release")
            return ReleaseResource(binding, record);
        return Result<ScriptCallResult>::Failure(Missing());
    }

    /** @copydoc GameplayAiScriptBridge::ReleaseResource */
    Result<Extensions::ScriptCallResult> GameplayAiScriptBridge::ReleaseResource(const Extensions::ScriptCapabilityBinding &binding,
                                                                                 Resource &record) {
        using Extensions::ScriptCallResult;
        if (record.task.IsValid()) {
            auto released = context_->ReleaseTask(record.agent, record.task);
            if (released.HasError())
                return Result<ScriptCallResult>::Failure(released.ErrorValue());
        }
        auto released = binding.ReleaseHandle(record.script);
        if (released.HasError())
            return Result<ScriptCallResult>::Failure(released.ErrorValue());
        for (auto &slot : resources_)
            if (slot && &*slot == &record) {
                slot.reset();
                break;
            }
        return Result<ScriptCallResult>::Success(ScriptCallResult::Success());
    }

    /** @copydoc GameplayAiScriptBridge::ResolveAgent */
    Result<Extensions::ScriptCallResult> GameplayAiScriptBridge::ResolveAgent(
        const Extensions::ScriptCapabilityBinding &binding, const Extensions::ScriptInvocationProviderRegistration &provider,
        const Extensions::ScriptInvocationRequest &request) {
        using Extensions::ScriptCallResult;
        using Extensions::ScriptValue;
        if (request.arguments.size() != 1 || !request.arguments[0].AsUnsignedInteger())
            return Result<ScriptCallResult>::Failure(Missing());
        auto identity = AI::AgentId::Create(*request.arguments[0].AsUnsignedInteger());
        if (identity.HasError())
            return Result<ScriptCallResult>::Failure(identity.ErrorValue());
        auto agent = context_->ResolveAgent(identity.Value());
        if (agent.HasError())
            return Result<ScriptCallResult>::Failure(agent.ErrorValue());
        auto vacant = std::find_if(resources_.begin(), resources_.end(), [](const auto &slot) {
            return !slot;
        });
        if (vacant == resources_.end())
            return Result<ScriptCallResult>::Failure(MakeError(AI::AIErrors::TaskCapacityExceeded));
        auto script = binding.IssueHandle(provider, "horo.ai.agent");
        if (script.HasError())
            return Result<ScriptCallResult>::Failure(script.ErrorValue());

        struct HandleRollback final {
            const Extensions::ScriptCapabilityBinding &binding;
            const Extensions::ScriptHandle &handle;
            bool committed{};

            ~HandleRollback() {
                if (!committed)
                    try {
                        static_cast<void>(binding.ReleaseHandle(handle));
                    } catch (const std::bad_alloc &) {
                    }
            }
        } handleRollback{binding, script.Value()};

        ScriptCallResult result = ScriptCallResult::Success({ScriptValue{script.Value()}});
        vacant->emplace(Resource{binding, std::move(script).Value(), agent.Value(), {}});
        handleRollback.committed = true;
        pendingPublication_ = &**vacant;
        return Result<ScriptCallResult>::Success(std::move(result));
    }

    /** @copydoc GameplayAiScriptBridge::StartTask */
    Result<Extensions::ScriptCallResult> GameplayAiScriptBridge::StartTask(Resource &record,
                                                                           const Extensions::ScriptCapabilityBinding &binding,
                                                                           const Extensions::ScriptInvocationProviderRegistration &provider,
                                                                           const Extensions::ScriptInvocationRequest &request) {
        using Extensions::ScriptCallResult;
        using Extensions::ScriptValue;
        if (record.task.IsValid() || request.arguments.size() != 2 || !request.arguments[1].AsUnsignedInteger())
            return Result<ScriptCallResult>::Failure(Missing());
        auto taskId = AI::TaskId::Create(*request.arguments[1].AsUnsignedInteger());
        if (taskId.HasError())
            return Result<ScriptCallResult>::Failure(taskId.ErrorValue());
        auto vacant = std::find_if(resources_.begin(), resources_.end(), [](const auto &slot) {
            return !slot;
        });
        if (vacant == resources_.end())
            return Result<ScriptCallResult>::Failure(MakeError(AI::AIErrors::TaskCapacityExceeded));
        auto script = binding.IssueHandle(provider, "horo.ai.task");
        if (script.HasError())
            return Result<ScriptCallResult>::Failure(script.ErrorValue());

        struct HandleRollback final {
            const Extensions::ScriptCapabilityBinding &binding;
            const Extensions::ScriptHandle &handle;
            bool committed{};

            ~HandleRollback() {
                if (!committed)
                    try {
                        static_cast<void>(binding.ReleaseHandle(handle));
                    } catch (const std::bad_alloc &) {
                    }
            }
        } handleRollback{binding, script.Value()};

        auto result = ScriptCallResult::Success({ScriptValue{script.Value()}});
        auto started = context_->StartTask(record.agent, taskId.Value(), request.diagnostics.CaptureOperationContext());
        if (started.HasError()) {
            return Result<ScriptCallResult>::Failure(started.ErrorValue());
        }
        vacant->emplace(Resource{binding, std::move(script).Value(), record.agent, started.Value()});
        handleRollback.committed = true;
        pendingPublication_ = &**vacant;
        return Result<ScriptCallResult>::Success(std::move(result));
    }

    /** @copydoc GameplayAiScriptBridge::ReadBlackboard */
    Result<Extensions::ScriptCallResult> GameplayAiScriptBridge::ReadBlackboard(Resource &record,
                                                                                const Extensions::ScriptInvocationRequest &request) {
        using Extensions::ScriptCallResult;
        using Extensions::ScriptValue;
        if (record.task.IsValid() || request.arguments.size() != 2 || !request.arguments[1].AsUnsignedInteger())
            return Result<ScriptCallResult>::Failure(Missing());
        auto key = AI::BlackboardKeyId::Create(*request.arguments[1].AsUnsignedInteger());
        if (key.HasError())
            return Result<ScriptCallResult>::Failure(key.ErrorValue());
        auto value = context_->ReadBlackboard(record.agent, key.Value());
        if (value.HasError())
            return Result<ScriptCallResult>::Failure(value.ErrorValue());
        return Result<ScriptCallResult>::Success(
            ScriptCallResult::Success({value.Value() ? Detail::ToScriptBlackboardValue(*value.Value()) : ScriptValue::Null()}));
    }

    /** @copydoc GameplayAiScriptBridge::WriteBlackboard */
    Result<Extensions::ScriptCallResult> GameplayAiScriptBridge::WriteBlackboard(Resource &record,
                                                                                 const Extensions::ScriptInvocationRequest &request) {
        using Extensions::ScriptCallResult;
        using Extensions::ScriptValue;
        if (record.task.IsValid() || request.arguments.size() != 2 || request.arguments[1].GetKind() != ScriptValue::Kind::Array ||
            request.arguments[1].AsArray().size() > AI::MaximumBlackboardWritesPerBatch)
            return Result<ScriptCallResult>::Failure(Missing());
        auto admittedBatch = context_->BeginBlackboardWrite(record.agent);
        if (admittedBatch.HasError())
            return Result<ScriptCallResult>::Failure(admittedBatch.ErrorValue());
        auto batch = std::move(admittedBatch).Value();
        for (const auto &write : request.arguments[1].AsArray()) {
            const ScriptValue *key{}, *input{};
            for (const auto &[name, field] : write.AsStruct()) {
                if (name == "key")
                    key = &field;
                else if (name == "value")
                    input = &field;
            }
            if (write.StructType() != "ai_write" || !key || !input || !key->AsUnsignedInteger())
                return Result<ScriptCallResult>::Failure(Missing());
            auto identity = AI::BlackboardKeyId::Create(*key->AsUnsignedInteger());
            if (identity.HasError())
                return Result<ScriptCallResult>::Failure(identity.ErrorValue());
            auto value = Detail::FromScriptBlackboardValue(*input);
            if (value.HasError())
                return Result<ScriptCallResult>::Failure(value.ErrorValue());
            auto staged = batch.Stage({identity.Value(), std::move(value).Value(), {}});
            if (staged.HasError())
                return Result<ScriptCallResult>::Failure(staged.ErrorValue());
        }
        auto committed = context_->CommitBlackboardWrite(record.agent, std::move(batch));
        if (committed.HasError())
            return Result<ScriptCallResult>::Failure(committed.ErrorValue());
        return Result<ScriptCallResult>::Success(ScriptCallResult::Success({ScriptValue{committed.Value().revision}}));
    }
}  // namespace Horo::Gameplay
