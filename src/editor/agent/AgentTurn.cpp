#include "AgentTurn.h"

#include "AgentTurnStream.h"

#include <condition_variable>
#include <iterator>
#include <mutex>
#include <thread>

namespace Horo::Agent {
    namespace {
        using namespace std::chrono_literals;
        using enum AgentTurnStatus;
        using Clock = std::chrono::steady_clock;

        /** @brief Releases the exact admitted worker after every terminal path. */
        struct TurnAdmission final {
            std::atomic_flag &active;

            explicit TurnAdmission(std::atomic_flag &flag) noexcept : active(flag) {}

            TurnAdmission(const TurnAdmission &) = delete;
            TurnAdmission &operator=(const TurnAdmission &) = delete;

            ~TurnAdmission() {
                active.clear();
            }
        };

        /** @brief Treats a failing passive observer as authority withdrawal. */
        bool HasProviderAuthority(const AgentTurnFreshness &providerAuthority) noexcept {
            try {
                return providerAuthority();
            } catch (...) {
                return false;
            }
        }

        /** @brief Combines the deadline, authority withdrawal, and host cancellation into the cooperative token. */
        class TurnStop final {
        public:
            TurnStop(const Clock::time_point deadline, const std::stop_token parent, const AgentTurnFreshness *const providerAuthority)
                : deadline_(deadline), parent_(parent), cancel_(parent,
                                                                [this] {
                                                                    stop_.request_stop();
                                                                }),
                  timer_([this, providerAuthority](const std::stop_token shutdown) {
                      Observe(shutdown, providerAuthority);
                  }) {}

            [[nodiscard]] std::optional<AgentTurnStatus> Status() const noexcept {
                if (parent_.stop_requested())
                    return Cancelled;
                if (authorityRevoked_.load(std::memory_order_acquire))
                    return ContextDenied;
                return Clock::now() >= deadline_ ? std::optional{TimedOut} : std::nullopt;
            }

            [[nodiscard]] std::stop_token Token() const noexcept {
                return stop_.get_token();
            }

            [[nodiscard]] Clock::time_point Deadline() const noexcept {
                return deadline_;
            }

        private:
            /** @brief Converts passive authority withdrawal into the provider's cooperative stop signal. */
            void Observe(const std::stop_token shutdown, const AgentTurnFreshness *const providerAuthority) {
                constexpr auto AuthorityCheckInterval = 10ms;
                std::unique_lock lock{mutex_};
                auto now = Clock::now();
                while (!shutdown.stop_requested() && now < deadline_) {
                    if (providerAuthority && !HasProviderAuthority(*providerAuthority)) {
                        authorityRevoked_.store(true, std::memory_order_release);
                        break;
                    }
                    const auto next = providerAuthority ? std::min(deadline_, now + AuthorityCheckInterval) : deadline_;
                    wake_.wait_until(lock, shutdown, next, [] { return false; });
                    now = Clock::now();
                }
                lock.unlock();
                if (!shutdown.stop_requested())
                    stop_.request_stop();
            }

            Clock::time_point deadline_;
            std::stop_token parent_;
            std::stop_source stop_;
            std::mutex mutex_;
            std::condition_variable_any wake_;
            std::atomic_bool authorityRevoked_{}; /**< Written by the joined deadline thread and observed by the worker. */
            std::stop_callback<std::function<void()>> cancel_;
            std::jthread timer_;
        };

        /** @brief Keeps an admitted context invalid once any of its authority checks fails. */
        class TurnContextGate final {
        public:
            TurnContextGate(const AgentTurnContext *context, const AgentTurnFreshness &isCurrent) noexcept
                : context_(context), isCurrent_(isCurrent) {}

            [[nodiscard]] bool Check() noexcept {
                if (!CheckFreshness() || !context_)
                    return valid_;
                try {
                    valid_ = context_->preview.IsCurrent(context_->policy, context_->project, context_->isCurrent);
                } catch (...) {
                    valid_ = false;
                }
                return valid_;
            }

            [[nodiscard]] bool CheckFreshness() noexcept {
                if (valid_ && isCurrent_) {
                    try {
                        valid_ = isCurrent_();
                    } catch (...) {
                        valid_ = false;
                    }
                }
                return valid_;
            }

        private:
            const AgentTurnContext *context_;
            const AgentTurnFreshness &isCurrent_;
            bool valid_{true};
        };

