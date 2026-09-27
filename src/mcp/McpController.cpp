#include "Horo/Mcp/McpController.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Mcp/McpErrors.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Horo::Mcp {
    namespace {
        enum class OperationState : std::uint8_t {
            Queued,
            Running,
            Succeeded,
            Failed,
            Cancelled,
            TimedOut
        };

        [[nodiscard]] bool Terminal(const OperationState state) noexcept {
            return state >= OperationState::Succeeded;
        }

        [[nodiscard]] std::string_view Name(const OperationState state) noexcept {
            switch (state) {
                case OperationState::Queued:
                    return "queued";
                case OperationState::Running:
                    return "running";
                case OperationState::Succeeded:
                    return "succeeded";
                case OperationState::Failed:
                    return "failed";
                case OperationState::Cancelled:
                    return "cancelled";
                case OperationState::TimedOut:
                    return "timed_out";
            }
            return "failed";
        }

        [[nodiscard]] std::size_t Index(const McpOwnerContext owner) noexcept {
            return static_cast<std::size_t>(owner);
        }

        struct Operation final {
            std::uint64_t id{};
            nlohmann::json requestId;
            McpSessionHandle session;
            McpToolId tool;
            nlohmann::json arguments;
            McpRequestContext context;
            CancellationSource cancellation;
            McpOwnerContext owner{McpOwnerContext::Editor};
            std::shared_ptr<const McpToolSnapshot> snapshot;
            OperationState state{OperationState::Queued};
            std::optional<nlohmann::json> result;
            std::optional<Error> error;
            std::optional<double> progress;
            std::string phase;
        };

        [[nodiscard]] nlohmann::json View(const Operation &operation) {
            nlohmann::json value = {{"schemaVersion", 1},
                                    {"requestId", operation.requestId},
                                    {"operationId", operation.id},
                                    {"tool", operation.tool.value},
                                    {"status", Name(operation.state)},
                                    {"progress", operation.progress ? nlohmann::json(*operation.progress) : nlohmann::json(nullptr)},
                                    {"phase", operation.phase},
                                    {"result", operation.result.value_or(nullptr)},
                                    {"error", operation.error ? SafeErrorData(*operation.error) : nlohmann::json{nullptr}}};
            return value;
        }

        [[nodiscard]] bool ValidLimits(const McpControllerLimits &limits) noexcept {
            return limits.maximumPending > 0 && limits.maximumPending <= 4096 && limits.maximumActive > 0 && limits.maximumActive <= 4096 &&
                   limits.maximumRecent <= 4096 && limits.maximumPumpBatch > 0 && limits.maximumPumpBatch <= 256 &&
                   limits.shutdownDrainTimeout > std::chrono::milliseconds::zero() && limits.shutdownDrainTimeout <= std::chrono::hours{24};
        }

        [[nodiscard]] std::optional<std::uint64_t> OperationId(const nlohmann::json &params) {
            if (!params.is_object() || !params.contains("operationId") || !params["operationId"].is_number_integer() ||
                (!params["operationId"].is_number_unsigned() && params["operationId"].get<std::int64_t>() <= 0))
                return std::nullopt;
            const auto id = params["operationId"].get<std::uint64_t>();
            return id == 0 ? std::nullopt : std::optional{id};
        }
    }  // namespace

    struct McpController::State final : std::enable_shared_from_this<State> {
        explicit State(std::shared_ptr<McpToolRegistry> registryValue, McpControllerLimits limitsValue)
            : registry(std::move(registryValue)), limits(limitsValue) {}

        std::mutex mutex;
        std::condition_variable drained;
        std::shared_ptr<McpToolRegistry> registry;
        McpControllerLimits limits;
        std::array<std::optional<std::thread::id>, 4> owners;
        std::array<bool, 4> pumping{};
        std::array<std::deque<std::shared_ptr<Operation>>, 4> queues;
        std::unordered_map<std::uint64_t, std::shared_ptr<Operation>> operations;
        std::deque<std::uint64_t> recent;
        std::uint64_t nextId{1};
        std::size_t pending{};
        std::size_t active{};
        std::size_t callbacks{};
        bool stopping{};

        /** @brief Publishes exactly one terminal state while holding mutex. */
        void Finish(const std::shared_ptr<Operation> &operation, const OperationState terminal, std::optional<Error> error = {},
                    std::optional<nlohmann::json> result = {}) {
            if (Terminal(operation->state))
                return;
            if (operation->state == OperationState::Queued) {
                auto &queue = queues[Index(operation->owner)];
                std::erase(queue, operation);
                --pending;
            }
            --active;
            operation->state = terminal;
            operation->error = std::move(error);
            operation->result = std::move(result);
            recent.push_back(operation->id);
            while (recent.size() > limits.maximumRecent) {
                operations.erase(recent.front());
                recent.pop_front();
            }
            if (limits.maximumRecent == 0)
                operations.erase(operation->id);
            drained.notify_all();
        }

        /** @brief Makes deadline and cancellation priority explicit at every state boundary. */
        bool StopIfRequested(const std::shared_ptr<Operation> &operation) {
            if (Terminal(operation->state))
                return true;
            if (std::chrono::steady_clock::now() >= operation->context.deadline) {
                operation->cancellation.RequestCancellation();
                Finish(operation, OperationState::TimedOut, MakeError(McpErrors::RequestTimedOut));
                return true;
            }
            if (operation->context.cancellation.IsCancellationRequested() || operation->cancellation.Token().IsCancellationRequested()) {
                operation->cancellation.RequestCancellation();
                Finish(operation, OperationState::Cancelled, MakeError(McpErrors::RequestCancelled));
                return true;
            }
            return false;
        }

        /** @brief Applies bounded progress only while the operation is running. */
        void ReportProgress(const std::uint64_t id, const double fraction, std::string phase) {
            if (!std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0 || phase.size() > 256 || !IsValidUtf8ScalarSequence(phase))
                return;
            std::lock_guard lock{mutex};
            const auto found = operations.find(id);
            if (found != operations.end() && found->second->state == OperationState::Running) {
                found->second->progress = fraction;
                found->second->phase = std::move(phase);
            }
        }

        /** @brief Admits one request and retains its snapshot under the bounded queue lock. */
        [[nodiscard]] Result<nlohmann::json> QueueCall(const McpRequest &request, const McpRequestContext &context, McpToolId tool,
                                                       const McpOwnerContext owner, std::shared_ptr<const McpToolSnapshot> snapshot) {
            std::lock_guard lock{mutex};
            if (stopping)
                return Result<nlohmann::json>::Failure(MakeError(McpErrors::ShuttingDown));
            if (pending >= limits.maximumPending || active >= limits.maximumActive || nextId == std::numeric_limits<std::uint64_t>::max())
                return Result<nlohmann::json>::Failure(MakeError(McpErrors::OperationCapacityExceeded));
            for (const auto &[id, existing] : operations) {
                static_cast<void>(id);
                if (existing->session == context.session && existing->requestId == request.id && !Terminal(existing->state))
                    return Result<nlohmann::json>::Failure(MakeError(McpErrors::RequestInvalid));
            }
            auto operation = std::make_shared<Operation>();
            operation->id = nextId++;
            operation->requestId = request.id;
            operation->session = context.session;
            operation->tool = std::move(tool);
            operation->arguments = request.params["arguments"];
            operation->context = context;
            operation->cancellation = CancellationSource{context.cancellation};
            operation->context.cancellation = operation->cancellation.Token();
            operation->context.reportProgress = [weak = weak_from_this(), id = operation->id](const double fraction, std::string phase) {
                if (const auto state = weak.lock())
                    state->ReportProgress(id, fraction, std::move(phase));
            };
            operation->owner = owner;
            operation->snapshot = std::move(snapshot);
            operations.emplace(operation->id, operation);
            queues[Index(owner)].push_back(operation);
            ++pending;
            ++active;
            StopIfRequested(operation);
            return Result<nlohmann::json>::Success(View(*operation));
        }

        /** @brief Finalizes a deferred callback and releases one shutdown drain lease. */
        void Complete(const std::shared_ptr<Operation> &operation, Result<nlohmann::json> outcome) {
            std::lock_guard lock{mutex};
            if (operation && !StopIfRequested(operation)) {
                if (outcome.HasValue())
                    Finish(operation, OperationState::Succeeded, {}, std::move(outcome).Value());
                else
                    Finish(operation, OperationState::Failed, outcome.ErrorValue());
            }
            --callbacks;
            drained.notify_all();
        }
    };

    /** @copydoc McpController::Create */
    Result<std::shared_ptr<McpController>> McpController::Create(std::shared_ptr<McpToolRegistry> registry, McpControllerLimits limits) {
        if (registry == nullptr || !ValidLimits(limits))
            return Result<std::shared_ptr<McpController>>::Failure(MakeError(McpErrors::ConfigurationInvalid));
        return Result<std::shared_ptr<McpController>>::Success(
            std::shared_ptr<McpController>(new McpController(std::make_shared<State>(std::move(registry), limits))));
    }

    McpController::McpController(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

    McpController::~McpController() {
        static_cast<void>(Shutdown());
    }

    /** @copydoc McpController::Dispatch */
    Result<nlohmann::json> McpController::Dispatch(const McpRequest &request, const McpRequestContext &context) {
        if (request.method == "tools/list")
            return DispatchList(context);
        if (request.method == "operations/get" || request.method == "operations/cancel")
            return DispatchOperation(request, context);
        return DispatchCall(request, context);
    }

    /** @copydoc McpController::DispatchList */
    Result<nlohmann::json> McpController::DispatchList(const McpRequestContext &context) {
        std::lock_guard lock{state_->mutex};
        if (state_->stopping)
            return Result<nlohmann::json>::Failure(MakeError(McpErrors::ShuttingDown));
        const auto snapshot = state_->registry->Read();
        if (context.registryRevision != 0 && context.registryRevision != snapshot->Generation())
            return Result<nlohmann::json>::Failure(MakeError(McpErrors::RegistryRevisionStale));
        nlohmann::json tools = nlohmann::json::array();
        for (const auto &descriptor : snapshot->Discover(context.capabilities))
            tools.push_back({{"name", descriptor.id.value},
                             {"description", descriptor.description},
                             {"inputSchema", descriptor.inputSchema},
                             {"outputSchema", descriptor.outputSchema}});
        return Result<nlohmann::json>::Success({{"tools", std::move(tools)}, {"registryRevision", snapshot->Generation()}});
    }

    /** @copydoc McpController::DispatchOperation */
    Result<nlohmann::json> McpController::DispatchOperation(const McpRequest &request, const McpRequestContext &context) {
        const auto id = OperationId(request.params);
        if (!id)
            return Result<nlohmann::json>::Failure(MakeError(McpErrors::RequestInvalid));
        std::lock_guard lock{state_->mutex};
        const auto found = state_->operations.find(*id);
        if (found == state_->operations.end() || found->second->session != context.session)
            return Result<nlohmann::json>::Failure(MakeError(McpErrors::OperationUnavailable));
        const auto &operation = found->second;
        if (request.method == "operations/cancel" && !Terminal(operation->state)) {
            operation->cancellation.RequestCancellation();
            state_->Finish(operation, OperationState::Cancelled, MakeError(McpErrors::RequestCancelled));
        } else {
            state_->StopIfRequested(operation);
        }
        return Result<nlohmann::json>::Success(View(*operation));
    }

    /** @copydoc McpController::DispatchCall */
    Result<nlohmann::json> McpController::DispatchCall(const McpRequest &request, const McpRequestContext &context) {
        if (request.method != "tools/call" || !request.params.is_object() || !request.params.contains("name") ||
            !request.params["name"].is_string() || !request.params.contains("arguments") || !request.params["arguments"].is_object())
            return Result<nlohmann::json>::Failure(MakeError(McpErrors::RequestInvalid));

        const McpToolId tool{request.params["name"].get<std::string>()};
        if (tool.value.empty() || tool.value.size() > 128)
            return Result<nlohmann::json>::Failure(MakeError(McpErrors::RequestInvalid));
        const auto snapshot = state_->registry->Read();
        if (context.registryRevision != 0 && context.registryRevision != snapshot->Generation())
            return Result<nlohmann::json>::Failure(MakeError(McpErrors::RegistryRevisionStale));
        const auto owner = snapshot->Owner(tool, context.capabilities);
        if (owner.HasError())
            return Result<nlohmann::json>::Failure(owner.ErrorValue());
        return state_->QueueCall(request, context, tool, owner.Value(), snapshot);
    }

    /** @copydoc McpController::CancelAccepted */
    Result<void> McpController::CancelAccepted(const McpSessionHandle session, const nlohmann::json &requestId) {
        std::lock_guard lock{state_->mutex};
        for (const auto &[id, operation] : state_->operations) {
            static_cast<void>(id);
            if (operation->session == session && operation->requestId == requestId && !Terminal(operation->state)) {
                operation->cancellation.RequestCancellation();
                state_->Finish(operation, OperationState::Cancelled, MakeError(McpErrors::RequestCancelled));
                return Result<void>::Success();
            }
        }
        return Result<void>::Failure(MakeError(McpErrors::RequestInvalid));
    }

    /** @copydoc McpController::BindOwner */
    Result<void> McpController::BindOwner(const McpOwnerContext owner) {
        if (owner > McpOwnerContext::Build)
            return Result<void>::Failure(MakeError(McpErrors::OwnerUnavailable));
        std::lock_guard lock{state_->mutex};
        if (state_->stopping)
            return Result<void>::Failure(MakeError(McpErrors::ShuttingDown));
        auto &bound = state_->owners[Index(owner)];
        if (bound && *bound != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(McpErrors::OwnerUnavailable));
        bound = std::this_thread::get_id();
        return Result<void>::Success();
    }

    /** @copydoc McpController::Pump */
    Result<std::size_t> McpController::Pump(const McpOwnerContext owner) {
        if (owner > McpOwnerContext::Build)
            return Result<std::size_t>::Failure(MakeError(McpErrors::OwnerUnavailable));
        const auto state = state_;
        {
            std::lock_guard lock{state->mutex};
            if (!state->owners[Index(owner)] || *state->owners[Index(owner)] != std::this_thread::get_id() || state->pumping[Index(owner)])
                return Result<std::size_t>::Failure(MakeError(McpErrors::OwnerUnavailable));
            state->pumping[Index(owner)] = true;
        }

        struct PumpGuard final {
            std::shared_ptr<State> state;
            McpOwnerContext owner;

            ~PumpGuard() {
                std::lock_guard lock{state->mutex};
                state->pumping[Index(owner)] = false;
            }
        } guard{state, owner};

        std::size_t executed{};
        for (std::size_t count = 0; count < state->limits.maximumPumpBatch; ++count) {
            const auto step = PumpOne(owner);
            if (!step)
                break;
            executed += *step ? 1 : 0;
        }
        return Result<std::size_t>::Success(executed);
    }

    /** @copydoc McpController::PumpOne */
    std::optional<bool> McpController::PumpOne(const McpOwnerContext owner) {
        const auto state = state_;
        std::shared_ptr<Operation> operation;
        {
            std::lock_guard lock{state->mutex};
            auto &queue = state->queues[Index(owner)];
            if (queue.empty() || state->stopping)
                return std::nullopt;
            operation = queue.front();
            if (state->StopIfRequested(operation))
                return false;
            queue.pop_front();
            --state->pending;
            operation->state = OperationState::Running;
            ++state->callbacks;
        }
        const auto completed = std::make_shared<std::atomic_bool>(false);
        auto complete = [weakState = std::weak_ptr<State>{state}, weakOperation = std::weak_ptr<Operation>{operation},
                         completed](Result<nlohmann::json> outcome) {
            if (completed->exchange(true))
                return;
            const auto state = weakState.lock();
            if (!state)
                return;
            state->Complete(weakOperation.lock(), std::move(outcome));
        };
        try {
            operation->snapshot->InvokeAsync(operation->tool, operation->arguments, operation->context, complete);
        } catch (...) {  // NOSONAR: completion must drain even when an adapter throws before retaining its callback.
            complete(Result<nlohmann::json>::Failure(MakeError(McpErrors::ControllerFailed)));
        }
        operation->snapshot.reset();
        return true;
    }

    /** @copydoc McpController::Shutdown */
    Result<void> McpController::Shutdown() {
        const auto state = state_;
        std::unique_lock lock{state->mutex};
        state->stopping = true;
        std::vector<std::shared_ptr<Operation>> active;
        active.reserve(state->operations.size());
        for (const auto &[id, operation] : state->operations) {
            static_cast<void>(id);
            active.push_back(operation);
        }
        for (const auto &operation : active) {
            if (!Terminal(operation->state)) {
                operation->cancellation.RequestCancellation();
                state->Finish(operation, OperationState::Cancelled, MakeError(McpErrors::RequestCancelled));
            }
        }
        if (!state->drained.wait_for(lock, state->limits.shutdownDrainTimeout, [&state] {
            return state->callbacks == 0;
        }))
            return Result<void>::Failure(MakeError(McpErrors::DrainTimedOut));
        return Result<void>::Success();
    }
}  // namespace Horo::Mcp
