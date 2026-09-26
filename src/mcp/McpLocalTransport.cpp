#include "Horo/Mcp/McpLocalTransport.h"

#include "Horo/Mcp/McpErrors.h"
#include "McpJsonBounds.h"

#include <algorithm>
#include <utility>

namespace Horo::Mcp {
    namespace {
        constexpr std::size_t MaximumFramesPerFeed = 64;

        /** @brief Encodes only stable error identity, never application paths, exception text, or credentials. */
        [[nodiscard]] nlohmann::json ErrorReply(const nlohmann::json &id, const int code, const std::string_view message,
                                                const ErrorCodeDescriptor *descriptor = nullptr) {
            nlohmann::json payload = {{"code", code}, {"message", message}};
            if (descriptor != nullptr) {
                const Error error = MakeError(*descriptor);
                payload["data"] = {{"domain", error.domain.Value()}, {"code", error.code.Value()}};
            }
            return {{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(payload)}};
        }

        /** @brief Converts a bounded response to one line and rejects unexpected serialization expansion. */
        [[nodiscard]] std::string BoundedLine(const nlohmann::json &reply, const McpSessionLimits &limits) {
            std::string line = reply.dump();
            if (line.size() > limits.maximumFrameBytes)
                line = ErrorReply(nullptr, -32000, "MCP result exceeds the local frame limit.", &McpErrors::ResultCapacityExceeded).dump();
            line.push_back('\n');
            return line;
        }

        /** @brief Requires the common JSON-RPC envelope before controller dispatch. */
        [[nodiscard]] bool ValidEnvelope(const nlohmann::json &message) {
            const auto version = message.find("jsonrpc");
            const auto method = message.find("method");
            return message.is_object() && version != message.end() && version->is_string() &&
                   version->get_ref<const std::string &>() == "2.0" && method != message.end() && method->is_string();
        }

        /** @brief Handles only a well-formed cancellation notification for the admitted generation. */
        void CancelNotifiedRequest(const nlohmann::json &message, McpSessionManager &manager, const McpSessionHandle session) {
            const auto params = message.find("params");
            if (params == message.end() || !params->is_object())
                return;
            const auto cancelled = params->find("requestId");
            if (cancelled != params->end())
                static_cast<void>(manager.Cancel(session, *cancelled));
        }
    }  // namespace

    /** @copydoc McpLocalTransport::Start */
    Result<std::shared_ptr<McpLocalTransport>> McpLocalTransport::Start(std::shared_ptr<McpSessionManager> manager,
                                                                        McpSessionAdmission admission) {
        if (manager == nullptr)
            return Result<std::shared_ptr<McpLocalTransport>>::Failure(MakeError(McpErrors::ConfigurationInvalid));
        auto opened = manager->Open(std::move(admission));
        if (opened.HasError())
            return Result<std::shared_ptr<McpLocalTransport>>::Failure(opened.ErrorValue());
        return Result<std::shared_ptr<McpLocalTransport>>::Success(
            std::shared_ptr<McpLocalTransport>(new McpLocalTransport(std::move(manager), opened.Value())));
    }

    McpLocalTransport::~McpLocalTransport() {
        Disconnect();
    }

    /** @copydoc McpLocalTransport::Feed */
    Result<std::vector<std::string>> McpLocalTransport::Feed(const std::string_view bytes) {
        const McpSessionLimits &limits = manager_->Limits();
        if (bytes.size() > limits.maximumFrameBytes || std::ranges::count(bytes, '\n') > MaximumFramesPerFeed)
            return Result<std::vector<std::string>>::Failure(MakeError(McpErrors::InputCapacityExceeded));

        std::vector<CompleteFrame> complete;
        McpSessionHandle session;
        {
            std::lock_guard lock{mutex_};
            if (!session_.IsValid())
                return Result<std::vector<std::string>>::Failure(MakeError(McpErrors::SessionUnavailable));
            session = session_;
            for (const char byte : bytes) {
                if (discarding_) {
                    if (byte == '\n') {
                        complete.push_back({.oversized = true});
                        discarding_ = false;
                    }
                } else if (byte == '\n') {
                    if (!partial_.empty() && partial_.back() == '\r')
                        partial_.pop_back();
                    complete.push_back({.text = std::move(partial_)});
                    partial_.clear();
                } else if (partial_.size() == limits.maximumFrameBytes) {
                    partial_.clear();
                    discarding_ = true;
                } else {
                    partial_.push_back(byte);
                }
            }
        }

        std::vector<std::string> replies;
        replies.reserve(complete.size());
        for (const CompleteFrame &frame : complete) {
            if (const auto reply = ProcessFrame(frame, session); reply.has_value() && Session() == session)
                replies.push_back(*reply);
        }
        return Result<std::vector<std::string>>::Success(std::move(replies));
    }