        /** @brief A stale admitted context carries no response or provider error payload. */
        AgentTurnResult StaleContextResult() {
            return {.status = ContextDenied, .contextError = Security::AgentPolicyReason::StaleAuthority};
        }

        /** @brief Projects failed rounds without publishing private provider details. */
        AgentTurnResult FailedRound(const AgentTurnStatus status, const std::optional<ModelError> &providerError = std::nullopt) {
            if (status == ContextDenied)
                return StaleContextResult();
            if (status == ProviderFailed && providerError)
                return {.status = status, .providerError = providerError->code, .retryAfter = providerError->retryAfter};
            return {status, {}};
        }

        /** @brief Validates host policy before starting a timer or calling either port. */
        bool ValidLimits(const AgentTurnLimits &limits) noexcept {
            return limits.maximumRounds > 0 && limits.maximumRounds <= 16 && limits.maximumToolCalls <= 32 && limits.maximumEvents > 0 &&
                   limits.maximumEvents <= 65'536 && limits.maximumHistoryBytes > 0 && limits.maximumHistoryBytes <= (1U << 20U) &&
                   limits.maximumOutputTokens > 0 && limits.maximumOutputTokens <= 131'072 && limits.timeout > 0ms &&
                   limits.timeout <= 5min;
        }

        /** @brief Accounts for every owned byte before it can be retained in a subsequent request. */
        bool Charge(std::size_t &remaining, const std::string_view text) noexcept {
            if (text.size() > remaining)
                return false;
            remaining -= text.size();
            return true;
        }

        /** @brief Counts the initial host-authored context and tool schema bytes against the same turn allowance. */
        bool ChargeRequest(std::size_t &remaining, const ModelRequest &request) noexcept {
            for (const auto &message : request.messages) {
                if (!Charge(remaining, message.text) || !Charge(remaining, message.toolCallId))
                    return false;
                for (const auto &intent : message.toolIntents)
                    if (!Charge(remaining, intent.callId) || !Charge(remaining, intent.name) || !Charge(remaining, intent.argumentsJson))
                        return false;
            }
            return std::ranges::all_of(request.tools, [&](const ModelTool &tool) {
                return Charge(remaining, tool.name) && Charge(remaining, tool.description) && Charge(remaining, tool.parametersJson);
            });
        }

        /** @brief Admits the entire sibling request set before submitting its first tool. */
        template <typename Validate>
        AgentTurnStatus PrepareTools(const ModelRequest &history, const ModelMessage &message, const Validate &validate) {
            for (const auto &intent : message.toolIntents) {
                if (std::ranges::find(history.tools, intent.name, &ModelTool::name) == history.tools.end())
                    return ToolDenied;
                for (const auto &earlier : history.messages)
                    if (std::ranges::find(earlier.toolIntents, intent.callId, &ModelToolIntent::callId) != earlier.toolIntents.end())
                        return InvalidModelResponse;
                if (const auto status = validate(intent); status != Completed)
                    return status;
            }
            return Completed;
        }

        /** @brief Validates an inert draft and rechecks freshness before it can leave the runtime. */
        AgentTurnStatus ValidatePlanRound(const ModelRequest &request, const ModelMessage &message, const AgentPlanToolValidator &plan,
                                          const TurnStop &stop, TurnContextGate &context, std::vector<AgentPlannedTool> &tools) {
            try {
                tools.reserve(message.toolIntents.size());
                const auto status = PrepareTools(request, message, [&](const ModelToolIntent &intent) {
                    if (const auto stopped = stop.Status())
                        return *stopped;
                    if (!context.Check())
                        return ContextDenied;
                    auto inspected = plan(intent);
                    if (inspected.status != Completed)
                        return inspected.status;
                    if (!inspected.tool || inspected.tool->effect > AgentPlannedEffect::Mutation)
                        return ToolDenied;
                    tools.push_back(std::move(*inspected.tool));
                    return Completed;
                });
                if (const auto stopped = stop.Status())
                    return *stopped;
                return context.Check() ? status : ContextDenied;
            } catch (const std::bad_alloc &) {
                throw;
            } catch (...) {
                return stop.Status().value_or(ToolFailed);
            }
        }

