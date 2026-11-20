#include "AgentTurnMcp.h"

#include "Horo/Mcp/McpErrors.h"

#include <algorithm>
#include <thread>

namespace Horo::Agent {
    namespace {
        using namespace std::chrono_literals;
        using enum AgentTurnStatus;
        using Json = nlohmann::json;
        using Clock = std::chrono::steady_clock;

        struct ArgumentDepthExceeded final {};

        /** @brief Decodes one bounded object without retaining unbounded recursive input. */
        Json DecodeArguments(const std::string_view arguments) {
            if (arguments.size() > (64U << 10U))
                return nullptr;
            try {
                return Json::parse(arguments, [](const int depth, Json::parse_event_t, const Json &) {
                    if (depth > 32)
                        throw ArgumentDepthExceeded{};
                    return true;
                }, false);
            } catch (const ArgumentDepthExceeded &) {
                return nullptr;
            }
        }

        /** @brief Projects stable MCP failure codes onto the editor's terminal outcomes. */
        AgentTurnStatus ErrorStatus(const std::string_view code) {
            using namespace Mcp::McpErrors;
            if (code == AuthorizationDenied.code.Value() || code == ApprovalRequired.code.Value() || code == ToolUnavailable.code.Value() ||
                code == ToolCapabilityUnavailable.code.Value())
                return ToolDenied;
            if (code == RequestCancelled.code.Value())
                return Cancelled;
            if (code == RequestTimedOut.code.Value())
                return TimedOut;
            return ToolFailed;
        }

        /** @brief Confirms that each reply still names the same exact native operation. */
        bool ReadOperationId(const Json &value, std::uint64_t &operation) {
            if (!value.is_object() || !value.contains("operationId") || !value["operationId"].is_number_unsigned())
                return false;
            const auto observed = value["operationId"].get<std::uint64_t>();
            if (observed == 0 || (operation != 0 && observed != operation))
                return false;
            operation = observed;
            return true;
        }

        /** @brief Decodes terminal controller status, with no outcome until the owner settles the operation. */
        std::optional<AgentTurnStatus> ReadToolOutcome(const Json &value, std::string &result) {
            if (!value.contains("status") || !value["status"].is_string())
                return ToolFailed;
            const auto &status = value["status"].get_ref<const std::string &>();
            if (status == "succeeded" && value.contains("result")) {
                result = value["result"].dump();
                return Completed;
            }
            if (status == "cancelled")
                return Cancelled;
            if (status == "timed_out")
                return TimedOut;
            if (status == "queued" || status == "running")
                return std::nullopt;
            return ToolFailed;
        }
    }  // namespace

    /** @copydoc AgentTurnMcp::AgentTurnMcp */
    AgentTurnMcp::AgentTurnMcp(std::shared_ptr<const Mcp::McpInProcessAdapter> adapter)
        : adapter_(std::move(adapter)), session_(adapter_ ? adapter_->Session() : Mcp::McpSessionHandle{}) {}

    /** @copydoc AgentTurnMcp::ValidArguments */
    bool AgentTurnMcp::ValidArguments(const std::string_view arguments) const {
        return DecodeArguments(arguments).is_object();
    }

    /** @copydoc AgentTurnMcp::Run */
    AgentTurnStatus AgentTurnMcp::Run(const std::uint64_t turnId, const std::size_t callOrdinal, const ModelToolIntent &intent,
                                      const Clock::time_point deadline, const std::stop_token stop, std::string &result) const {
        if (!session_.IsValid() || adapter_->Session() != session_)
            return ToolDenied;
        if (stop.stop_requested() || Clock::now() >= deadline)
            return Clock::now() >= deadline ? TimedOut : Cancelled;
        auto arguments = DecodeArguments(intent.argumentsJson);
        if (!arguments.is_object())
            return InvalidModelResponse;
        // The host-issued identity never incorporates provider-controlled names or call IDs.
        const auto id = "editor-agent/" + std::to_string(turnId) + "/" + std::to_string(callOrdinal);
        auto state =
            adapter_->Call({.id = id, .method = "tools/call", .params = {{"name", intent.name}, {"arguments", std::move(arguments)}}});
        std::uint64_t operation{};
        for (;;) {
            if (adapter_->Session() != session_)
                return ToolDenied;
            if (stop.stop_requested() || Clock::now() >= deadline) {
                static_cast<void>(adapter_->Cancel(id));
                return Clock::now() >= deadline ? TimedOut : Cancelled;
            }
            if (state.HasError())
                return ErrorStatus(state.ErrorValue().code.Value());
            const auto &value = state.Value();
            if (!ReadOperationId(value, operation))
                return ToolFailed;
            if (const auto outcome = ReadToolOutcome(value, result))
                return *outcome;
            std::this_thread::sleep_until(std::min(Clock::now() + 10ms, deadline));
            if (stop.stop_requested() || Clock::now() >= deadline)
                continue;
            state = adapter_->Call({.id = id + "/poll", .method = "operations/get", .params = {{"operationId", operation}}});
        }
    }
}  // namespace Horo::Agent
