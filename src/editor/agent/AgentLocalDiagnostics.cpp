#include "AgentTurnMcp.h"
#include "Horo/Mcp/McpAuthorization.h"
#include "Horo/Mcp/McpController.h"
#include "Horo/Mcp/ProjectCodeQueryTools.h"

#include <algorithm>
#include <format>

namespace Horo::Agent {
    namespace {
        using namespace Application;
        using Page = Result<CodeQueryPage>;
        using Session = Result<AgentTurnMcp>;
        constexpr auto Owner = Mcp::McpOwnerContext::Background;
        constexpr std::size_t MaximumRows = 16;
        constexpr std::size_t MaximumMessageBytes = 3U << 10U;

        /** @brief Bounds the raw message projection before the query makes an owned copy. */
        bool FitsDiagnosticBudget(const std::vector<std::string> &messages) {
            auto remaining = MaximumMessageBytes;
            for (const auto &message : messages) {
                if (message.size() > remaining)
                    return false;
                remaining -= message.size();
            }
            return true;
        }

        /** @brief Publishes only the existing diagnostic Query adapter into a new worker-owned registry. */
        Session OpenDiagnosticsSession(std::vector<Mcp::McpToolRegistration> tools, const CodeQueryContext &context,
                                       std::shared_ptr<Security::SecureRandomSource> entropy) {
            std::erase_if(tools, [](const auto &tool) {
                return tool.descriptor.id.value != ProjectDiagnosticsTool;
            });
            if (tools.size() != 1)
                return Session::Failure(MakeError(CodeQueryErrors::Unavailable));
            auto grants = tools.front().descriptor.requiredCapabilities;
            auto auth = std::make_shared<Mcp::McpAuthorization>(std::move(entropy));
            // This private authority contains only the user-selected, read-only observation.
            if (const auto admitted = auth->SetTrust(context.projectIdentity, context.projectGeneration, true); admitted.HasError())
                return Session::Failure(admitted.ErrorValue());
            auto registry = std::make_shared<Mcp::McpToolRegistry>(auth);
            const auto revision = registry->Publish(std::move(tools), grants);
            if (revision.HasError())
                return Session::Failure(revision.ErrorValue());
            auto controller = Mcp::McpController::Create(registry, {}, auth);
            if (controller.HasError())
                return Session::Failure(controller.ErrorValue());
            if (const auto bound = controller.Value()->BindOwner(Owner); bound.HasError())
                return Session::Failure(bound.ErrorValue());
            auto manager = Mcp::McpSessionManager::Create(controller.Value(), {}, auth);
            if (manager.HasError())
                return Session::Failure(manager.ErrorValue());
            Mcp::McpSessionAdmission admission{.clientIdentity = "editor-context-diagnostics",
                                               .capabilities = std::move(grants),
                                               .projectIdentity = context.projectIdentity,
                                               .authorizationRevision = context.projectGeneration,
                                               .registryRevision = revision.Value()};
            auto credential = auth->IssueCredential(admission, std::chrono::minutes{1});
            if (credential.HasError())
                return Session::Failure(credential.ErrorValue());
            auto principal = auth->Authenticate(admission, std::move(credential).Value());
            if (principal.HasError())
                return Session::Failure(principal.ErrorValue());
            admission.authority = std::move(principal).Value();
            auto embedded = Mcp::McpInProcessAdapter::Start(std::move(manager).Value(), std::move(admission));
            if (embedded.HasError())
                return Session::Failure(embedded.ErrorValue());
            return Session::Success(AgentTurnMcp{std::move(embedded).Value(),
                                                 {context.projectIdentity, context.projectGeneration},
                                                 [controller = std::move(controller).Value()] {
                const auto pumped = controller->Pump(Owner);
                return pumped.HasValue() ? Result<void>::Success() : Result<void>::Failure(pumped.ErrorValue());
            }});
        }

        /** @brief Projects only retained diagnostics, without exposing a store or native path. */
        CodeQueryProviders DiagnosticProvider(AgentDiagnosticCapture input, const CodeQueryContext &context) {
            CodeQueryObservation captured{context.projectIdentity,
                                          context.projectGeneration,
                                          std::move(input.revision),
                                          {},
                                          input.truncated ? 1U : 0U};
            captured.records.reserve(input.messages.size());
            std::uint64_t ordinal{};
            for (auto &message : input.messages)
                captured.records.push_back({.kind = CodeQueryRecordKind::Diagnostic, .label = std::move(message), .byteOffset = ordinal++});
            return {.diagnostics = [captured = std::move(captured), current = std::move(input.isCurrent)](const CodeQueryRequest &,
                                                                                                          const CodeQueryContext &) {
                using Observation = Result<CodeQueryObservation>;
                return current() ? Observation::Success(captured) : Observation::Failure(MakeError(CodeQueryErrors::Stale));
            }};
        }
    }  // namespace

    /** @copydoc ReadLocalBuildDiagnostics */
    Result<Application::CodeQueryPage> ReadLocalBuildDiagnostics(BuildOutputOperationTail tail,
                                                                 const Application::CodeQueryContext &context,
                                                                 const std::uint64_t requestId,
                                                                 std::shared_ptr<Security::SecureRandomSource> entropy) {
        if (!tail.operation || !tail.lastSequence || tail.kind != BuildOutputTailKind::Diagnostics)
            return Page::Failure(MakeError(CodeQueryErrors::Invalid));
        return ReadLocalDiagnostics({std::format("operation:{}:{}", tail.operation, tail.lastSequence), std::move(tail.messages),
                                     tail.truncated,
                                     [witness = std::move(tail.witness)] {
            return witness.IsCurrent();
        }},
                                    context, requestId, std::move(entropy));
    }

    /** @copydoc ReadLocalDiagnostics */
    Result<Application::CodeQueryPage> ReadLocalDiagnostics(AgentDiagnosticCapture captured, const Application::CodeQueryContext &context,
                                                            const std::uint64_t requestId,
                                                            std::shared_ptr<Security::SecureRandomSource> entropy) {
        if (!entropy || !captured.isCurrent || captured.messages.empty() || captured.messages.size() > MaximumRows)
            return Page::Failure(MakeError(CodeQueryErrors::Invalid));
        if (!FitsDiagnosticBudget(captured.messages))
            return Page::Failure(MakeError(CodeQueryErrors::Capacity));
        if (const auto active = Platform::CheckProjectReadContext(context); active.HasError())
            return Page::Failure(active.ErrorValue());
        if (!captured.isCurrent())
            return Page::Failure(MakeError(CodeQueryErrors::Stale));
        auto generation = [current = captured.isCurrent, cancellation = context.cancellation, generation = context.projectGeneration] {
            return current() && !cancellation.IsCancellationRequested() ? generation : 0;
        };
        auto capability = ProjectCodeQuery::Create({}, context.projectIdentity, context.projectGeneration,
                                                   DiagnosticProvider(std::move(captured), context));
        if (capability.HasError())
            return Page::Failure(capability.ErrorValue());
        auto tools = Mcp::MakeProjectCodeQueryTools(std::move(capability).Value(), context.projectIdentity, context.projectGeneration,
                                                    std::move(generation), Owner);
        if (tools.HasError())
            return Page::Failure(tools.ErrorValue());
        auto session = OpenDiagnosticsSession(std::move(tools).Value(), context, std::move(entropy));
        if (session.HasError())
            return Page::Failure(session.ErrorValue());
        return session.Value().ReadContextQuery(requestId, 0, {.kind = CodeQueryKind::Diagnostics, .limit = MaximumRows}, context);
    }
}  // namespace Horo::Agent