        /** @brief Maintains the terminal boundary even if a provider implementation throws. */
        ModelOutcome StreamResponse(IModelProvider &provider, const ModelRequest &request, const TurnStop &stop,
                                    Detail::AgentTurnStream &stream, const AgentTurnTextObserver &observeText, TurnContextGate &context) {
            try {
                return provider.Stream(request, [&](const ModelEvent &event) {
                    if (stop.Token().stop_requested() || !context.CheckFreshness() || !stream.Accept(event))
                        return false;
                    if (observeText && event.kind == ModelEventKind::TextDelta) {
                        if (!context.Check())
                            return false;
                        observeText(event.text);
                    }
                    return true;
                }, stop.Token());
            } catch (const std::bad_alloc &) {
                throw;
            } catch (...) {
                return {ModelError{ModelErrorCode::Protocol, "The model provider failed to deliver its response", std::nullopt}};
            }
        }

        /** @brief Classifies one exact provider response before charging it to the turn or executing any tools. */
        AgentTurnStatus CompleteModelRound(const ModelOutcome &outcome, const Detail::AgentTurnStream &stream, const TurnStop &stop,
                                           TurnContextGate &context) {
            if (const auto stopped = stop.Status())
                return *stopped;
            if (!context.Check())
                return ContextDenied;
            if (const auto rejected = stream.Rejection())
                return *rejected == Detail::TurnStreamFailure::Capacity ? BudgetExceeded : InvalidModelResponse;
            if (!outcome.Succeeded())
                return outcome.error->code == ModelErrorCode::Cancelled ? Cancelled : ProviderFailed;
            return stream.Failure() ? InvalidModelResponse : Completed;
        }

        /** @brief Tracks the finite budgets shared by every inference and tool call in one admitted turn. */
        struct TurnProgress final {
            Detail::TurnStreamBudget remaining;
            const TurnStop &stop;
            TurnContextGate &context;
            std::uint64_t id;
            const AgentToolActivityObserver &observeTool;
            std::size_t submitted{};
        };

        /** @brief Delivers typed activity without allowing presentation failures to change the admission or operation. */
        void ReportToolActivity(const AgentToolActivityObserver &observe, const AgentToolActivity activity) noexcept {
            if (!observe)
                return;
            try {
                observe(activity);
            } catch (...) {
                // Tool authority and completion are independent of presentation.
            }
        }

        /** @brief Reports the exact already-admitted tool dispatch and one settled status. */
        AgentTurnStatus DispatchTool(const IAgentTurnTools &tools, const ModelToolIntent &intent, TurnProgress &progress,
                                     std::string &result) {
            const auto ordinal = ++progress.submitted;
            ReportToolActivity(progress.observeTool, {ordinal, {}});
            if (const auto stopped = progress.stop.Status()) {
                ReportToolActivity(progress.observeTool, {ordinal, *stopped});
                return *stopped;
            }
            if (!progress.context.Check()) {
                ReportToolActivity(progress.observeTool, {ordinal, ContextDenied});
                return ContextDenied;
            }
            AgentTurnStatus state{ToolFailed};
            try {
                state = tools.Run(progress.id, ordinal, intent, progress.stop.Deadline(), progress.stop.Token(), result);
            } catch (const std::bad_alloc &) {
                ReportToolActivity(progress.observeTool, {ordinal, progress.stop.Status().value_or(ToolFailed)});
                throw;
            } catch (...) {
                // Report the same terminal failure as ordinary tool settlement.
            }
            state = progress.stop.Status().value_or(state);
            ReportToolActivity(progress.observeTool, {ordinal, state});
            return state;
        }

        /** @brief Appends the assistant intent round and related results only after successful dispatch. */
        AgentTurnStatus AppendToolRound(const IAgentTurnTools &tools, ModelRequest &request, ModelMessage message, TurnProgress &progress) {
            try {
                const auto admission = PrepareTools(request, message, [&](const ModelToolIntent &intent) {
                    return tools.ValidArguments(intent.argumentsJson) ? Completed : InvalidModelResponse;
                });
                if (const auto stopped = progress.stop.Status())
                    return *stopped;
                if (admission != Completed)
                    return admission;
                std::vector<ModelMessage> results;
                results.reserve(message.toolIntents.size());
                for (const auto &intent : message.toolIntents) {
                    if (const auto stopped = progress.stop.Status())
                        return *stopped;
                    std::string result;
                    const auto state = DispatchTool(tools, intent, progress, result);
                    if (const auto stopped = progress.stop.Status())
                        return *stopped;
                    if (state != Completed)
                        return state;
                    if (!Charge(progress.remaining.bytes, intent.callId) || !Charge(progress.remaining.bytes, result))
                        return BudgetExceeded;
                    results.push_back({.role = ModelRole::Tool, .text = std::move(result), .toolCallId = intent.callId});
                }
                request.messages.push_back(std::move(message));
                request.messages.insert(request.messages.end(), std::make_move_iterator(results.begin()),
                                        std::make_move_iterator(results.end()));
                return Completed;
            } catch (const std::bad_alloc &) {
                throw;
            } catch (...) {
                return progress.stop.Status().value_or(ToolFailed);
            }
        }

    }  // namespace

