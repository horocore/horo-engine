#include "McpServe.h"

#include "Horo/Cli/CliErrors.h"
#include "Horo/Mcp/McpController.h"
#include "Horo/Mcp/McpLocalTransport.h"

#include <array>

namespace Horo::Application::Internal {
    namespace {
        constexpr std::array Owners{Mcp::McpOwnerContext::Runtime, Mcp::McpOwnerContext::Background, Mcp::McpOwnerContext::Build};

        /** @brief Disconnects before controller/session drain while registry and application leases remain alive. */
        struct McpLifetime final {
            McpLifetime() = default;
            McpLifetime(const McpLifetime &) = delete;
            McpLifetime &operator=(const McpLifetime &) = delete;
            McpLifetime(McpLifetime &&) = delete;
            McpLifetime &operator=(McpLifetime &&) = delete;

            std::shared_ptr<Mcp::McpController> controller;
            std::shared_ptr<Mcp::McpSessionManager> sessions;
            std::shared_ptr<Mcp::McpLocalTransport> transport;
            bool closed{};

            Result<void> Shutdown() {
                if (closed)
                    return Result<void>::Success();
                closed = true;
                if (transport)
                    transport->Disconnect();
                auto sessionResult = sessions ? sessions->Shutdown() : Result<void>::Success();
                auto controllerResult = controller ? controller->Shutdown() : Result<void>::Success();
                return sessionResult.HasError() ? sessionResult : controllerResult;
            }

            ~McpLifetime() {
                static_cast<void>(Shutdown());
            }
        };

        Result<void> WriteReplies(const McpServeChannel &channel, const std::vector<std::string> &replies) {
            for (const auto &reply : replies) {
                if (const auto written = channel.write(reply); written.HasError())
                    return written;
            }
            return Result<void>::Success();
        }

        Result<void> PumpOwners(const Mcp::McpController &controller) {
            for (const auto owner : Owners) {
                if (const auto pumped = controller.Pump(owner); pumped.HasError())
                    return Result<void>::Failure(pumped.ErrorValue());
            }
            return Result<void>::Success();
        }

        /** @brief Runs only protocol I/O and owner pumping; the caller owns reverse shutdown on every result. */
        Result<void> RunChannel(const McpLifetime &lifetime, const McpServeChannel &channel) {
            while (!channel.stopped()) {
                auto read = channel.read();
                if (read.HasError())
                    return Result<void>::Failure(read.ErrorValue());
                if (read.Value().disconnected)
                    break;
                // Feed limits are MCP-owned. A native chunk has at most 64 bytes/newlines.
                auto replies = lifetime.transport->Feed(read.Value().bytes);
                if (replies.HasError())
                    return Result<void>::Failure(replies.ErrorValue());
                if (const auto written = WriteReplies(channel, replies.Value()); written.HasError())
                    return written;
                if (const auto pumped = PumpOwners(*lifetime.controller); pumped.HasError())
                    return pumped;
            }
            return channel.stopped() ? Result<void>::Failure(MakeError(Cli::CliErrors::ExecutionCancelled)) : Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ServeMcp */
    Result<void> ServeMcp(std::shared_ptr<Mcp::McpToolRegistry> registry, const McpServeChannel &channel,
                          const McpServeAdmission &admission) {
        if (!registry || !channel.read || !channel.write || !channel.stopped)
            return Result<void>::Failure(MakeError(Cli::CliErrors::ExecutionContextInvalid));
        McpLifetime lifetime;
        auto controller = Mcp::McpController::Create(registry, {}, admission.authorization);
        if (controller.HasError())
            return Result<void>::Failure(controller.ErrorValue());
        lifetime.controller = std::move(controller).Value();
        for (const auto owner : Owners) {
            const auto bound = lifetime.controller->BindOwner(owner);
            if (bound.HasError())
                return bound;
        }
        auto sessions = Mcp::McpSessionManager::Create(lifetime.controller, {}, admission.authorization);
        if (sessions.HasError())
            return Result<void>::Failure(sessions.ErrorValue());
        lifetime.sessions = std::move(sessions).Value();
        auto transport = Mcp::McpLocalTransport::Start(lifetime.sessions, admission.session);
        if (transport.HasError())
            return Result<void>::Failure(transport.ErrorValue());
        lifetime.transport = std::move(transport).Value();
        const auto result = RunChannel(lifetime, channel);
        if (const auto shutdown = lifetime.Shutdown(); shutdown.HasError())
            return shutdown;
        return result;
    }
}  // namespace Horo::Application::Internal