    /** @copydoc McpLocalTransport::SwitchProject */
    Result<McpSessionHandle> McpLocalTransport::SwitchProject(std::optional<std::string> projectIdentity) {
        std::lock_guard lock{mutex_};
        auto switched = manager_->SwitchProject(session_, std::move(projectIdentity));
        if (switched.HasValue()) {
            session_ = switched.Value();
            partial_.clear();
            discarding_ = false;
        }
        return switched;
    }

    /** @copydoc McpLocalTransport::Disconnect */
    void McpLocalTransport::Disconnect() noexcept {
        McpSessionHandle closed;
        {
            std::lock_guard lock{mutex_};
            closed = session_;
            session_ = {};
            partial_.clear();
            discarding_ = false;
        }
        if (closed.IsValid())
            manager_->Close(closed);
    }

    /** @copydoc McpLocalTransport::Session */
    McpSessionHandle McpLocalTransport::Session() const noexcept {
        std::lock_guard lock{mutex_};
        return session_;
    }

    McpLocalTransport::McpLocalTransport(std::shared_ptr<McpSessionManager> manager, const McpSessionHandle session) noexcept
        : manager_(std::move(manager)), session_(session) {}

    /** @copydoc McpLocalTransport::ProcessFrame */
    std::optional<std::string> McpLocalTransport::ProcessFrame(const CompleteFrame &frame, const McpSessionHandle session) const {
        const McpSessionLimits &limits = manager_->Limits();
        if (frame.oversized || frame.text.size() > limits.maximumInputBytes || !FrameWithinBounds(frame.text, limits))
            return BoundedLine(ErrorReply(nullptr, -32600, "MCP input exceeds a declared limit.", &McpErrors::InputCapacityExceeded),
                               limits);

        const nlohmann::json message = nlohmann::json::parse(frame.text, nullptr, false);
        if (message.is_discarded())
            return BoundedLine(ErrorReply(nullptr, -32700, "Invalid JSON."), limits);
        if (!JsonWithinBounds(message, limits, limits.maximumInputBytes))
            return BoundedLine(ErrorReply(nullptr, -32600, "MCP input exceeds a declared limit.", &McpErrors::InputCapacityExceeded),
                               limits);
        if (!ValidEnvelope(message))
            return BoundedLine(ErrorReply(nullptr, -32600, "Invalid JSON-RPC request."), limits);

        const std::string &method = message["method"].get_ref<const std::string &>();
        const auto id = message.find("id");
        if (id == message.end()) {
            if (method == "notifications/cancelled")
                CancelNotifiedRequest(message, *manager_, session);
            return std::nullopt;
        }
        if (!ValidRequestId(*id, limits))
            return BoundedLine(ErrorReply(nullptr, -32600, "Invalid JSON-RPC request identity."), limits);

        McpRequest request{.id = *id, .method = method};
        if (const auto params = message.find("params"); params != message.end()) {
            if (!params->is_object() && !params->is_array())
                return BoundedLine(ErrorReply(*id, -32602, "Invalid JSON-RPC parameters."), limits);
            request.params = *params;
        }
        auto result = manager_->Dispatch(session, request);
        if (result.HasError()) {
            const Error &error = result.ErrorValue();
            nlohmann::json reply = ErrorReply(*id, -32000, "MCP request failed.");
            reply["error"]["data"] = {{"domain", error.domain.Value()}, {"code", error.code.Value()}};
            return BoundedLine(reply, limits);
        }
        return BoundedLine({{"jsonrpc", "2.0"}, {"id", *id}, {"result", std::move(result).Value()}}, limits);
    }
}  // namespace Horo::Mcp