    /** @copydoc AgentTurnRuntime::AgentTurnRuntime(AgentTurnFreshness) */
    AgentTurnRuntime::AgentTurnRuntime(AgentTurnFreshness isCurrent) : isCurrent_(std::move(isCurrent)) {}

    /** @copydoc AgentTurnRuntime::Run */
    AgentTurnResult AgentTurnRuntime::Run(IModelProvider &provider, const IAgentTurnTools *tools, const std::uint64_t turnId,
                                          ModelRequest request, const AgentTurnLimits &limits, const std::stop_token cancellation,
                                          const AgentTurnContext *context, const AgentTurnTextObserver &observeText,
                                          const AgentToolActivityObserver &observeTool) {
        return RunTurn(provider, turnId, std::move(request), limits, cancellation, {tools, context, observeText, observeTool, nullptr});
    }

    /** @copydoc AgentTurnRuntime::RunBefore */
    AgentTurnResult AgentTurnRuntime::RunBefore(IModelProvider &provider, const IAgentTurnTools &tools, const std::uint64_t turnId,
                                                ModelRequest request, const AgentTurnLimits &limits, const Clock::time_point deadline,
                                                const std::stop_token cancellation, const AgentToolActivityObserver &observeTool) {
        return RunTurn(provider, turnId, std::move(request), limits, cancellation, {&tools, nullptr, {}, observeTool, nullptr}, deadline);
    }

    /** @copydoc AgentTurnRuntime::RunScopedText */
    AgentTurnResult AgentTurnRuntime::RunScopedText(IModelProvider &provider, const std::uint64_t turnId, ModelRequest request,
                                                    const AgentTurnLimits &limits, const std::stop_token cancellation,
                                                    const AgentTurnFreshness &providerAuthority) {
        if (!providerAuthority)
            return {InvalidRequest};
        return RunTurn(provider, turnId, std::move(request), limits, cancellation,
                       {nullptr, nullptr, {}, {}, nullptr, &providerAuthority});
    }

    /** @copydoc AgentTurnRuntime::Plan */
    AgentTurnPlan AgentTurnRuntime::Plan(IModelProvider &provider, const std::uint64_t turnId, ModelRequest request,
                                         const AgentTurnLimits &limits, const AgentPlanToolValidator &validateTool,
                                         const std::stop_token cancellation, const AgentTurnContext *context,
                                         const AgentTurnFreshness &providerAuthority) {
        if (!validateTool)
            return {InvalidRequest};
        PlanReview review{validateTool};
        const auto deadline = context ? context->project.deadline : Clock::time_point::max();
        auto result = RunTurn(provider, turnId, std::move(request), limits, cancellation,
                              {nullptr, context, {}, {}, &review, providerAuthority ? &providerAuthority : nullptr}, deadline);
        if (result.status != Completed)
            return {.status = result.status,
                    .providerError = result.providerError,
                    .contextError = result.contextError,
                    .retryAfter = result.retryAfter,
                    .timing = review.timing};
        return {.status = Completed,
                .draft = std::move(result.messages.back()),
                .usage = result.finalUsage,
                .tools = std::move(review.tools),
                .timing = review.timing};
    }

