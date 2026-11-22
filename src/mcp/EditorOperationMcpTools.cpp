#include "EditorBuildMcpTools.h"
#include "EditorTestMcpTools.h"
#include "Horo/Foundation/Utf8.h"
#include "Horo/Mcp/McpErrors.h"

#include <algorithm>
#include <array>

namespace Horo::Editor {
    namespace {
        using Json = nlohmann::json;
        using Output = Result<Json>;
        using Id = std::uint64_t;

        enum class Action {
            Start,
            Status,
            Cancel
        };

        /** @brief Retains only the latest command-owned operation and its exact authenticated session. */
        struct OwnedOperation final {
            Id id{};
            Mcp::McpSessionHandle session;
            std::weak_ptr<const Mcp::McpAuthority> authority;

            /** @brief Matches the exact session and opaque decimal identifier supplied by start. */
            bool Matches(const Mcp::McpRequestContext &context, const std::string &requested) const {
                return id != 0 && context.session == session && authority.lock() == context.authority && requested == std::to_string(id);
            }
        };

        /** @brief Describes the stable wire vocabulary and grants of one family of operations. */
        struct CommandPack final {
            std::string_view prefix;
            std::string_view idKey;
            std::string_view label;
            std::string_view executionGrant;
            std::string_view observationGrant;
        };

        /** @brief Serializes the native phase with a stable machine-readable name. */
        std::string_view StateName(const Application::GameplayBuildState state) {
            using enum Application::GameplayBuildState;

            struct PhaseName {
                Application::GameplayBuildState phase;
                std::string_view name;
            };

            constexpr std::array names{
                PhaseName{Queued, "queued"},
                PhaseName{AcquiringLock, "acquiring_lock"},
                PhaseName{WaitingForExternalBuild, "waiting_for_external_build"},
                PhaseName{Configuring, "configuring"},
                PhaseName{Building, "building"},
                PhaseName{Validating, "validating"},
                PhaseName{Succeeded, "succeeded"},
                PhaseName{Failed, "failed"},
                PhaseName{Cancelled, "cancelled"},
                PhaseName{TimedOut, "timed_out"},
            };
            const auto found = std::ranges::find(names, state, &PhaseName::phase);
            return found != names.end() ? found->name : "unknown";
        }

        /** @brief Names test status using the same lifecycle exposed in the native operation panel. */
        std::string_view StateName(const OperationState state) {
            using enum OperationState;
            switch (state) {
                case Queued:
                    return "queued";
                case Running:
                    return "running";
                case Waiting:
                    return "waiting";
                case Cancelling:
                    return "cancelling";
                case Succeeded:
                    return "succeeded";
                case Failed:
                    return "failed";
                case Cancelled:
                    return "cancelled";
            }
            return "unknown";
        }

        std::optional<OperationId> OutputIdentity(const Application::GameplayBuildSnapshot &snapshot) {
            return snapshot.operationId;
        }

        std::optional<OperationId> OutputIdentity(const Application::GameplayTestSnapshot &snapshot) {
            return snapshot.operation;
        }

        /** @brief Correlates native operation output without transferring path-bearing diagnostics. */
        template <typename Endpoint> Output Observe(const Endpoint &endpoint, const Id id, const std::string_view idKey) {
            const auto snapshot = endpoint.status(id);
            if (!snapshot)
                return Output::Failure(MakeError(Mcp::McpErrors::OperationUnavailable));
            Json result{{std::string{idKey}, std::to_string(id)},
                        {"state", StateName(snapshot->state)},
                        {"cancellationRequested", snapshot->cancellationRequested}};
            if (const auto operation = OutputIdentity(*snapshot))
                result["operationId"] = std::to_string(*operation);
            if (snapshot->error)
                result["error"] = Mcp::SafeErrorData(*snapshot->error);
            return Output::Success(std::move(result));
        }

        /** @brief Admits commands against the exact workspace and requester that started the operation. */
        template <typename Endpoint> class OperationTool final : public Mcp::IMcpToolAdapter {
        public:
            OperationTool(std::weak_ptr<const Endpoint> endpoint, std::shared_ptr<OwnedOperation> owned, const Action action,
                          const std::string_view idKey)
                : endpoint_(std::move(endpoint)), owned_(std::move(owned)), action_(action), idKey_(idKey) {}

            Output Invoke(const Json &arguments, const Mcp::McpRequestContext &context) override {
                const auto endpoint = endpoint_.lock();
                if (!endpoint || endpoint->owner != std::this_thread::get_id())
                    return Output::Failure(MakeError(Mcp::McpErrors::OwnerUnavailable));
                if (context.projectIdentity != endpoint->project || context.authorizationRevision != endpoint->authorizationRevision)
                    return Output::Failure(MakeError(Mcp::McpErrors::AuthorizationDenied));
                if (context.IsStopRequested())
                    return Output::Failure(MakeError(Mcp::McpErrors::RequestCancelled));
                if (action_ == Action::Start)
                    return Start(*endpoint, context);
                if (!owned_->Matches(context, arguments.at(idKey_).get_ref<const std::string &>()))
                    return Output::Failure(MakeError(Mcp::McpErrors::OperationUnavailable));
                if (action_ == Action::Cancel)
                    static_cast<void>(endpoint->cancel(owned_->id));
                return Observe(*endpoint, owned_->id, idKey_);
            }

        private:
            /** @brief Starts the fixed workspace operation and rolls it back if authority is lost before acknowledgment. */
            Output Start(const Endpoint &endpoint, const Mcp::McpRequestContext &context) {
                const auto started = endpoint.start([&context] {
                    return context.IsStopRequested();
                });
                if (started.HasError()) {
                    const auto &error = started.ErrorValue();
                    return Output::Failure({.code = error.code,
                                            .domain = error.domain,
                                            .severity = error.severity,
                                            .message = "The editor could not start the project operation."});
                }
                if (context.IsStopRequested()) {
                    static_cast<void>(endpoint.cancel(started.Value()));
                    return Output::Failure(MakeError(Mcp::McpErrors::RequestCancelled));
                }
                *owned_ = {started.Value(), context.session, context.authority};
                return Observe(endpoint, started.Value(), idKey_);
            }

            std::weak_ptr<const Endpoint> endpoint_;
            std::shared_ptr<OwnedOperation> owned_;
            Action action_;
            std::string idKey_;
        };

