#include "AgentTurnMcp.h"

#include "Horo/Mcp/McpErrors.h"
#include "Horo/Mcp/McpToolRegistry.h"

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

        /** @brief Notifies presentation once per admitted operation without affecting execution. */
        void ObserveOperation(const std::function<void(std::uint64_t)> &observer, const std::uint64_t operation) noexcept {
            if (!observer)
                return;
            try {
                observer(operation);
            } catch (...) {
                // Terminal behavior and cancellation are independent of presentation.
            }
        }

        /** @brief Decodes terminal controller status, with no outcome until the owner settles the operation. */
        std::optional<Result<Json>> ReadToolOutcome(Json value) {
            using namespace Mcp::McpErrors;
            if (!value.contains("status") || !value["status"].is_string())
                return Result<Json>::Failure(MakeError(ToolOutputInvalid));
            const auto &status = value["status"].get_ref<const std::string &>();
            if (status == "succeeded" && value.contains("result"))
                return Result<Json>::Success(std::move(value["result"]));
            if (status == "cancelled")
                return Result<Json>::Failure(MakeError(RequestCancelled));
            if (status == "timed_out")
                return Result<Json>::Failure(MakeError(RequestTimedOut));
            if (status == "queued" || status == "running")
                return std::nullopt;
            return Result<Json>::Failure(MakeError(ControllerFailed));
        }
    }  // namespace

    /** @copydoc InspectMcpPlanTool */
    AgentPlanToolInspection InspectMcpPlanTool(const Mcp::McpToolSnapshot &registry, const std::span<const std::string> capabilities,
                                               const ModelToolIntent &intent) {
        const auto arguments = DecodeArguments(intent.argumentsJson);
        if (!arguments.is_object())
            return {InvalidModelResponse};
        const auto inspected = registry.InspectDraft({intent.name}, arguments, capabilities);
        if (inspected.HasError()) {
            const auto &error = inspected.ErrorValue();
            const auto &invalid = Mcp::McpErrors::ToolInputInvalid;
            return {ErrorChainContains(error, invalid.domain, invalid.code) ? InvalidModelResponse : ErrorStatus(error.code.Value())};
        }
        switch (inspected.Value()) {
            case Mcp::McpToolEffect::Query:
                return {Completed, AgentPlannedEffect::Read};
            case Mcp::McpToolEffect::PresentationSideEffect:
                return {Completed, AgentPlannedEffect::Presentation};
            case Mcp::McpToolEffect::Mutation:
                return {Completed, AgentPlannedEffect::Mutation};
        }
        return {ToolDenied};
    }

    /** @copydoc AgentTurnMcp::AgentTurnMcp */
    AgentTurnMcp::AgentTurnMcp(std::shared_ptr<const Mcp::McpInProcessAdapter> adapter, AgentContextProjectAuthority project,
                               std::function<Result<void>()> pump, std::function<void(std::uint64_t)> observeOperation)
        : adapter_(std::move(adapter)), session_(adapter_ ? adapter_->Session() : Mcp::McpSessionHandle{}), project_(std::move(project)),
          pump_(std::move(pump)), observeOperation_(std::move(observeOperation)) {}

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
        auto outcome = CallTool(id, intent.name, std::move(arguments), deadline, [&] {
            return stop.stop_requested();
        });
        if (outcome.HasError())
            return ErrorStatus(outcome.ErrorValue().code.Value());
        result = outcome.Value().dump();
        return Completed;
    }

    /** @copydoc AgentTurnMcp::CallTool */
    Result<Json> AgentTurnMcp::CallTool(std::string id, const std::string_view name, Json arguments, const Clock::time_point deadline,
                                        const std::function<bool()> &stopped) const {
        using namespace Mcp::McpErrors;
        using Call = Result<Json>;
        if (!session_.IsValid() || adapter_->Session() != session_)
            return Call::Failure(MakeError(AuthorizationDenied));
        if (Clock::now() >= deadline)
            return Call::Failure(MakeError(RequestTimedOut));
        if (stopped())
            return Call::Failure(MakeError(RequestCancelled));
        auto state = adapter_->Call({.id = id, .method = "tools/call", .params = {{"name", name}, {"arguments", std::move(arguments)}}});
        std::uint64_t operation{};
        for (;;) {
            if (adapter_->Session() != session_)
                return Call::Failure(MakeError(AuthorizationDenied));
            if (stopped() || Clock::now() >= deadline) {
                static_cast<void>(adapter_->Cancel(id));
                return Call::Failure(MakeError(Clock::now() >= deadline ? RequestTimedOut : RequestCancelled));
            }
            if (state.HasError())
                return state;
            const bool accepted = operation == 0;
            if (!ReadOperationId(state.Value(), operation))
                return Call::Failure(MakeError(ToolOutputInvalid));
            if (accepted)
                ObserveOperation(observeOperation_, operation);
            if (auto outcome = ReadToolOutcome(std::move(state).Value()))
                return std::move(*outcome);
            if (pump_) {
                if (const auto pumped = pump_(); pumped.HasError()) {
                    static_cast<void>(adapter_->Cancel(id));
                    return Call::Failure(pumped.ErrorValue());
                }
            }
            std::this_thread::sleep_until(std::min(Clock::now() + 10ms, deadline));
            if (!stopped() && Clock::now() < deadline)
                state = adapter_->Call({.id = id + "/poll", .method = "operations/get", .params = {{"operationId", operation}}});
        }
    }

    /** @copydoc AgentTurnMcp::ReadContextQuery */
    Result<Application::CodeQueryPage> AgentTurnMcp::ReadContextQuery(const std::uint64_t requestId, const std::size_t itemOrdinal,
                                                                      const Application::CodeQueryRequest &query,
                                                                      const Application::CodeQueryContext &context) const {
        using namespace Application;
        using Reading = Result<CodeQueryPage>;
        using enum CodeQueryKind;
        if (project_.identity.empty() || project_.generation == 0 || context.projectIdentity != project_.identity ||
            context.projectGeneration != project_.generation)
            return Reading::Failure(MakeError(CodeQueryErrors::Stale));
        const bool diagnostic = query.kind == Diagnostics;
        if (requestId == 0 || (!diagnostic && query.kind != BuildStatus) || !query.pattern.empty() || query.limit == 0 ||
            query.limit > 32 || (query.offset && !query.expectedRevision))
            return Reading::Failure(MakeError(CodeQueryErrors::Invalid));
        if (const auto active = Platform::CheckProjectReadContext(context); active.HasError())
            return Reading::Failure(active.ErrorValue());
        Json arguments{{"path", query.path}, {"offset", query.offset}, {"limit", query.limit}};
        if (query.expectedRevision)
            arguments["revision"] = *query.expectedRevision;
        const auto id = "editor-context/" + std::to_string(requestId) + "/" + std::to_string(itemOrdinal);
        const auto result =
            CallTool(id, diagnostic ? ProjectDiagnosticsTool : "project.build_status", std::move(arguments), context.deadline, [&] {
            return Platform::CheckProjectReadContext(context).HasError();
        });
        if (const auto active = Platform::CheckProjectReadContext(context); active.HasError())
            return Reading::Failure(active.ErrorValue());
        if (result.HasError())
            return Reading::Failure(result.ErrorValue());
        try {
            const auto &page = result.Value();
            const auto &records = page.at("records");
            if (!records.is_array() || records.size() > query.limit)
                return Reading::Failure(MakeError(CodeQueryErrors::Invalid));
            CodeQueryPage owned{.revision = page.at("revision").get<std::string>(),
                                .projectGeneration = page.at("projectGeneration").get<std::uint64_t>(),
                                .text = page.at("text").get<std::string>(),
                                .droppedRecords = page.at("droppedRecords").get<std::uint64_t>()};
            if (owned.projectGeneration != project_.generation || (query.expectedRevision && owned.revision != *query.expectedRevision))
                return Reading::Failure(MakeError(CodeQueryErrors::Stale));
            if (page.at("hasMore").get<bool>())
                owned.nextOffset = page.at("nextOffset").get<std::size_t>();
            owned.records.reserve(records.size());
            for (const auto &row : records)
                owned.records.emplace_back(row.at("kind").get<CodeQueryRecordKind>(), row.at("path").get<std::string>(),
                                           row.at("label").get<std::string>(), row.at("line").get<std::uint32_t>(),
                                           row.at("column").get<std::uint32_t>(), row.at("byteOffset").get<std::uint64_t>(),
                                           row.at("byteCount").get<std::uint64_t>());
            return Reading::Success(std::move(owned));
        } catch (const Json::exception &) {
            return Reading::Failure(MakeError(CodeQueryErrors::Invalid));
        }
    }
}  // namespace Horo::Agent