    /** @brief Runs the shared bounded stream and chooses either review publication or tool dispatch. */
    AgentTurnResult AgentTurnRuntime::RunTurn(IModelProvider &provider, const std::uint64_t turnId, ModelRequest request,
                                              const AgentTurnLimits &limits, const std::stop_token cancellation, const TurnPorts &ports,
                                              const Clock::time_point deadline) {
        const auto &[tools, context, observeText, observeTool, plan, providerAuthority] = ports;
        if (cancellation.stop_requested())
            return {Cancelled, {}};
        if (active_.test_and_set())
            return {Busy, {}};
        const TurnAdmission admissionGuard{active_};
        try {
            constexpr ModelFeatures known{static_cast<std::uint32_t>(ModelFeature::Streaming) |
                                          static_cast<std::uint32_t>(ModelFeature::Tools) |
                                          static_cast<std::uint32_t>(ModelFeature::Usage)};
            if (turnId == 0 || !ValidLimits(limits) || (!request.tools.empty() && !tools && !plan) ||
                !AdmitModelRequest(request, known).Succeeded())
                return {InvalidRequest, {}};
            if (observeText && tools)
                return {InvalidRequest, {}};
            if (!context && Clock::now() >= deadline)
                return {TimedOut, {}};
            TurnContextGate contextGate{context, isCurrent_};
            const auto contextOffset = request.messages.size() - 1;
            const auto contextCount = context ? context->preview.Items().size() : 0;
            if (context) {
                if (!contextGate.CheckFreshness())
                    return StaleContextResult();
                const auto allowed = context->preview.InsertBeforeDispatch(context->policy, context->project, context->isCurrent,
                                                                           context->provider, turnId, request);
                if (Clock::now() >= deadline)
                    return {TimedOut, {}};
                if (!allowed.Allowed())
                    return {.status = ContextDenied, .contextError = allowed.reason};
            }
            auto remaining = Detail::TurnStreamBudget{limits.maximumEvents, limits.maximumHistoryBytes, limits.maximumToolCalls};
            if (!ChargeRequest(remaining.bytes, request))
                return {BudgetExceeded, {}};

            if (providerAuthority && !HasProviderAuthority(*providerAuthority))
                return StaleContextResult();
            TurnStop stop{std::min(deadline, Clock::now() + limits.timeout), cancellation, providerAuthority};
            std::uint32_t tokens = limits.maximumOutputTokens;
            const auto outputPerRound = request.maximumOutputTokens;
            TurnProgress progress{remaining, stop, contextGate, turnId, observeTool};
            for (std::size_t round = 0; round < limits.maximumRounds && tokens > 0; ++round) {
                if (const auto stopped = stop.Status())
                    return {*stopped, {}};
                if (!contextGate.Check())
                    return StaleContextResult();
                request.maximumOutputTokens = std::min(outputPerRound, tokens);
                if (!AdmitModelRequest(request, known).Succeeded())
                    return {BudgetExceeded, {}};
                Detail::AgentTurnStream stream{progress.remaining};
                const auto inferenceStart = plan ? Clock::now() : Clock::time_point{};
                const auto outcome = StreamResponse(provider, request, stop, stream, observeText, contextGate);
                if (plan)
                    plan->timing.emplace().inference = Clock::now() - inferenceStart;
                if (const auto status = CompleteModelRound(outcome, stream, stop, contextGate); status != Completed)
                    return FailedRound(status, outcome.error);
                const auto used =
                    stream.Usage() ? stream.Usage()->outputTokens.value_or(request.maximumOutputTokens) : request.maximumOutputTokens;
                if (used > request.maximumOutputTokens)
                    return {BudgetExceeded, {}};
                tokens -= static_cast<std::uint32_t>(used);
                progress.remaining = stream.Remaining();
                auto message = std::move(stream).TakeMessage();
                if (plan) {
                    const auto inspectionStart = Clock::now();
                    const auto status = ValidatePlanRound(request, message, plan->inspect, stop, contextGate, plan->tools);
                    if (!message.toolIntents.empty())
                        plan->timing->toolInspection = Clock::now() - inspectionStart;
                    if (status != Completed)
                        return FailedRound(status);
                }
                if (plan || message.toolIntents.empty()) {
                    if (context) {
                        const auto firstContext = request.messages.begin() + contextOffset;
                        request.messages.erase(firstContext, firstContext + contextCount);
                    }
                    request.messages.push_back(std::move(message));
                    return {.status = Completed, .messages = std::move(request.messages), .finalUsage = stream.Usage()};
                }
                if (!tools)
                    return {ToolDenied, {}};
                if (round + 1 == limits.maximumRounds || tokens == 0)
                    return {BudgetExceeded, {}};
                if (const auto status = AppendToolRound(*tools, request, std::move(message), progress); status != Completed)
                    return FailedRound(status);
            }
            return {BudgetExceeded, {}};
        } catch (const std::bad_alloc &) {
            return {BudgetExceeded, {}};
        } catch (const std::system_error &) {
            return {ProviderFailed, {}};
        }
    }
}  // namespace Horo::Agent
