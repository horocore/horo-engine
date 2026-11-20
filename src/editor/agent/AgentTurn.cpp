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

        /** @brief Combines the monotonic deadline and host cancellation into the provider's cooperative token. */
        class TurnStop final {
        public:
            TurnStop(const std::chrono::milliseconds timeout, const std::stop_token parent)
                : deadline_(Clock::now() + timeout), parent_(parent), cancel_(parent,
                                                                              [this] {
                                                                                  stop_.request_stop();
                                                                              }),
                  timer_([this](const std::stop_token shutdown) {
                      std::unique_lock lock{mutex_};
                      wake_.wait_until(lock, shutdown, deadline_, [] {
                          return false;
                      });
                      if (!shutdown.stop_requested())
                          stop_.request_stop();
                  }) {}

            [[nodiscard]] std::optional<AgentTurnStatus> Status() const noexcept {
                if (parent_.stop_requested())
                    return Cancelled;
                return Clock::now() >= deadline_ ? std::optional{TimedOut} : std::nullopt;
            }

            [[nodiscard]] std::stop_token Token() const noexcept {
                return stop_.get_token();
            }

            [[nodiscard]] Clock::time_point Deadline() const noexcept {
                return deadline_;
            }

        private:
            Clock::time_point deadline_;
            std::stop_token parent_;
            std::stop_source stop_;
            std::mutex mutex_;
            std::condition_variable_any wake_;
            std::stop_callback<std::function<void()>> cancel_;
            std::jthread timer_;
        };

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
        AgentTurnStatus PrepareTools(const IAgentTurnTools &tools, const ModelRequest &history, const ModelMessage &message) {
            for (const auto &intent : message.toolIntents) {
                if (std::ranges::find(history.tools, intent.name, &ModelTool::name) == history.tools.end())
                    return ToolDenied;
                for (const auto &earlier : history.messages)
                    if (std::ranges::find(earlier.toolIntents, intent.callId, &ModelToolIntent::callId) != earlier.toolIntents.end())
                        return InvalidModelResponse;
                if (!tools.ValidArguments(intent.argumentsJson))
                    return InvalidModelResponse;
            }
            return Completed;
        }

        /** @brief Maintains the terminal boundary even if a provider implementation throws. */
        ModelOutcome StreamResponse(IModelProvider &provider, const ModelRequest &request, const TurnStop &stop,
                                    Detail::AgentTurnStream &stream) {
            try {
                return provider.Stream(request, [&stop, &stream](const ModelEvent &event) {
                    return !stop.Token().stop_requested() && stream.Accept(event);
                }, stop.Token());
            } catch (const std::bad_alloc &) {
                throw;
            } catch (...) {
                return {ModelError{ModelErrorCode::Protocol, "The model provider failed to deliver its response", std::nullopt}};
            }
        }

        /** @brief Classifies one exact provider response before charging it to the turn or executing any tools. */
        AgentTurnStatus CompleteModelRound(const ModelOutcome &outcome, const Detail::AgentTurnStream &stream, const TurnStop &stop) {
            if (const auto stopped = stop.Status())
                return *stopped;
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
            std::uint64_t id;
            std::size_t submitted{};
        };

        /** @brief Appends the assistant intent round and related results only after successful dispatch. */
        AgentTurnStatus AppendToolRound(const IAgentTurnTools &tools, ModelRequest &request, ModelMessage message, TurnProgress &progress) {
            if (const auto admission = PrepareTools(tools, request, message); admission != Completed)
                return admission;
            std::vector<ModelMessage> results;
            results.reserve(message.toolIntents.size());
            for (const auto &intent : message.toolIntents) {
                std::string result;
                const auto state =
                    tools.Run(progress.id, ++progress.submitted, intent, progress.stop.Deadline(), progress.stop.Token(), result);
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
        }

    }  // namespace

    /** @copydoc AgentTurnRuntime::Run */
    AgentTurnResult AgentTurnRuntime::Run(IModelProvider &provider, const IAgentTurnTools *tools, const std::uint64_t turnId,
                                          ModelRequest request, const AgentTurnLimits &limits, const std::stop_token cancellation) {
        if (cancellation.stop_requested())
            return {Cancelled, {}};
        if (active_.test_and_set())
            return {Busy, {}};
        const TurnAdmission admissionGuard{active_};
        try {
            constexpr ModelFeatures known{static_cast<std::uint32_t>(ModelFeature::Streaming) |
                                          static_cast<std::uint32_t>(ModelFeature::Tools) |
                                          static_cast<std::uint32_t>(ModelFeature::Usage)};
            if (turnId == 0 || !ValidLimits(limits) || (!request.tools.empty() && !tools) || !AdmitModelRequest(request, known).Succeeded())
                return {InvalidRequest, {}};
            auto remaining = Detail::TurnStreamBudget{limits.maximumEvents, limits.maximumHistoryBytes, limits.maximumToolCalls};
            if (!ChargeRequest(remaining.bytes, request))
                return {BudgetExceeded, {}};

            TurnStop stop{limits.timeout, cancellation};
            std::uint32_t tokens = limits.maximumOutputTokens;
            const auto outputPerRound = request.maximumOutputTokens;
            TurnProgress progress{remaining, stop, turnId};
            for (std::size_t round = 0; round < limits.maximumRounds && tokens > 0; ++round) {
                if (const auto stopped = stop.Status())
                    return {*stopped, {}};
                request.maximumOutputTokens = std::min(outputPerRound, tokens);
                if (!AdmitModelRequest(request, known).Succeeded())
                    return {BudgetExceeded, {}};
                Detail::AgentTurnStream stream{progress.remaining};
                const auto outcome = StreamResponse(provider, request, stop, stream);
                if (const auto status = CompleteModelRound(outcome, stream, stop); status == ProviderFailed)
                    return {status, {}, outcome.error->code};
                else if (status != Completed)
                    return {status, {}};
                const auto used =
                    stream.Usage() ? stream.Usage()->outputTokens.value_or(request.maximumOutputTokens) : request.maximumOutputTokens;
                if (used > request.maximumOutputTokens)
                    return {BudgetExceeded, {}};
                tokens -= static_cast<std::uint32_t>(used);
                progress.remaining = stream.Remaining();
                auto message = std::move(stream).TakeMessage();
                if (message.toolIntents.empty()) {
                    request.messages.push_back(std::move(message));
                    return {Completed, std::move(request.messages)};
                }
                if (!tools)
                    return {ToolDenied, {}};
                if (round + 1 == limits.maximumRounds || tokens == 0)
                    return {BudgetExceeded, {}};
                if (const auto status = AppendToolRound(*tools, request, std::move(message), progress); status != Completed)
                    return {status, {}};
            }
            return {BudgetExceeded, {}};
        } catch (const std::bad_alloc &) {
            return {BudgetExceeded, {}};
        } catch (const std::system_error &) {
            return {ProviderFailed, {}};
        }
    }
}  // namespace Horo::Agent
