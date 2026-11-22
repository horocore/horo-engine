#include "EditorBuildMcpTools.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Mcp/McpErrors.h"

#include <algorithm>
#include <array>

namespace Horo::Editor {
    namespace {
        using Json = nlohmann::json;
        using Output = Result<Json>;
        using Id = EditorBuildCommandEndpoint::Id;

        enum class Action {
            Start,
            Status,
            Cancel
        };

        /** @brief Retains only the latest command-owned build and its exact authenticated session. */
        struct OwnedBuild final {
            Id id{};
            Mcp::McpSessionHandle session;
            std::weak_ptr<const Mcp::McpAuthority> authority;

            /** @brief Matches the exact session and opaque decimal identifier supplied by start. */
            bool Matches(const Mcp::McpRequestContext &context, const std::string &requested) const {
                return id != 0 && context.session == session && authority.lock() == context.authority && requested == std::to_string(id);
            }
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

        /** @brief Correlates native build output without transferring path-bearing compiler messages. */
        Output Observe(const EditorBuildCommandEndpoint &endpoint, const Id id) {
            const auto build = endpoint.status(id);
            if (!build)
                return Output::Failure(MakeError(Mcp::McpErrors::OperationUnavailable));
            Json result{{"buildId", std::to_string(id)},
                        {"state", StateName(build->state)},
                        {"cancellationRequested", build->cancellationRequested}};
            if (build->operationId)
                result["operationId"] = std::to_string(*build->operationId);
            if (build->error)
                result["error"] = Mcp::SafeErrorData(*build->error);
            return Output::Success(std::move(result));
        }

        /** @brief Admits commands against the exact workspace and requester that started the operation. */
        class BuildTool final : public Mcp::IMcpToolAdapter {
        public:
            BuildTool(std::weak_ptr<const EditorBuildCommandEndpoint> endpoint, std::shared_ptr<OwnedBuild> owned, const Action action)
                : endpoint_(std::move(endpoint)), owned_(std::move(owned)), action_(action) {}

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
                if (!owned_->Matches(context, arguments.at("buildId").get_ref<const std::string &>()))
                    return Output::Failure(MakeError(Mcp::McpErrors::OperationUnavailable));
                if (action_ == Action::Cancel)
                    static_cast<void>(endpoint->cancel(owned_->id));
                return Observe(*endpoint, owned_->id);
            }

        private:
            /** @brief Starts the fixed workspace build. Loss of authority before acknowledgment cancels the new build. */
            Output Start(const EditorBuildCommandEndpoint &endpoint, const Mcp::McpRequestContext &context) {
                const auto started = endpoint.start([&context] {
                    return context.IsStopRequested();
                });
                if (started.HasError()) {
                    const auto &error = started.ErrorValue();
                    return Output::Failure({.code = error.code,
                                            .domain = error.domain,
                                            .severity = error.severity,
                                            .message = "The editor could not start the project build."});
                }
                if (context.IsStopRequested()) {
                    static_cast<void>(endpoint.cancel(started.Value()));
                    return Output::Failure(MakeError(Mcp::McpErrors::RequestCancelled));
                }
                *owned_ = {started.Value(), context.session, context.authority};
                return Observe(endpoint, started.Value());
            }

            std::weak_ptr<const EditorBuildCommandEndpoint> endpoint_;
            std::shared_ptr<OwnedBuild> owned_;
            Action action_;
        };

        /** @brief Describes one command without selecting a toolchain, build preset, or native path. */
        Mcp::McpToolRegistration RegisterTool(const Action action, std::string name, std::string description,
                                              const std::shared_ptr<const EditorBuildCommandEndpoint> &endpoint,
                                              const std::shared_ptr<OwnedBuild> &owned) {
            const Json id{{"type", "string"}, {"minLength", 1}, {"maxLength", 20}};
            Json input{{"type", "object"}, {"additionalProperties", false}};
            if (action != Action::Start) {
                input["properties"] = {{"buildId", id}};
                input["required"] = {"buildId"};
            }
            return {.descriptor = {.id = {std::move(name)},
                                   .description = std::move(description),
                                   .inputSchema = std::move(input),
                                   .outputSchema = {{"type", "object"},
                                                    {"properties",
                                                     {{"buildId", id},
                                                      {"operationId", id},
                                                      {"state", {{"type", "string"}, {"maxLength", 32}}},
                                                      {"cancellationRequested", {{"type", "boolean"}}},
                                                      {"error", {{"type", "object"}}}}},
                                                    {"required", {"buildId", "state", "cancellationRequested"}},
                                                    {"additionalProperties", false}},
                                   .effect = action == Action::Status ? Mcp::McpToolEffect::Query : Mcp::McpToolEffect::Mutation,
                                   .requiredCapabilities = {std::string{action == Action::Status ? EditorBuildObservationGrant
                                                                                                 : EditorBuildExecutionGrant}}},
                    .adapter = std::make_shared<BuildTool>(endpoint, owned, action),
                    .owner = Mcp::McpOwnerContext::Editor};
        }
    }  // namespace

    /** @copydoc MakeEditorBuildMcpTools */
    Result<std::vector<Mcp::McpToolRegistration>> MakeEditorBuildMcpTools(std::weak_ptr<const EditorBuildCommandEndpoint> workspace) {
        using Tools = Result<std::vector<Mcp::McpToolRegistration>>;
        const auto endpoint = workspace.lock();
        if (!endpoint || endpoint->owner != std::this_thread::get_id() || endpoint->project.empty() || endpoint->project.size() > 256 ||
            !IsValidUtf8ScalarSequence(endpoint->project) || !endpoint->authorizationRevision || !endpoint->start || !endpoint->status ||
            !endpoint->cancel)
            return Tools::Failure(MakeError(Mcp::McpErrors::ConfigurationInvalid));
        const auto owned = std::make_shared<OwnedBuild>();
        return Tools::Success(
            {RegisterTool(Action::Start, "editor.build.start", "Start the editor-configured gameplay build.", endpoint, owned),
             RegisterTool(Action::Status, "editor.build.status", "Observe a build started by this session.", endpoint, owned),
             RegisterTool(Action::Cancel, "editor.build.cancel", "Request cancellation of this session's build.", endpoint, owned)});
    }
}  // namespace Horo::Editor