        /** @brief Describes one command without selecting a toolchain, build preset, or native path. */
        template <typename Endpoint>
        Mcp::McpToolRegistration RegisterTool(const CommandPack &pack, const Action action, const std::string_view suffix,
                                              std::string description, const std::shared_ptr<const Endpoint> &endpoint,
                                              const std::shared_ptr<OwnedOperation> &owned) {
            const std::string key{pack.idKey};
            const Json id{{"type", "string"}, {"minLength", 1}, {"maxLength", 20}};
            Json input{{"type", "object"}, {"additionalProperties", false}};
            if (action != Action::Start) {
                input["properties"] = {{key, id}};
                input["required"] = {key};
            }
            return {.descriptor = {.id = {std::string{pack.prefix} + "." + std::string{suffix}},
                                   .description = std::move(description),
                                   .inputSchema = std::move(input),
                                   .outputSchema = {{"type", "object"},
                                                    {"properties",
                                                     {{key, id},
                                                      {"operationId", id},
                                                      {"state", {{"type", "string"}, {"maxLength", 32}}},
                                                      {"cancellationRequested", {{"type", "boolean"}}},
                                                      {"error", {{"type", "object"}}}}},
                                                    {"required", {key, "state", "cancellationRequested"}},
                                                    {"additionalProperties", false}},
                                   .effect = action == Action::Status ? Mcp::McpToolEffect::Query : Mcp::McpToolEffect::Mutation,
                                   .requiredCapabilities = {std::string{action == Action::Status ? pack.observationGrant
                                                                                                 : pack.executionGrant}}},
                    .adapter = std::make_shared<OperationTool<Endpoint>>(endpoint, owned, action, pack.idKey),
                    .owner = Mcp::McpOwnerContext::Editor};
        }

        /** @brief Creates one command family over a live editor owner without starting a process. */
        template <typename Endpoint>
        Result<std::vector<Mcp::McpToolRegistration>> MakeTools(std::weak_ptr<const Endpoint> workspace, const CommandPack &pack) {
            using Tools = Result<std::vector<Mcp::McpToolRegistration>>;
            const auto endpoint = workspace.lock();
            if (!endpoint || endpoint->owner != std::this_thread::get_id() || endpoint->project.empty() || endpoint->project.size() > 256 ||
                !IsValidUtf8ScalarSequence(endpoint->project) || !endpoint->authorizationRevision || !endpoint->start ||
                !endpoint->status || !endpoint->cancel)
                return Tools::Failure(MakeError(Mcp::McpErrors::ConfigurationInvalid));
            const auto owned = std::make_shared<OwnedOperation>();
            const std::string label{pack.label};
            return Tools::Success(
                {RegisterTool(pack, Action::Start, "start", "Start the editor-configured " + label + ".", endpoint, owned),
                 RegisterTool(pack, Action::Status, "status", "Observe the " + label + " started by this session.", endpoint, owned),
                 RegisterTool(pack, Action::Cancel, "cancel", "Request cancellation of this session's " + label + ".", endpoint, owned)});
        }
    }  // namespace

    /** @copydoc MakeEditorBuildMcpTools */
    Result<std::vector<Mcp::McpToolRegistration>> MakeEditorBuildMcpTools(std::weak_ptr<const EditorBuildCommandEndpoint> workspace) {
        return MakeTools(std::move(workspace),
                         {"editor.build", "buildId", "gameplay build", EditorBuildExecutionGrant, EditorBuildObservationGrant});
    }

    /** @copydoc MakeEditorTestMcpTools */
    Result<std::vector<Mcp::McpToolRegistration>> MakeEditorTestMcpTools(std::weak_ptr<const EditorTestCommandEndpoint> workspace) {
        return MakeTools(std::move(workspace),
                         {"editor.test", "testId", "gameplay test operation", EditorTestExecutionGrant, EditorTestObservationGrant});
    }
}  // namespace Horo::Editor
