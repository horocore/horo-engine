#include "Horo/Debug/CommandExecutor.h"

#include "ConsoleText.h"
#include "Horo/Debug/ConsoleVariableSet.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <deque>
#include <mutex>
#include <type_traits>

namespace Horo::Debug {
    namespace {
        /** @brief Executes prepared variable state access on the fixed owner, through the same admission and lifecycle queue. */
        class VariableCommand final : public ICommandHandler {
        public:
            VariableCommand(std::shared_ptr<ConsoleVariableSet> variables, std::string name, std::size_t outputLimit)
                : variables_(std::move(variables)), name_(std::move(name)), outputLimit_(outputLimit) {}

            /** @copydoc ICommandHandler::Execute */
            Result<DebugCommandOutput> Execute(const DebugCommandContext &context, std::span<const ArgumentValue> arguments,
                                               const CancellationToken &cancellation, const JobExecutionContext *) override {
                if (cancellation.IsCancellationRequested())
                    return Result<DebugCommandOutput>::Failure(JobCancelled().ErrorValue());
                if (arguments.size() > 1)
                    return Result<DebugCommandOutput>::Failure(MakeCommandError(CommandError::Usage));
                if (!arguments.empty()) {
                    auto converted = std::visit([](const auto &value) -> Result<ConsoleValue> {
                        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, DebugEntityReference>)
                            return Result<ConsoleValue>::Failure(MakeCommandError(CommandError::Usage));
                        else
                            return Result<ConsoleValue>::Success(value);
                    }, arguments.front());
                    if (converted.HasError())
                        return Result<DebugCommandOutput>::Failure(converted.ErrorValue());
                    const auto written = variables_->Set(name_, std::move(converted).Value(), context);
                    if (written.HasError())
                        return Result<DebugCommandOutput>::Failure(written.ErrorValue());
                }
                auto state = variables_->Read(name_, context);
                if (state.HasError())
                    return Result<DebugCommandOutput>::Failure(state.ErrorValue());
                std::string text;
                text.reserve(outputLimit_);
                const auto append = [&](std::string_view value) {
                    text.append(Detail::ConsoleTextPrefix(value, outputLimit_ - text.size()));
                };
                const auto value = [&](const ConsoleValue &item) {
                    std::visit([&](const auto &typed) {
                        using T = std::decay_t<decltype(typed)>;
                        if constexpr (std::is_same_v<T, std::string>) {
                            append("\"");
                            append(typed);
                            append("\"");
                        } else if constexpr (std::is_same_v<T, bool>)
                            append(typed ? "true" : "false");
                        else {
                            std::array<char, 64> buffer{};
                            const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), typed);
                            if (error == std::errc{})
                                append({buffer.data(), static_cast<std::size_t>(end - buffer.data())});
                        }
                    }, item);
                };
                append(name_);
                append(" active=");
                value(state.Value().active);
                if (state.Value().pending) {
                    append(" pending=");
                    value(*state.Value().pending);
                    switch (state.Value().applyPolicy) {
                        case ConsoleVariableApplyPolicy::NextFrame:
                            append(" apply=next_frame");
                            break;
                        case ConsoleVariableApplyPolicy::NextSceneLoad:
                            append(" apply=next_scene_load");
                            break;
                        case ConsoleVariableApplyPolicy::RestartRequired:
                            append(" apply=restart_required");
                            break;
                        default:
                            break;
                    }
                }
                return Result<DebugCommandOutput>::Success({std::move(text)});
            }

        private:
            std::shared_ptr<ConsoleVariableSet> variables_;
            std::string name_;
            std::size_t outputLimit_;
        };

        /** @brief Retains only admitted typed input and an owned capability until its safe point. */
        struct PendingCommand final {
            CommandInvocation invocation;
            DebugCommandContext context;
            std::shared_ptr<ICommandHandler> handler;
            CancellationSource cancellation;
            OperationId operation{};
            CommandThreadPolicy policy{};
            bool redactOutput{};
        };

        /** @brief Live scheduler lease used solely to project authoritative job lifecycle. */
        struct WorkerCommand final {
            std::shared_ptr<JobHandle> handle;
            OperationId operation{};
        };

        /** @brief Connects an operation's cancellation to its durable scheduler lease, including pre-bind races. */
        class CommandCancellation final {
        public:
            explicit CommandCancellation(CancellationSource source) : source_(std::move(source)) {}

            /** @brief Cancels intent first, then an already admitted job without retaining the scheduler object. */
            void Request() {
                source_.RequestCancellation();
                std::shared_ptr<JobHandle> handle;
                {
                    std::lock_guard lock(mutex_);
                    handle = handle_;
                }
                if (handle)
                    (void)handle->RequestCancel();
            }

            /** @brief Installs the admitted lease and catches cancellation requested before its publication. */
            void Bind(std::shared_ptr<JobHandle> handle) {
                {
                    std::lock_guard lock(mutex_);
                    handle_ = handle;
                }
                if (source_.Token().IsCancellationRequested())
                    (void)handle->RequestCancel();
            }

        private:
            CancellationSource source_;
            // Protects publication of one durable job lease; no feature/store callback runs under this lock.
            std::mutex mutex_;
            std::shared_ptr<JobHandle> handle_;
        };

        /** @brief Prevents exception text or sensitive argument echoes from entering presentation stores. */
        Result<DebugCommandOutput> Invoke(const PendingCommand &pending, const std::size_t outputLimit,
                                          const JobExecutionContext *job = nullptr) {
            if (pending.cancellation.Token().IsCancellationRequested())
                return Result<DebugCommandOutput>::Failure(JobCancelled().ErrorValue());
            try {
                auto result = pending.handler->Execute(pending.context, pending.invocation.arguments, pending.cancellation.Token(), job);
                if (pending.redactOutput) {
                    if (result.HasError() && IsJobCancelled(result.ErrorValue()))
                        return Result<DebugCommandOutput>::Failure(JobCancelled().ErrorValue());
                    if (result.HasError())
                        return Result<DebugCommandOutput>::Failure(MakeCommandError(CommandError::HandlerFailed));
                    return Result<DebugCommandOutput>::Success({std::string(std::string_view("[REDACTED]").substr(0, outputLimit))});
                }
                if (result.HasError())
                    return result;
                auto output = std::move(result).Value();
                if (!IsValidUtf8ScalarSequence(output.text))
                    return Result<DebugCommandOutput>::Failure(MakeCommandError(CommandError::HandlerFailed));
                output.text.resize(Detail::ConsoleTextPrefix(output.text, outputLimit).size());
                return Result<DebugCommandOutput>::Success(std::move(output));
            } catch (...) {
                return Result<DebugCommandOutput>::Failure(MakeCommandError(CommandError::HandlerFailed));
            }
        }

        /** @brief Publishes bounded terminal presentation through the existing operation authority. */
        void Finish(OperationStore &operations, const PendingCommand &pending, const Result<DebugCommandOutput> &result) {
            OperationUpdate update;
            if (result.HasValue()) {
                update.state = OperationState::Succeeded;
                update.message = result.Value().text;
                update.progress = 1.0F;
            } else {
                update.state = IsJobCancelled(result.ErrorValue()) ? OperationState::Cancelled : OperationState::Failed;
                update.error = result.ErrorValue();
            }
            (void)operations.Update(pending.operation, std::move(update));
        }
    }  // namespace

    struct CommandExecutor::State final {
        std::shared_ptr<const CommandRegistry> registry;
        std::vector<CommandBinding> bindings;
        JobSystem *jobs{};
        std::shared_ptr<OperationStore> operations;
        std::shared_ptr<ICommandAudit> audit;
        std::shared_ptr<ConsoleVariableSet> variables;
        std::thread::id owner, render;
        std::size_t capacity{};
        // Protects admission/boundary checks, cutoff queue, active drain credits and worker leases on submitting/draining threads.
        // Stop cancels active drains through shutdown; their guards retain/release credits without waiting.
        // Never held across feature callbacks.
        std::mutex mutex;
        std::deque<PendingCommand> pending;
        std::vector<WorkerCommand> workers;
        std::size_t inFlight{};
        CancellationSource shutdown;
        bool stopped{};
        bool drainingOwner{}, drainingRender{};
    };

    /** @copydoc CommandExecutor::CommandExecutor */
    CommandExecutor::CommandExecutor(std::shared_ptr<State> state) : state_(std::move(state)) {}

    /** @copydoc CommandExecutor::Create */
    Result<std::unique_ptr<CommandExecutor>> CommandExecutor::Create(std::shared_ptr<const CommandRegistry> registry,
                                                                     const std::span<const CommandBinding> bindings, JobSystem &jobs,
                                                                     std::shared_ptr<OperationStore> operations,
                                                                     const std::thread::id owner, const std::thread::id render,
                                                                     const std::size_t capacity, std::shared_ptr<ICommandAudit> audit,
                                                                     std::shared_ptr<ConsoleVariableSet> variables) {
        if (!registry || !registry->Profile() || !operations || capacity == 0 || capacity > 4096 || owner == std::thread::id{} ||
            render == std::thread::id{})
            return Result<std::unique_ptr<CommandExecutor>>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        const auto descriptors = registry->Discover({}, {15, true, true});
        if (variables) {
            if (variables->Profile() != *registry->Profile() || variables->OwnerThread() != owner)
                return Result<std::unique_ptr<CommandExecutor>>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
            for (const auto &variable : variables->Discover({}, {15, true, true}))
                for (const auto &command : descriptors)
                    if (variable.id == command.id || variable.name == command.name ||
                        (variable.name.size() > command.name.size() && variable.name.starts_with(command.name) &&
                         variable.name[command.name.size()] == '.') ||
                        (command.name.size() > variable.name.size() && command.name.starts_with(variable.name) &&
                         command.name[variable.name.size()] == '.'))
                        return Result<std::unique_ptr<CommandExecutor>>::Failure(MakeCommandError(CommandError::Duplicate));
        }
        if (bindings.size() != descriptors.size())
            return Result<std::unique_ptr<CommandExecutor>>::Failure(MakeCommandError(CommandError::MissingCapability));
        if (!audit && std::ranges::any_of(descriptors, [](const auto &descriptor) {
            return (static_cast<unsigned>(descriptor.flags) & 1U) != 0;
        }))
            return Result<std::unique_ptr<CommandExecutor>>::Failure(MakeCommandError(CommandError::MissingCapability));
        for (std::size_t i = 0; i < bindings.size(); ++i) {
            const auto descriptor = registry->Resolve(bindings[i].id, {15, true, true});
            if (!bindings[i].handler || descriptor.HasError())
                return Result<std::unique_ptr<CommandExecutor>>::Failure(MakeCommandError(CommandError::MissingCapability));
            if (static_cast<unsigned>(bindings[i].resource) > 1 ||
                (bindings[i].resource != JobResource::Cpu && descriptor.Value().threadPolicy != CommandThreadPolicy::WorkerJob))
                return Result<std::unique_ptr<CommandExecutor>>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
            if (descriptor.Value().threadPolicy == CommandThreadPolicy::WorkerJob && bindings[i].resource == JobResource::Cpu &&
                jobs.WorkerCount() == 0)
                return Result<std::unique_ptr<CommandExecutor>>::Failure(MakeCommandError(CommandError::MissingCapability));
            for (std::size_t j = 0; j < i; ++j)
                if (bindings[i].id == bindings[j].id)
                    return Result<std::unique_ptr<CommandExecutor>>::Failure(MakeCommandError(CommandError::Duplicate));
        }
        auto state = std::make_shared<State>();
        state->registry = std::move(registry);
        state->bindings.assign(bindings.begin(), bindings.end());
        state->jobs = &jobs;
        state->operations = std::move(operations);
        state->audit = std::move(audit);
        state->variables = std::move(variables);
        state->owner = owner;
        state->render = render;
        state->capacity = capacity;
        state->workers.reserve(capacity);
        return Result<std::unique_ptr<CommandExecutor>>::Success(std::unique_ptr<CommandExecutor>(new CommandExecutor(std::move(state))));
    }

    /** @copydoc CommandExecutor::~CommandExecutor */
    CommandExecutor::~CommandExecutor() {
        Stop();
    }

    /** @copydoc CommandExecutor::UsesRegistry */
    bool CommandExecutor::UsesRegistry(const CommandRegistry &registry) const noexcept {
        return state_->registry.get() == &registry;
    }

    /** @copydoc CommandExecutor::IsOwnerThread */
    bool CommandExecutor::IsOwnerThread() const noexcept {
        return state_->owner == std::this_thread::get_id();
    }

    /** @copydoc CommandExecutor::ValidateOwnerBoundary */
    Result<void> CommandExecutor::ValidateOwnerBoundary() const {
        if (!IsOwnerThread())
            return Result<void>::Failure(MakeCommandError(CommandError::WrongThread));
        std::lock_guard lock(state_->mutex);
        if (state_->stopped)
            return Result<void>::Failure(MakeCommandError(CommandError::ShuttingDown));
        if (state_->drainingOwner || (state_->drainingRender && state_->render == state_->owner))
            return Result<void>::Failure(MakeCommandError(CommandError::ReentrantDispatch));
        return Result<void>::Success();
    }

    /** @copydoc CommandExecutor::Submit */
    Result<CommandReceipt> CommandExecutor::Submit(const std::string_view input, const DebugCommandContext &context, const bool startup,
                                                   std::optional<ConfigurationSnapshotRef> configuration) {
        {
            std::lock_guard lock(state_->mutex);
            if (state_->stopped)
                return Result<CommandReceipt>::Failure(MakeCommandError(CommandError::ShuttingDown));
        }
        auto prepared = state_->registry->Prepare(input, context, startup);
        CommandInvocation invocation;
        DebugCommandDescriptor descriptor;
        std::string variableName;
        std::shared_ptr<ICommandHandler> handler;
        JobResource resource{JobResource::Cpu};
        if (prepared.HasError()) {
            if (!state_->variables || startup || prepared.ErrorValue().code.Value() != "console.unknown_command")
                return Result<CommandReceipt>::Failure(prepared.ErrorValue());
            auto variable = state_->variables->Prepare(input, context, state_->registry->Limits());
            if (variable.HasError())
                return Result<CommandReceipt>::Failure(variable.ErrorValue());
            invocation = std::move(variable.Value().invocation);
            variableName = std::move(variable.Value().name);
            descriptor.name = variableName;
            descriptor.threadPolicy = CommandThreadPolicy::OwnerThreadNextFrame;
            handler = std::make_shared<VariableCommand>(state_->variables, variableName, state_->registry->Limits().outputBytes);
        } else {
            invocation = std::move(prepared).Value();
            const auto resolved = state_->registry->Resolve(invocation.id, context);
            if (resolved.HasError())
                return Result<CommandReceipt>::Failure(resolved.ErrorValue());
            descriptor = resolved.Value();
            const auto found = std::ranges::find(state_->bindings, invocation.id, &CommandBinding::id);
            handler = found->handler;
            resource = found->resource;
        }
        if ((static_cast<unsigned>(descriptor.flags) & 1U) != 0) {
            try {
                auto audited = state_->audit->Record(invocation.id, invocation.redactedText);
                if (audited.HasError())
                    return Result<CommandReceipt>::Failure(audited.ErrorValue());
            } catch (...) {
                return Result<CommandReceipt>::Failure(MakeCommandError(CommandError::HandlerFailed));
            }
        }
        PendingCommand pending{std::move(invocation),   context, std::move(handler), CancellationSource(state_->shutdown.Token()), 0,
                               descriptor.threadPolicy, false};
        pending.redactOutput = (static_cast<unsigned>(descriptor.flags) & 2U) != 0 ||
                               std::ranges::any_of(descriptor.arguments, &DebugArgumentDescriptor::sensitive);
        const auto redactedText = pending.invocation.redactedText;
        std::unique_lock lock(state_->mutex);
        if (state_->stopped)
            return Result<CommandReceipt>::Failure(MakeCommandError(CommandError::ShuttingDown));
        if (descriptor.threadPolicy == CommandThreadPolicy::ImmediateConsoleThread) {
            lock.unlock();
            auto result = Invoke(pending, state_->registry->Limits().outputBytes);
            if (result.HasError())
                return Result<CommandReceipt>::Failure(result.ErrorValue());
            return Result<CommandReceipt>::Success({{}, {}, std::move(result).Value(), redactedText});
        }
        if (state_->pending.size() + state_->workers.size() + state_->inFlight >= state_->capacity)
            return Result<CommandReceipt>::Failure(MakeCommandError(CommandError::LimitExceeded));
        const auto cancellation = std::make_shared<CommandCancellation>(pending.cancellation);
        auto operation = state_->operations->Begin({OperationKind::Other, std::string(descriptor.name), {}, {}, {}, true, [cancellation] {
            cancellation->Request();
        }});
        if (!operation)
            return Result<CommandReceipt>::Failure(MakeCommandError(CommandError::LimitExceeded));
        pending.operation = *operation;
        if (descriptor.threadPolicy != CommandThreadPolicy::WorkerJob) {
            state_->pending.push_back(std::move(pending));
            return Result<CommandReceipt>::Success({operation, {}, {}, redactedText});
        }
        JobDescriptor job;
        job.parentCancellation = pending.cancellation.Token();
        job.operationId = operation;
        job.configuration = std::move(configuration);
        job.resource = resource;
        const auto operations = state_->operations;
        const auto outputLimit = state_->registry->Limits().outputBytes;
        const auto ownerThread = state_->owner;
        const auto renderThread = state_->render;
        JobProducerScope noBlockingAdmission(JobProducerRole::TransportOwner);
        auto submitted = state_->jobs->SubmitContext(job, [pending, operations, outputLimit, ownerThread,
                                                           renderThread](const JobExecutionContext &execution) {
            if (std::this_thread::get_id() == ownerThread || std::this_thread::get_id() == renderThread) {
                auto rejected = Result<DebugCommandOutput>::Failure(MakeCommandError(CommandError::WrongThread));
                Finish(*operations, pending, rejected);
                return Result<void>::Failure(rejected.ErrorValue());
            }
            (void)operations->Update(pending.operation, {OperationState::Running, {}, {}, {}, {}});
            auto result = Invoke(pending, outputLimit, &execution);
            Finish(*operations, pending, result);
            if (result.HasValue())
                return Result<void>::Success();
            return Result<void>::Failure(result.ErrorValue());
        });
        if (submitted.HasError()) {
            lock.unlock();
            const auto terminal = IsJobCancelled(submitted.ErrorValue()) ? OperationState::Cancelled : OperationState::Failed;
            (void)state_->operations->Update(*operation, {terminal, {}, {}, {}, submitted.ErrorValue()});
            return Result<CommandReceipt>::Failure(submitted.ErrorValue());
        }
        const auto id = submitted.Value().Id();
        auto handle = std::make_shared<JobHandle>(std::move(submitted).Value());
        cancellation->Bind(handle);
        state_->workers.push_back({std::move(handle), *operation});
        return Result<CommandReceipt>::Success({operation, id, {}, redactedText});
    }

    /** @copydoc CommandExecutor::DrainOwner */
    Result<std::size_t> CommandExecutor::DrainOwner(std::size_t budget) {
        return Drain(CommandThreadPolicy::OwnerThreadNextFrame, budget);
    }

    /** @copydoc CommandExecutor::DrainRender */
    Result<std::size_t> CommandExecutor::DrainRender(std::size_t budget) {
        return Drain(CommandThreadPolicy::RenderSafePoint, budget);
    }

    /** @brief Checks actual thread identity before moving a bounded cutoff batch out of the admission mutex. */
    Result<std::size_t> CommandExecutor::Drain(const CommandThreadPolicy policy, std::size_t budget) {
        const auto owner = policy == CommandThreadPolicy::RenderSafePoint ? state_->render : state_->owner;
        if (std::this_thread::get_id() != owner)
            return Result<std::size_t>::Failure(MakeCommandError(CommandError::WrongThread));
        std::vector<PendingCommand> batch;
        std::vector<std::pair<OperationId, JobSnapshot>> workerSnapshots;
        budget = std::min(budget, state_->capacity);

        struct DrainGuard final {
            std::shared_ptr<State> state;
            bool render{}, armed{};
            std::size_t outstanding{};

            ~DrainGuard() {
                if (!armed)
                    return;
                std::lock_guard lock(state->mutex);
                state->inFlight -= outstanding;
                (render ? state->drainingRender : state->drainingOwner) = false;
            }
        } guard{state_, policy == CommandThreadPolicy::RenderSafePoint, false};

        {
            std::lock_guard lock(state_->mutex);
            if (state_->stopped)
                return Result<std::size_t>::Failure(MakeCommandError(CommandError::ShuttingDown));
            auto &draining = guard.render ? state_->drainingRender : state_->drainingOwner;
            if (draining)
                return Result<std::size_t>::Failure(MakeCommandError(CommandError::ReentrantDispatch));
            batch.reserve(std::min(budget, state_->pending.size()));
            workerSnapshots.reserve(state_->workers.size());
            draining = true;
            guard.armed = true;
            for (auto it = state_->pending.begin(); it != state_->pending.end() && batch.size() < budget;) {
                if (it->policy == policy) {
                    batch.push_back(std::move(*it));
                    ++state_->inFlight;
                    ++guard.outstanding;
                    it = state_->pending.erase(it);
                } else
                    ++it;
            }
            for (auto it = state_->workers.begin(); it != state_->workers.end();) {
                auto snapshot = it->handle->Snapshot();
                if (snapshot)
                    workerSnapshots.emplace_back(it->operation, *snapshot);
                if (snapshot && snapshot->terminalResult)
                    it = state_->workers.erase(it);
                else
                    ++it;
            }
        }
        for (const auto &[id, snapshot] : workerSnapshots) {
            if (snapshot.state == JobState::Cancelled)
                (void)state_->operations->Update(id, {OperationState::Cancelled, {}, {}, {}, {}});
            else if (!snapshot.terminalResult && snapshot.state == JobState::Running)
                (void)state_->operations
                    ->Update(id, {OperationState::Running, std::string(snapshot.progress.phase.View()), {}, snapshot.progress.value, {}});
        }
        for (const auto &pending : batch) {
            (void)state_->operations->Update(pending.operation, {OperationState::Running, {}, {}, {}, {}});
            auto result = Invoke(pending, state_->registry->Limits().outputBytes);
            Finish(*state_->operations, pending, result);
            {
                std::lock_guard lock(state_->mutex);
                --state_->inFlight;
                --guard.outstanding;
            }
        }
        return Result<std::size_t>::Success(batch.size());
    }

    /** @copydoc CommandExecutor::Stop */
    void CommandExecutor::Stop() {
        std::deque<PendingCommand> abandoned;
        std::vector<WorkerCommand> workers;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->stopped)
                return;
            state_->stopped = true;
            state_->shutdown.RequestCancellation();
            abandoned.swap(state_->pending);
            workers.swap(state_->workers);
        }
        for (const auto &pending : abandoned)
            (void)state_->operations->Update(pending.operation, {OperationState::Cancelled, {}, {}, {}, {}});
        for (const auto &worker : workers) {
            (void)worker.handle->RequestCancel();
            const auto snapshot = worker.handle->Snapshot();
            if (snapshot && snapshot->state == JobState::Cancelled)
                (void)state_->operations->Update(worker.operation, {OperationState::Cancelled, {}, {}, {}, {}});
        }
    }
}  // namespace Horo::Debug
