#include "EditorInspectionMcpTools.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Mcp/McpErrors.h"

#include <limits>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        using Json = nlohmann::json;
        using JsonResult = Result<Json>;
        constexpr auto MaximumObjects = ObjectInspectionRequest::MaximumObjects;
        constexpr auto SmallestGridStep = std::numeric_limits<double>::denorm_min();

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

        /** @brief Requires a separate visible reference for paginated target measurements. */
        Json DistanceInputSchema() {
            auto result = InputSchema();
            result["properties"]["referenceObject"] = {{"type", "integer"}, {"minimum", 1}};
            result["required"].push_back("referenceObject");
            return result;
        }

        /** @brief Requires an explicit numeric grid spacing. */
        Json GridInputSchema() {
            auto result = InputSchema();
            result["properties"]["gridStep"] = {{"type", "number"}, {"minimum", SmallestGridStep}};
            result["required"].push_back("gridStep");
            return result;
        }

        /** @brief Describes numeric records without names, free text, or runtime handles. */
        Json VectorSchema(const std::size_t count) {
            return {{"type", "array"}, {"minItems", count}, {"maxItems", count}, {"items", {{"type", "number"}}}};
        }

        /** @brief Places a typed record in the shared revision-fenced page envelope. */
        Json PageSchema(const Json &row) {
            return {{"type", "object"},
                    {"properties",
                     {{"scene", SceneSchema()},
                      {"records", {{"type", "array"}, {"maxItems", MaximumObjects}, {"items", row}}},
                      {"nextOffset", {{"type", "integer"}, {"minimum", 1}, {"maximum", MaximumObjects - 1}}}}},
                    {"required", {"scene", "records"}},
                    {"additionalProperties", false}};
        }

        /** @brief Describes authored local values and the committed world pose. */
        Json TransformSchema() {
            return PageSchema(
                {{"type", "object"},
                 {"properties",
                  {{"object", {{"type", "integer"}, {"minimum", 1}}},
                   {"translation", VectorSchema(3)},
                   {"rotationXYZW", VectorSchema(4)},
                   {"scale", VectorSchema(3)},
                   {"localMatrixRowMajor", VectorSchema(16)},
                   {"worldMatrixRowMajor", VectorSchema(16)},
                   {"worldPivot", VectorSchema(3)}}},
                 {"required",
                  {"object", "translation", "rotationXYZW", "scale", "localMatrixRowMajor", "worldMatrixRowMajor", "worldPivot"}},
                 {"additionalProperties", false}});
        }

        /** @brief Describes world-space axis-aligned bounds. */
        Json BoundsSchema() {
            return PageSchema(
                {{"type", "object"},
                 {"properties",
                  {{"object", {{"type", "integer"}, {"minimum", 1}}}, {"minimum", VectorSchema(3)}, {"maximum", VectorSchema(3)}}},
                 {"required", {"object", "minimum", "maximum"}},
                 {"additionalProperties", false}});
        }

        /** @brief Enumerates the closed, payload-free set of authored component counts. */
        Json ComponentSchema() {
            Json fields = Json::object();
            Json required = Json::array();
            for (const auto name : InspectedComponents::Names) {
                fields[std::string{name}] = {{"type", "integer"}, {"minimum", 0}};
                required.push_back(name);
            }
            return PageSchema({{"type", "object"},
                               {"properties",
                                {{"object", {{"type", "integer"}, {"minimum", 1}}},
                                 {"counts",
                                  {{"type", "object"},
                                   {"properties", std::move(fields)},
                                   {"required", std::move(required)},
                                   {"additionalProperties", false}}}}},
                               {"required", {"object", "counts"}},
                               {"additionalProperties", false}});
        }

        /** @brief Describes committed geometry counters without renderer-native identities. */
        Json GeometrySchema() {
            const Json count{{"type", "integer"}, {"minimum", 1}};
            return PageSchema({{"type", "object"},
                               {"properties", {{"object", count}, {"instances", count}, {"vertices", count}, {"triangles", count}}},
                               {"required", {"object", "instances", "vertices", "triangles"}},
                               {"additionalProperties", false}});
        }

        /** @brief Describes world-space gap, overlap, nearest points and relative orientation. */
        Json DistanceSchema() {
            const Json id{{"type", "integer"}, {"minimum", 1}};
            return PageSchema(
                {{"type", "object"},
                 {"properties",
                  {{"object", id},
                   {"referenceObject", id},
                   {"gap", {{"type", "number"}, {"minimum", 0}}},
                   {"overlapDepth", VectorSchema(3)},
                   {"nearestReference", VectorSchema(3)},
                   {"nearestTarget", VectorSchema(3)},
                   {"forwardAngleRadians", {{"type", "number"}, {"minimum", 0}}}}},
                 {"required",
                  {"object", "referenceObject", "gap", "overlapDepth", "nearestReference", "nearestTarget", "forwardAngleRadians"}},
                 {"additionalProperties", false}});
        }

        /** @brief Describes the original and nearest aligned positions. */
        Json GridSchema() {
            return PageSchema({{"type", "object"},
                               {"properties",
                                {{"object", {{"type", "integer"}, {"minimum", 1}}},
                                 {"gridStep", {{"type", "number"}, {"minimum", SmallestGridStep}}},
                                 {"position", VectorSchema(3)},
                                 {"snappedPosition", VectorSchema(3)},
                                 {"offset", VectorSchema(3)}}},
                               {"required", {"object", "gridStep", "position", "snappedPosition", "offset"}},
                               {"additionalProperties", false}});
        }

        /** @brief Maps schema-valid numeric fields onto the owner query. */
        ObjectInspectionRequest ReadRequest(const Json &arguments) {
            ObjectInspectionRequest request{arguments.at("objects").get<std::vector<std::uint64_t>>(),
                                            arguments.value("offset", std::size_t{0}), arguments.value("limit", MaximumObjects)};
            if (const auto scene = arguments.find("scene"); scene != arguments.end())
                request.expected =
                    InspectionSceneRevision{scene->at("epoch").get<std::uint64_t>(), scene->at("revision").get<std::uint64_t>()};
            if (const auto reference = arguments.find("referenceObject"); reference != arguments.end())
                request.referenceObject = reference->get<std::uint64_t>();
            if (const auto step = arguments.find("gridStep"); step != arguments.end())
                request.gridStep = step->get<double>();
            return request;
        }

        /** @brief Serializes local and world transform values. */
        Json WriteRecord(const InspectedTransform &entry) {
            return {{"object", entry.local.object},
                    {"translation", entry.local.translation},
                    {"rotationXYZW", entry.local.rotation},
                    {"scale", entry.local.scale},
                    {"localMatrixRowMajor", entry.localMatrixRowMajor},
                    {"worldMatrixRowMajor", entry.worldMatrixRowMajor},
                    {"worldPivot", entry.worldPivot}};
        }

        /** @brief Serializes numeric world-space limits. */
        Json WriteRecord(const InspectedBounds &entry) {
            return {{"object", entry.object}, {"minimum", entry.minimum}, {"maximum", entry.maximum}};
        }

        /** @brief Serializes count values using the same closed names as schema admission. */
        Json WriteRecord(const InspectedComponents &entry) {
            Json counts = Json::object();
            for (std::size_t index = 0; index < entry.counts.size(); ++index)
                counts[std::string{InspectedComponents::Names[index]}] = entry.counts[index];
            return {{"object", entry.object}, {"counts", std::move(counts)}};
        }

        /** @brief Serializes per-object committed viewport submission counts. */
        Json WriteRecord(const InspectedGeometry &entry) {
            return {{"object", entry.object}, {"instances", entry.instances}, {"vertices", entry.vertices}, {"triangles", entry.triangles}};
        }

        /** @brief Serializes the numeric pair measurement and the two exact object identities. */
        Json WriteRecord(const InspectedDistance &entry) {
            return {{"object", entry.object},
                    {"referenceObject", entry.referenceObject},
                    {"gap", entry.gap},
                    {"overlapDepth", entry.overlapDepth},
                    {"nearestReference", entry.nearestReference},
                    {"nearestTarget", entry.nearestTarget},
                    {"forwardAngleRadians", entry.forwardAngleRadians}};
        }

        /** @brief Serializes world-space grid alignment with the exact requested spacing. */
        Json WriteRecord(const InspectedGridOffset &entry) {
            return {{"object", entry.object},
                    {"gridStep", entry.gridStep},
                    {"position", entry.position},
                    {"snappedPosition", entry.snappedPosition},
                    {"offset", entry.offset}};
        }

        /** @brief Exports an owner-produced page with an optional continuation offset. */
        template <typename Page> Json WritePage(const Page &page) {
            Json rows = Json::array();
            for (const auto &entry : page.records)
                rows.push_back(WriteRecord(entry));
            Json result{{"scene", {{"epoch", page.scene.epoch}, {"revision", page.scene.revision}}}, {"records", std::move(rows)}};
            if (page.nextOffset)
                result["nextOffset"] = *page.nextOffset;
            return result;
        }

        /** @brief Uses only the current project and exact authorization generation on the editor lane. */
        template <auto Read> class InspectionTool final : public Mcp::IMcpToolAdapter {
        public:
            explicit InspectionTool(std::weak_ptr<const EditorInspectionCommandEndpoint> endpoint) : endpoint_(std::move(endpoint)) {}

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
                const auto page = (endpoint.get()->*Read)(request);
                if (page.HasError())
                    return JsonResult::Failure(page.ErrorValue());
                return JsonResult::Success(WritePage(page.Value()));
            }

        private:
            std::weak_ptr<const EditorInspectionCommandEndpoint> endpoint_;
        };

        /** @brief Publishes a read-only query under the shared inspection capability and bounds. */
        template <auto Read>
        Mcp::McpToolRegistration Registration(const std::weak_ptr<const EditorInspectionCommandEndpoint> &workspace, std::string id,
                                              std::string description, Json input, Json schema) {
            constexpr bool includesMatrices = std::is_same_v<decltype(Read), decltype(&EditorInspectionCommandEndpoint::readTransforms)>;
            return {.descriptor = {.id = {std::move(id)},
                                   .description = std::move(description),
                                   .inputSchema = std::move(input),
                                   .outputSchema = std::move(schema),
                                   .effect = Mcp::McpToolEffect::Query,
                                   .requiredCapabilities = {std::string{EditorSceneInspectionGrant}},
                                   .bounds = {.maximumInputBytes = 1024,
                                              .maximumResultBytes = includesMatrices ? 16384U : 8192U,
                                              .maximumDepth = 6,
                                              .maximumNodes = includesMatrices ? 1024U : 512U}},
                    .adapter = std::make_shared<InspectionTool<Read>>(workspace),
                    .owner = Mcp::McpOwnerContext::Editor};
        }

        /** @brief Omits tools for owners that do not supply the typed query. */
        template <auto Read>
        void AppendInspection(std::vector<Mcp::McpToolRegistration> &tools,
                              const std::shared_ptr<const EditorInspectionCommandEndpoint> &endpoint, std::string id,
                              std::string description, Json (*schema)(), Json (*input)() = InputSchema) {
            if (endpoint.get()->*Read)
                tools.push_back(Registration<Read>(endpoint, std::move(id), std::move(description), input(), schema()));
        }
    }  // namespace

    /** @copydoc MakeEditorInspectionMcpTools */
    Result<std::vector<Mcp::McpToolRegistration>> MakeEditorInspectionMcpTools(
        std::weak_ptr<const EditorInspectionCommandEndpoint> workspace) {
        using Output = Result<std::vector<Mcp::McpToolRegistration>>;
        const auto endpoint = workspace.lock();
        if (!endpoint || endpoint->owner != std::this_thread::get_id() || endpoint->project.empty() || endpoint->project.size() > 256 ||
            !IsValidUtf8ScalarSequence(endpoint->project) || !endpoint->authorizationRevision)
            return Output::Failure(MakeError(Mcp::McpErrors::ConfigurationInvalid));
        std::vector<Mcp::McpToolRegistration> tools;
        AppendInspection<
            &EditorInspectionCommandEndpoint::readTransforms>(tools, endpoint, "inspect.transforms",
                                                              "Read visible authored local transforms and committed world poses.",
                                                              TransformSchema);
        AppendInspection<
            &EditorInspectionCommandEndpoint::readBounds>(tools, endpoint, "inspect.bounds",
                                                          "Read visible authored world-space axis-aligned bounds by exact Scene object ID.",
                                                          BoundsSchema);
        AppendInspection<&EditorInspectionCommandEndpoint::readComponents>(tools, endpoint, "inspect.components",
                                                                           "Read counts of authored components by visible Scene object ID.",
                                                                           ComponentSchema);
        AppendInspection<&EditorInspectionCommandEndpoint::readGeometry>(tools, endpoint, "inspect.rendering",
                                                                         "Read matching committed viewport instance, vertex and triangle "
                                                                         "counts by visible Scene object ID.",
                                                                         GeometrySchema);
        AppendInspection<&EditorInspectionCommandEndpoint::readDistances>(tools, endpoint, "inspect.distances",
                                                                          "Measure world-axis-aligned bounds and forward angles from a "
                                                                          "visible reference object to visible targets.",
                                                                          DistanceSchema, DistanceInputSchema);
        AppendInspection<&EditorInspectionCommandEndpoint::
                             readGridOffsets>(tools, endpoint, "inspect.grid_offsets",
                                              "Measure committed object origins against an explicit zero-origin world grid.", GridSchema,
                                              GridInputSchema);
        if (tools.empty())
            return Output::Failure(MakeError(Mcp::McpErrors::ConfigurationInvalid));
        return Output::Success(std::move(tools));
    }
}  // namespace Horo::Editor
