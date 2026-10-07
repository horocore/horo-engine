#include "McpServe.h"

#include "Horo/Cli/CliErrors.h"
#include "Horo/Mcp/McpController.h"
#include "Horo/Mcp/McpLocalTransport.h"

#include <array>
#include <optional>

namespace Horo::Application::Internal {
    namespace {
        constexpr std::array Owners{Mcp::McpOwnerContext::Runtime, Mcp::McpOwnerContext::Background, Mcp::McpOwnerContext::Build};

        /** @brief Disconnects before controller/session drain while registry and application leases remain alive. */
        struct McpLifetime final {
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
    }  // namespace

    /** @copydoc ServeMcp */
    Result<void> ServeMcp(std::shared_ptr<Mcp::McpToolRegistry> registry, const McpServeChannel &channel,
                          const std::span<const std::string> capabilities) {
        if (!registry || !channel.read || !channel.write || !channel.stopped)
            return Result<void>::Failure(MakeError(Cli::CliErrors::ExecutionContextInvalid));
        McpLifetime lifetime;
        auto controller = Mcp::McpController::Create(registry);
        if (controller.HasError())
            return Result<void>::Failure(controller.ErrorValue());
        lifetime.controller = std::move(controller).Value();
        for (const auto owner : Owners) {
            const auto bound = lifetime.controller->BindOwner(owner);
            if (bound.HasError())
                return bound;
        }
        auto sessions = Mcp::McpSessionManager::Create(lifetime.controller);
        if (sessions.HasError())
            return Result<void>::Failure(sessions.ErrorValue());
        lifetime.sessions = std::move(sessions).Value();
        auto transport = Mcp::McpLocalTransport::Start(lifetime.sessions, {.clientIdentity = "horo.cli.local",
                                                                           .capabilities = {capabilities.begin(), capabilities.end()},
                                                                           .registryRevision = registry->Read()->Generation()});
        if (transport.HasError())
            return Result<void>::Failure(transport.ErrorValue());
        lifetime.transport = std::move(transport).Value();
        std::optional<Error> failure;
        while (!channel.stopped()) {
            auto read = channel.read();
            if (read.HasError()) {
                failure = read.ErrorValue();
                break;
            }
            if (read.Value().disconnected)
                break;
            // Feed limits are MCP-owned. A native chunk has at most 64 bytes/newlines.
            auto replies = lifetime.transport->Feed(read.Value().bytes);
            if (replies.HasError()) {
                failure = replies.ErrorValue();
                break;
            }
            for (const auto &reply : replies.Value()) {
                const auto written = channel.write(reply);
                if (written.HasError()) {
                    failure = written.ErrorValue();
                    break;
                }
            }
            if (failure)
                break;
            for (const auto owner : Owners) {
                const auto pumped = lifetime.controller->Pump(owner);
                if (pumped.HasError()) {
                    failure = pumped.ErrorValue();
                    break;
                }
            }
            if (failure)
                break;
        }
        if (!failure && channel.stopped())
            failure = MakeError(Cli::CliErrors::ExecutionCancelled);
        const auto shutdown = lifetime.Shutdown();
        if (shutdown.HasError())
            return shutdown;
        return failure ? Result<void>::Failure(std::move(*failure)) : Result<void>::Success();
    }
}  // namespace Horo::Application::Internal
