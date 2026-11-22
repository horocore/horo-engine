#include "EditorSourceMcpTools.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Mcp/McpErrors.h"

#include <limits>

namespace Horo::Editor {
    namespace {
        using Json = nlohmann::json;
        using JsonResult = Result<Json>;
        constexpr auto MaximumCoordinate = std::numeric_limits<std::uint32_t>::max();

        /** @brief Bounds input shape and rejects out-of-range coordinates at schema admission. */
        Json SourceInputSchema(const bool navigate) {
            Json properties{{"path", {{"type", "string"}, {"minLength", 1}, {"maxLength", 1024}}}};
            Json required = Json::array({"path"});
            if (navigate) {
                const Json coordinate{{"type", "integer"}, {"minimum", 1}, {"maximum", MaximumCoordinate}};
                properties["line"] = coordinate;
                properties["column"] = coordinate;
                required.push_back("line");
                required.push_back("column");
            }
            return {{"type", "object"},
                    {"properties", std::move(properties)},
                    {"required", std::move(required)},
                    {"additionalProperties", false}};
        }

        /** @brief Translates one validated one-based UTF-16 position to the shared source-coordinate contract. */
        std::optional<EditorSourceCommandPosition> Destination(const Json &arguments, const bool navigate) {
            if (!navigate)
                return std::nullopt;
            return EditorSourceCommandPosition{arguments.at("line").get<std::uint32_t>() - 1,
                                               arguments.at("column").get<std::uint32_t>() - 1};
        }

        /** @brief Dispatches only on the exact active editor workspace that issued the registration. */
        class SourceTool final : public Mcp::IMcpToolAdapter {
        public:
            SourceTool(std::weak_ptr<const EditorSourceCommandEndpoint> endpoint, const bool navigate)
                : endpoint_(std::move(endpoint)), navigate_(navigate) {}

            JsonResult Invoke(const Json &arguments, const Mcp::McpRequestContext &context) override {
                const auto endpoint = endpoint_.lock();
                if (!endpoint || endpoint->owner != std::this_thread::get_id())
                    return JsonResult::Failure(MakeError(Mcp::McpErrors::OwnerUnavailable));
                if (!context.projectIdentity || *context.projectIdentity != endpoint->project ||
                    context.authorizationRevision != endpoint->authorizationRevision)
                    return JsonResult::Failure(MakeError(Mcp::McpErrors::AuthorizationDenied));
                if (context.IsStopRequested())
                    return JsonResult::Failure(MakeError(Mcp::McpErrors::RequestCancelled));
                const auto &path = arguments.at("path").get_ref<const std::string &>();
                if (!IsValidUtf8ScalarSequence(path))
                    return JsonResult::Failure(MakeError(Mcp::McpErrors::ToolInputInvalid));
                const auto source = SourceDocumentId::Parse(path);
                if (source.HasError())
                    return JsonResult::Failure(MakeError(Mcp::McpErrors::ToolInputInvalid));
                auto presented = endpoint->present(source.Value(), Destination(arguments, navigate_), [&context] {
                    return context.IsStopRequested();
                });
                if (presented.HasError()) {
                    const auto &error = presented.ErrorValue();
                    return JsonResult::Failure({.code = error.code,
                                                .domain = error.domain,
                                                .severity = error.severity,
                                                .message = "The editor could not present the requested project source."});
                }
                return JsonResult::Success({{"presented", true}});
            }

        private:
            std::weak_ptr<const EditorSourceCommandEndpoint> endpoint_;
            const bool navigate_;
        };

        /** @brief Describes a presentation effect, which the registry admits only with exact approval. */
        Mcp::McpToolRegistration Registration(const std::shared_ptr<const EditorSourceCommandEndpoint> &endpoint, const bool navigate) {
            return {.descriptor = {.id = {navigate ? "editor.source.navigate" : "editor.source.open"},
                                   .description = navigate ? "Navigate the embedded editor to a project source position using one-based "
                                                             "lines and UTF-16 columns."
                                                           : "Open or focus a project source in the embedded editor.",
                                   .inputSchema = SourceInputSchema(navigate),
                                   .outputSchema = {{"type", "object"},
                                                    {"properties", {{"presented", {{"type", "boolean"}}}}},
                                                    {"required", {"presented"}},
                                                    {"additionalProperties", false}},
                                   .effect = Mcp::McpToolEffect::PresentationSideEffect,
                                   .requiredCapabilities = {std::string{EditorSourcePresentationGrant}},
                                   .bounds = {.maximumInputBytes = 2048, .maximumResultBytes = 256, .maximumDepth = 4, .maximumNodes = 16}},
                    .adapter = std::make_shared<SourceTool>(endpoint, navigate),
                    .owner = Mcp::McpOwnerContext::Editor};
        }
    }  // namespace

    /** @copydoc MakeEditorSourceMcpTools */
    Result<std::vector<Mcp::McpToolRegistration>> MakeEditorSourceMcpTools(std::weak_ptr<const EditorSourceCommandEndpoint> workspace) {
        using Output = Result<std::vector<Mcp::McpToolRegistration>>;
        const auto endpoint = workspace.lock();
        if (!endpoint || endpoint->owner != std::this_thread::get_id() || !endpoint->present || endpoint->project.empty() ||
            endpoint->project.size() > 256 || !IsValidUtf8ScalarSequence(endpoint->project) || !endpoint->authorizationRevision)
            return Output::Failure(MakeError(Mcp::McpErrors::ConfigurationInvalid));
        return Output::Success({Registration(endpoint, false), Registration(endpoint, true)});
    }
}  // namespace Horo::Editor
