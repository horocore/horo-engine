#include "EditorInspectionMcpTools.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Mcp/McpErrors.h"

namespace Horo::Editor {
    namespace {
        using Json = nlohmann::json;
        using JsonResult = Result<Json>;
        constexpr auto MaximumObjects = TransformInspectionRequest::MaximumObjects;

        /** @brief Describes the numeric Scene fence carried across pages. */
        Json SceneSchema() {
            return {{"type", "object"},
                    {"properties", {{"epoch", {{"type", "integer"}, {"minimum", 1}}}, {"revision", {{"type", "integer"}, {"minimum", 0}}}}},
                    {"required", {"epoch", "revision"}},
                    {"additionalProperties", false}};
        }

        /** @brief Rejects unbounded requests and unknown fields before dispatch. */
        Json InputSchema() {
            return {{"type", "object"},
                    {"properties",
                     {{"objects",
                       {{"type", "array"},
                        {"minItems", 1},
                        {"maxItems", MaximumObjects},
                        {"items", {{"type", "integer"}, {"minimum", 1}}}}},
                      {"offset", {{"type", "integer"}, {"minimum", 0}, {"maximum", MaximumObjects - 1}}},
                      {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", MaximumObjects}}},
                      {"scene", SceneSchema()}}},
                    {"required", {"objects"}},
                    {"additionalProperties", false}};
        }

        /** @brief Describes numeric records without names, free text, or runtime handles. */
        Json OutputSchema() {
            const auto vector = [](const std::size_t count) {
                return Json{{"type", "array"}, {"minItems", count}, {"maxItems", count}, {"items", {{"type", "number"}}}};
            };
            const Json row{{"type", "object"},
                           {"properties",
                            {{"object", {{"type", "integer"}, {"minimum", 1}}},
                             {"translation", vector(3)},
                             {"rotationXYZW", vector(4)},
                             {"scale", vector(3)}}},
                           {"required", {"object", "translation", "rotationXYZW", "scale"}},
                           {"additionalProperties", false}};
            return {{"type", "object"},
                    {"properties",
                     {{"scene", SceneSchema()},
                      {"records", {{"type", "array"}, {"maxItems", MaximumObjects}, {"items", row}}},
                      {"nextOffset", {{"type", "integer"}, {"minimum", 1}, {"maximum", MaximumObjects - 1}}}}},
                    {"required", {"scene", "records"}},
                    {"additionalProperties", false}};
        }

        /** @brief Maps schema-valid numeric fields onto the owner query. */
        TransformInspectionRequest ReadRequest(const Json &arguments) {
            TransformInspectionRequest request{arguments.at("objects").get<std::vector<std::uint64_t>>(),
                                               arguments.value("offset", std::size_t{0}), arguments.value("limit", MaximumObjects)};
            if (const auto scene = arguments.find("scene"); scene != arguments.end())
                request.expected =
                    InspectionSceneRevision{scene->at("epoch").get<std::uint64_t>(), scene->at("revision").get<std::uint64_t>()};
            return request;
        }

        /** @brief Exports an owner-produced page with an optional continuation offset. */
        Json WritePage(const TransformInspectionPage &page) {
            Json rows = Json::array();
            for (const auto &entry : page.records)
                rows.push_back({{"object", entry.object},
                                {"translation", entry.translation},
                                {"rotationXYZW", entry.rotation},
                                {"scale", entry.scale}});
            Json result{{"scene", {{"epoch", page.scene.epoch}, {"revision", page.scene.revision}}}, {"records", std::move(rows)}};
            if (page.nextOffset)
                result["nextOffset"] = *page.nextOffset;
            return result;
        }

        /** @brief Uses only the current project and exact authorization generation on the editor lane. */
        class TransformTool final : public Mcp::IMcpToolAdapter {
        public:
            explicit TransformTool(std::weak_ptr<const EditorInspectionCommandEndpoint> endpoint) : endpoint_(std::move(endpoint)) {}

            JsonResult Invoke(const Json &arguments, const Mcp::McpRequestContext &context) override {
                const auto endpoint = endpoint_.lock();
                if (!endpoint || endpoint->owner != std::this_thread::get_id())
                    return JsonResult::Failure(MakeError(Mcp::McpErrors::OwnerUnavailable));
                if (context.projectIdentity != endpoint->project || context.authorizationRevision != endpoint->authorizationRevision)
                    return JsonResult::Failure(MakeError(Mcp::McpErrors::AuthorizationDenied));
                if (context.IsStopRequested())
                    return JsonResult::Failure(MakeError(Mcp::McpErrors::RequestCancelled));
                const auto request = ReadRequest(arguments);
                if (request.offset && !request.expected)
                    return JsonResult::Failure(MakeError(Mcp::McpErrors::ToolInputInvalid));
                const auto page = endpoint->readTransforms(request);
                if (page.HasError())
                    return JsonResult::Failure(page.ErrorValue());
                return JsonResult::Success(WritePage(page.Value()));
            }

        private:
            std::weak_ptr<const EditorInspectionCommandEndpoint> endpoint_;
        };
    }  // namespace

    /** @copydoc MakeEditorInspectionMcpTools */
    Result<std::vector<Mcp::McpToolRegistration>> MakeEditorInspectionMcpTools(
        std::weak_ptr<const EditorInspectionCommandEndpoint> workspace) {
        using Output = Result<std::vector<Mcp::McpToolRegistration>>;
        const auto endpoint = workspace.lock();
        if (!endpoint || endpoint->owner != std::this_thread::get_id() || !endpoint->readTransforms || endpoint->project.empty() ||
            endpoint->project.size() > 256 || !IsValidUtf8ScalarSequence(endpoint->project) || !endpoint->authorizationRevision)
            return Output::Failure(MakeError(Mcp::McpErrors::ConfigurationInvalid));
        return Output::Success(
            {{.descriptor = {.id = {"inspect.transforms"},
                             .description = "Read a page of visible authored local transforms by exact Scene object ID.",
                             .inputSchema = InputSchema(),
                             .outputSchema = OutputSchema(),
                             .effect = Mcp::McpToolEffect::Query,
                             .requiredCapabilities = {std::string{EditorSceneInspectionGrant}},
                             .bounds = {.maximumInputBytes = 1024, .maximumResultBytes = 8192, .maximumDepth = 6, .maximumNodes = 512}},
              .adapter = std::make_shared<TransformTool>(workspace),
              .owner = Mcp::McpOwnerContext::Editor}});
    }
}  // namespace Horo::Editor
