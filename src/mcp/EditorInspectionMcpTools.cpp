#include "EditorInspectionMcpTools.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Mcp/McpAuthorization.h"
#include "Horo/Mcp/McpErrors.h"

#include <algorithm>
#include <limits>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        using Json = nlohmann::json;
        using JsonResult = Result<Json>;
        constexpr auto MaximumObjects = ObjectInspectionRequest::MaximumObjects;
        constexpr auto SmallestPositiveNumber = std::numeric_limits<double>::denorm_min();
        constexpr std::string_view SceneEditsTool = "scene.propose_edits";
        constexpr std::string_view SceneTransformsTool = "scene.propose_transforms";
        enum class SceneProposalKind {
            Transforms,
            Edits
        };

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

        /** @brief Pages the collider contributors of one exact object. */
        Json CollisionInputSchema() {
            auto result = InputSchema();
            auto &fields = result.at("properties");
            fields.at("objects")["maxItems"] = 1;
            fields.at("offset")["maximum"] = CollisionInspectionPage::MaximumOffset;
            fields.at("limit")["maximum"] = CollisionInspectionPage::MaximumColliders;
            return result;
        }

        /** @brief Takes an ordered fixed body pair and an exact project query channel. */
        Json PenetrationInputSchema() {
            auto result = InputSchema();
            auto &fields = result.at("properties");
            fields.at("objects")["minItems"] = 2;
            fields.at("objects")["maxItems"] = 2;
            fields.erase("offset");
            fields.erase("limit");
            fields["channel"] = {{"type", "string"}, {"minLength", 36}, {"maxLength", 36}};
            result["required"].push_back("channel");
            return result;
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
            result["properties"]["gridStep"] = {{"type", "number"}, {"minimum", SmallestPositiveNumber}};
            result["required"].push_back("gridStep");
            return result;
        }

        /** @brief Describes numeric records without names, free text, or runtime handles. */
        Json VectorSchema(const std::size_t count) {
            return {{"type", "array"}, {"minItems", count}, {"maxItems", count}, {"items", {{"type", "number"}}}};
        }

        /** @brief Checks float representability before the adapter narrows JSON numbers. */
        Json LocalTransformVectorSchema(const std::size_t count) {
            auto result = VectorSchema(count);
            constexpr auto maximum = static_cast<double>(std::numeric_limits<float>::max());
            result["items"].update({{"minimum", -maximum}, {"maximum", maximum}});
            return result;
        }

        /** @brief Describes a whole bounded named batch on one exact authored Scene revision. */
        Json ProposalInputSchema(const SceneProposalKind kind) {
            Json entry{{"type", "object"},
                       {"properties",
                        {{"object", {{"type", "integer"}, {"minimum", 1}}},
                         {"translation", LocalTransformVectorSchema(3)},
                         {"rotationXYZW", LocalTransformVectorSchema(4)},
                         {"scale", LocalTransformVectorSchema(3)}}},
                       {"required", {"object", "translation", "rotationXYZW", "scale"}},
                       {"additionalProperties", false}};
            Json schema{{"type", "object"},
                        {"properties",
                         {{"scene", SceneSchema()},
                          {"transforms", {{"type", "array"}, {"minItems", 1}, {"maxItems", MaximumObjects}, {"items", std::move(entry)}}}}},
                        {"required", {"scene", "transforms"}},
                        {"additionalProperties", false}};
            if (kind == SceneProposalKind::Edits) {
                schema["required"] = {"scene"};
                Json rename{{"type", "object"},
                            {"properties",
                             {{"object", {{"type", "integer"}, {"minimum", 1}}}, {"name", {{"type", "string"}, {"minLength", 1}}}}},
                            {"required", {"object", "name"}},
                            {"additionalProperties", false}};
                schema["properties"]["renames"] = {{"type", "array"},
                                                   {"minItems", 1},
                                                   {"maxItems", MaximumObjects},
                                                   {"items", std::move(rename)}};
                Json light{{"type", "object"},
                           {"properties", {{"object", {{"type", "integer"}, {"minimum", 1}}}, {"enabled", {{"type", "boolean"}}}}},
                           {"required", {"object", "enabled"}},
                           {"additionalProperties", false}};
                schema["properties"]["lights"] = {{"type", "array"},
                                                  {"minItems", 1},
                                                  {"maxItems", MaximumObjects},
                                                  {"items", std::move(light)}};
            }
            return schema;
        }

        /** @brief Describes a top-left-origin normalized viewport rectangle. */
        Json ViewportRectangleSchema() {
            Json point = VectorSchema(2);
            point["items"] = {{"type", "number"}, {"minimum", 0}, {"maximum", 1}};
            return {{"type", "object"},
                    {"properties", {{"minimum", point}, {"maximum", point}}},
                    {"required", {"minimum", "maximum"}},
                    {"additionalProperties", false}};
        }

        /** @brief Captures the viewport as one bounded observation to avoid cross-camera page drift. */
        Json VisualInputSchema() {
            auto result = InputSchema();
            auto &fields = result.at("properties");
            fields.erase("offset");
            fields.erase("limit");
            auto region = ViewportRectangleSchema();
            region["properties"]["aspect"] = {{"type", "number"},
                                              {"minimum", std::numeric_limits<float>::denorm_min()},
                                              {"maximum", std::numeric_limits<float>::max()}};
            region["required"].push_back("aspect");
            fields["viewportRegion"] = std::move(region);
            result["required"].push_back("viewportRegion");
            return result;
        }

        /** @brief Requires a bounded world-space ray for every surface page. */
        Json SurfaceInputSchema() {
            auto result = InputSchema();
            constexpr auto extent = std::numeric_limits<float>::max();
            Json vector = VectorSchema(3);
            vector["items"] = {{"type", "number"}, {"minimum", -extent}, {"maximum", extent}};
            result["properties"]["ray"] = {{"type", "object"},
                                           {"properties",
                                            {{"origin", vector},
                                             {"direction", vector},
                                             {"maximumDistance",
                                              {{"type", "number"}, {"minimum", SmallestPositiveNumber}, {"maximum", extent}}}}},
                                           {"required", {"origin", "direction", "maximumDistance"}},
                                           {"additionalProperties", false}};
            result["required"].push_back("ray");
            return result;
        }

        /** @brief Places a typed record in the shared revision-fenced page envelope. */
        Json PageSchema(const Json &row, const std::size_t maxItems = MaximumObjects, const std::size_t maxOffset = MaximumObjects - 1) {
            return {{"type", "object"},
                    {"properties",
                     {{"scene", SceneSchema()},
                      {"records", {{"type", "array"}, {"maxItems", maxItems}, {"items", row}}},
                      {"nextOffset", {{"type", "integer"}, {"minimum", 1}, {"maximum", maxOffset}}}}},
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

        /** @brief Describes both committed world-space enclosures. */
        Json BoundsSchema() {
            const Json volume{{"type", "number"}, {"minimum", 0}};
            const Json oriented{{"type", "object"},
                                {"properties",
                                 {{"center", VectorSchema(3)},
                                  {"halfExtents", VectorSchema(3)},
                                  {"volume", volume},
                                  {"axes", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", VectorSchema(3)}}}}},
                                {"required", {"center", "halfExtents", "volume", "axes"}},
                                {"additionalProperties", false}};
            return PageSchema({{"type", "object"},
                               {"properties",
                                {{"object", {{"type", "integer"}, {"minimum", 1}}},
                                 {"minimum", VectorSchema(3)},
                                 {"maximum", VectorSchema(3)},
                                 {"center", VectorSchema(3)},
                                 {"halfExtents", VectorSchema(3)},
                                 {"volume", volume},
                                 {"oriented", oriented}}},
                               {"required", {"object", "minimum", "maximum", "center", "halfExtents", "volume", "oriented"}},
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
                                 {"gridStep", {{"type", "number"}, {"minimum", SmallestPositiveNumber}}},
                                 {"position", VectorSchema(3)},
                                 {"snappedPosition", VectorSchema(3)},
                                 {"offset", VectorSchema(3)}}},
                               {"required", {"object", "gridStep", "position", "snappedPosition", "offset"}},
                               {"additionalProperties", false}});
        }

        /** @brief A sample exists only when the ray intersects the target mesh. */
        Json SurfaceSchema() {
            const Json sample{{"type", "object"},
                              {"properties",
                               {{"position", VectorSchema(3)},
                                {"normal", VectorSchema(3)},
                                {"distance", {{"type", "number"}, {"minimum", 0}}},
                                {"slopeRadians", {{"type", "number"}, {"minimum", 0}}}}},
                              {"required", {"position", "normal", "distance", "slopeRadians"}},
                              {"additionalProperties", false}};
            return PageSchema({{"type", "object"},
                               {"properties", {{"object", {{"type", "integer"}, {"minimum", 1}}}, {"sample", sample}}},
                               {"required", {"object"}},
                               {"additionalProperties", false}});
        }

        /** @brief Describes the closed set of authored collider fields. */
        Json CollisionSchema() {
            const Json id{{"type", "integer"}, {"minimum", 1}};
            const Json flag{{"type", "boolean"}};
            const Json dimension{{"type", "number"}, {"minimum", 0}};
            const Json shape{{"type", "object"},
                             {"properties",
                              {{"kind", {{"type", "string"}, {"enum", {"box", "sphere", "capsule", "plane", "asset"}}}},
                               {"halfExtentsMeters", VectorSchema(3)},
                               {"radiusMeters", dimension},
                               {"cylindricalHalfHeightMeters", dimension},
                               {"normal", VectorSchema(3)},
                               {"signedDistanceMeters", {{"type", "number"}}}}},
                             {"required", {"kind"}},
                             {"additionalProperties", false}};
            return PageSchema({{"type", "object"},
                               {"properties",
                                {{"object", id},
                                 {"slot", id},
                                 {"bodyObject", id},
                                 {"bodySlot", id},
                                 {"translationMeters", VectorSchema(3)},
                                 {"rotationXYZW", VectorSchema(4)},
                                 {"scale", VectorSchema(3)},
                                 {"shape", shape},
                                 {"enabled", flag},
                                 {"sensor", flag}}},
                               {"required",
                                {"object", "slot", "bodyObject", "bodySlot", "translationMeters", "rotationXYZW", "scale", "shape",
                                 "enabled", "sensor"}},
                               {"additionalProperties", false}},
                              CollisionInspectionPage::MaximumColliders, CollisionInspectionPage::MaximumOffset);
        }

        /** @brief Describes the strongest contact against the requested body when one exists. */
        Json PenetrationSchema() {
            const Json id{{"type", "integer"}, {"minimum", 1}};
            const Json sample{{"type", "object"},
                              {"properties",
                               {{"depthMeters", {{"type", "number"}, {"minimum", SmallestPositiveNumber}}},
                                {"originFramePositionMeters", VectorSchema(3)}}},
                              {"required", {"depthMeters", "originFramePositionMeters"}},
                              {"additionalProperties", false}};
            return PageSchema({{"type", "object"},
                               {"properties", {{"object", id}, {"referenceObject", id}, {"sample", sample}}},
                               {"required", {"object", "referenceObject"}},
                               {"additionalProperties", false}},
                              1);
        }

        /** @brief Describes each object's clipped bounds and strongest projected overlap. */
        Json VisualSchema() {
            const Json id{{"type", "integer"}, {"minimum", 1}};
            const Json overlap{{"type", "object"},
                               {"properties",
                                {{"object", id}, {"area", {{"type", "number"}, {"minimum", SmallestPositiveNumber}, {"maximum", 1}}}}},
                               {"required", {"object", "area"}},
                               {"additionalProperties", false}};
            return PageSchema({{"type", "object"},
                               {"properties",
                                {{"object", id},
                                 {"regionBounds", ViewportRectangleSchema()},
                                 {"clipped", {{"type", "boolean"}}},
                                 {"largestOverlap", overlap}}},
                               {"required", {"object", "clipped"}},
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
            if (const auto ray = arguments.find("ray"); ray != arguments.end())
                request.surfaceRay =
                    InspectionSurfaceRay{ray->at("origin").get<std::array<double, 3>>(), ray->at("direction").get<std::array<double, 3>>(),
                                         ray->at("maximumDistance").get<double>()};
            if (const auto channel = arguments.find("channel"); channel != arguments.end())
                request.physicsChannel = channel->get<std::string>();
            if (const auto region = arguments.find("viewportRegion"); region != arguments.end())
                request.viewportRegion = InspectionViewportRegion{{region->at("minimum").get<std::array<double, 2>>(),
                                                                   region->at("maximum").get<std::array<double, 2>>()},
                                                                  region->at("aspect").get<float>()};
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
            std::array<double, 3> center{};
            std::array<double, 3> halfExtents{};
            for (std::size_t axis = 0; axis < center.size(); ++axis) {
                const double minimum = entry.minimum[axis];
                halfExtents[axis] = (static_cast<double>(entry.maximum[axis]) - minimum) * 0.5;
                center[axis] = minimum + halfExtents[axis];
            }
            Json record{{"object", entry.object},     {"minimum", entry.minimum},
                        {"maximum", entry.maximum},   {"center", center},
                        {"halfExtents", halfExtents}, {"volume", 8.0 * halfExtents[0] * halfExtents[1] * halfExtents[2]}};
            if (entry.oriented) {
                const auto &box = *entry.oriented;
                record["oriented"] = {{"center", box.center}, {"axes", box.axes}, {"halfExtents", box.halfExtents}, {"volume", box.volume}};
            }
            return record;
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

        /** @brief Emits one exact target ID and an optional surface sample. */
        Json WriteRecord(const InspectedSurface &entry) {
            Json result{{"object", entry.object}};
            if (entry.sample) {
                const auto &sample = *entry.sample;
                result["sample"] = {{"distance", sample.distance},
                                    {"position", sample.position},
                                    {"normal", sample.normal},
                                    {"slopeRadians", sample.slopeRadians}};
            }
            return result;
        }

        /** @brief Converts each typed shape to its closed wire representation. */
        struct WriteColliderShape {
            Json operator()(const InspectedColliderBox &shape) const {
                return {{"kind", "box"}, {"halfExtentsMeters", shape.halfExtentsMeters}};
            }

            Json operator()(const InspectedColliderSphere &shape) const {
                return {{"kind", "sphere"}, {"radiusMeters", shape.radiusMeters}};
            }

            Json operator()(const InspectedColliderCapsule &shape) const {
                return {{"kind", "capsule"},
                        {"radiusMeters", shape.radiusMeters},
                        {"cylindricalHalfHeightMeters", shape.cylindricalHalfHeightMeters}};
            }

            Json operator()(const InspectedColliderPlane &shape) const {
                return {{"kind", "plane"}, {"normal", shape.normal}, {"signedDistanceMeters", shape.signedDistanceMeters}};
            }

            Json operator()(const InspectedColliderAsset &) const {
                return {{"kind", "asset"}};
            }
        };

        /** @brief Emits exact authored collider identities and numeric placement. */
        Json WriteRecord(const InspectedCollider &entry) {
            return {{"object", entry.object},
                    {"slot", entry.slot},
                    {"bodyObject", entry.bodyObject},
                    {"bodySlot", entry.bodySlot},
                    {"translationMeters", entry.translationMeters},
                    {"rotationXYZW", entry.rotationXYZW},
                    {"scale", entry.scale},
                    {"shape", std::visit(WriteColliderShape{}, entry.shape)},
                    {"enabled", entry.enabled},
                    {"sensor", entry.sensor}};
        }

        /** @brief Emits the exact authored pair and optional origin-frame contact. */
        Json WriteRecord(const InspectedPenetration &entry) {
            Json result{{"object", entry.object}, {"referenceObject", entry.referenceObject}};
            if (entry.sample)
                result["sample"] = {{"depthMeters", entry.sample->depthMeters},
                                    {"originFramePositionMeters", entry.sample->positionMeters}};
            return result;
        }

        /** @brief Emits visible geometric evidence without asset names or rendered data. */
        Json WriteRecord(const InspectedVisualIssues &entry) {
            Json result{{"object", entry.object}, {"clipped", entry.clipped}};
            if (entry.regionBounds)
                result["regionBounds"] = {{"minimum", entry.regionBounds->minimum}, {"maximum", entry.regionBounds->maximum}};
            if (entry.largestOverlap)
                result["largestOverlap"] = {{"object", entry.largestOverlap->object}, {"area", entry.largestOverlap->area}};
            return result;
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

        /** @brief Decodes one schema-validated optional list of typed Scene edits. */
        template <class T, class Decode> std::vector<T> ReadProposedEdits(const Json &arguments, const char *field, Decode decode) {
            std::vector<T> values;
            if (const auto rows = arguments.find(field); rows != arguments.end()) {
                values.reserve(rows->size());
                for (const auto &row : *rows)
                    values.push_back(decode(row));
            }
            return values;
        }

        /** @brief Exact typed payload shared by planning review and the editor preview adapter. */
        struct DecodedSceneProposal final {
            InspectionSceneRevision revision;
            std::vector<AgentViewportTransform> transforms;
            std::vector<AgentSceneObjectRename> renames;
            std::vector<AgentSceneLightUpdate> lights;
        };

        /** @brief An object may appear in multiple edit kinds, but each property has one proposed writer. */
        template <class Edit> bool HasDistinctEditTargets(const std::vector<Edit> &edits) {
            return std::ranges::all_of(edits, [&edits](const auto &edit) {
                return std::ranges::count(edits, edit.object, &Edit::object) == 1;
            });
        }

        /** @brief Applies the scene math contract before treating a proposed rotation as reviewable. */
        bool HasValidProposalRotation(const AgentViewportTransform &transform) {
            const auto &rotation = transform.rotation;
            return Math::Quaternion{rotation[0], rotation[1], rotation[2], rotation[3]}.TryNormalized().HasValue();
        }

        /** @brief Converts schema-validated Scene proposal input and checks the combined edit budget. */
        Result<DecodedSceneProposal> ReadSceneProposal(const Json &arguments) {
            const auto &scene = arguments.at("scene");
            const InspectionSceneRevision revision{scene.at("epoch").get<std::uint64_t>(), scene.at("revision").get<std::uint64_t>()};
            auto transforms = ReadProposedEdits<AgentViewportTransform>(arguments, "transforms", [](const Json &row) {
                return AgentViewportTransform{row.at("object").get<std::uint64_t>(), row.at("translation").get<std::array<float, 3>>(),
                                              row.at("rotationXYZW").get<std::array<float, 4>>(),
                                              row.at("scale").get<std::array<float, 3>>()};
            });
            auto renames = ReadProposedEdits<AgentSceneObjectRename>(arguments, "renames", [](const Json &row) {
                return AgentSceneObjectRename{row.at("object").get<std::uint64_t>(), row.at("name").get<std::string>()};
            });
            auto lights = ReadProposedEdits<AgentSceneLightUpdate>(arguments, "lights", [](const Json &row) {
                return AgentSceneLightUpdate{row.at("object").get<std::uint64_t>(), row.at("enabled").get<bool>()};
            });
            const auto count = transforms.size() + renames.size() + lights.size();
            if (count == 0 || count > MaximumObjects)
                return Result<DecodedSceneProposal>::Failure(MakeError(Mcp::McpErrors::ToolInputInvalid));
            if (!HasDistinctEditTargets(transforms) || !HasDistinctEditTargets(renames) || !HasDistinctEditTargets(lights) ||
                !std::ranges::all_of(transforms, HasValidProposalRotation))
                return Result<DecodedSceneProposal>::Failure(MakeError(Mcp::McpErrors::ToolInputInvalid));
            return Result<DecodedSceneProposal>::Success({revision, std::move(transforms), std::move(renames), std::move(lights)});
        }

        /** @brief Lists each affected object once in proposal order without querying live Editor state. */
        Result<Mcp::McpToolDraftScope> InspectSceneProposal(const Json &arguments) {
            auto proposal = ReadSceneProposal(arguments);
            if (proposal.HasError())
                return Result<Mcp::McpToolDraftScope>::Failure(proposal.ErrorValue());
            const auto &value = proposal.Value();
            Mcp::McpToolDraftScope scope{.opening = value.revision.epoch, .revision = value.revision.revision};
            const auto collect = [&scope](const auto &edits) {
                for (const auto &edit : edits)
                    if (std::ranges::find(scope.objects, edit.object) == scope.objects.end())
                        scope.objects.push_back(edit.object);
            };
            collect(value.transforms);
            collect(value.renames);
            collect(value.lights);
            return Result<Mcp::McpToolDraftScope>::Success(std::move(scope));
        }

        /** @brief Sends the shared validated Scene payload to the existing preview owner. */
        Result<std::uint64_t> StageSceneProposal(const EditorInspectionCommandEndpoint &endpoint, const Json &arguments,
                                                 std::function<bool()> authorized) {
            const auto proposal = ReadSceneProposal(arguments);
            if (proposal.HasError())
                return Result<std::uint64_t>::Failure(proposal.ErrorValue());
            const auto &edits = proposal.Value();
            return endpoint.proposeEdits(edits.revision, edits.transforms, edits.renames, edits.lights, std::move(authorized));
        }

        /** @brief Stages the exact proposal on the editor lane and retains a revocable, weak principal lease. */
        class SceneProposalTool final : public Mcp::IMcpToolAdapter {
        public:
            explicit SceneProposalTool(std::weak_ptr<const EditorInspectionCommandEndpoint> endpoint) : endpoint_(std::move(endpoint)) {}

            JsonResult Invoke(const Json &arguments, const Mcp::McpRequestContext &context) override {
                const auto endpoint = endpoint_.lock();
                if (!endpoint || endpoint->owner != std::this_thread::get_id() || !endpoint->proposeEdits)
                    return JsonResult::Failure(MakeError(Mcp::McpErrors::OwnerUnavailable));
                if (!context.authority || context.projectIdentity != endpoint->project ||
                    context.authorizationRevision != endpoint->authorizationRevision)
                    return JsonResult::Failure(MakeError(Mcp::McpErrors::AuthorizationDenied));
                if (context.IsStopRequested())
                    return JsonResult::Failure(MakeError(Mcp::McpErrors::RequestCancelled));
                auto authorized = [principal = std::weak_ptr<const Mcp::McpAuthority>{context.authority}] {
                    const auto active = principal.lock();
                    return active && !active->IsStopRequested();
                };
                const auto result = StageSceneProposal(*endpoint, arguments, std::move(authorized));
                if (result.HasError())
                    return JsonResult::Failure(result.ErrorValue());
                return JsonResult::Success({{"reviewId", std::to_string(result.Value())}});
            }

        private:
            std::weak_ptr<const EditorInspectionCommandEndpoint> endpoint_;
        };

        /** @brief Publishes a review-only tool under its own capability. */
        Mcp::McpToolRegistration ProposalRegistration(const std::shared_ptr<const EditorInspectionCommandEndpoint> &endpoint,
                                                      const SceneProposalKind kind) {
            const bool broader = kind == SceneProposalKind::Edits;
            return {.descriptor =
                        {.id = {std::string{broader ? SceneEditsTool : SceneTransformsTool}},
                         .description = broader
                                            ? "Preview authored names, local transforms, and light enablement for local operator review."
                                            : "Preview authored local transforms in the viewport for local operator review.",
                         .inputSchema = ProposalInputSchema(kind),
                         .outputSchema = {{"type", "object"},
                                          {"properties", {{"reviewId", {{"type", "string"}, {"minLength", 1}, {"maxLength", 20}}}}},
                                          {"required", {"reviewId"}},
                                          {"additionalProperties", false}},
                         .effect = Mcp::McpToolEffect::PresentationSideEffect,
                         .requiredCapabilities = {std::string{EditorSceneProposalGrant}},
                         .bounds = {.maximumInputBytes = 8192, .maximumResultBytes = 128, .maximumDepth = 6, .maximumNodes = 384}},
                    .adapter = std::make_shared<SceneProposalTool>(endpoint),
                    .owner = Mcp::McpOwnerContext::Editor,
                    .inspectDraft = &InspectSceneProposal};
        }

        /** @brief Publishes a read-only query under the shared inspection capability and bounds. */
        template <auto Read>
        Mcp::McpToolRegistration Registration(const std::weak_ptr<const EditorInspectionCommandEndpoint> &workspace, std::string id,
                                              std::string description, Json input, Json schema) {
            constexpr bool largePage = std::is_same_v<decltype(Read), decltype(&EditorInspectionCommandEndpoint::readTransforms)> ||
                                       std::is_same_v<decltype(Read), decltype(&EditorInspectionCommandEndpoint::readBounds)>;
            return {.descriptor = {.id = {std::move(id)},
                                   .description = std::move(description),
                                   .inputSchema = std::move(input),
                                   .outputSchema = std::move(schema),
                                   .effect = Mcp::McpToolEffect::Query,
                                   .requiredCapabilities = {std::string{EditorSceneInspectionGrant}},
                                   .bounds = {.maximumInputBytes = 1024,
                                              .maximumResultBytes = largePage ? 16384U : 8192U,
                                              .maximumDepth = 6,
                                              .maximumNodes = largePage ? 1024U : 512U}},
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

    /** @copydoc StageEditorScenePlan */
    Result<std::uint64_t> StageEditorScenePlan(const EditorInspectionCommandEndpoint &endpoint, const std::string_view tool,
                                               const nlohmann::json &arguments, std::function<bool()> authorized) {
        using Output = Result<std::uint64_t>;
        if (endpoint.owner != std::this_thread::get_id() || !endpoint.proposeEdits)
            return Output::Failure(MakeError(Mcp::McpErrors::OwnerUnavailable));
        if ((tool != SceneEditsTool && tool != SceneTransformsTool) || !authorized || !authorized())
            return Output::Failure(MakeError(Mcp::McpErrors::AuthorizationDenied));
        return StageSceneProposal(endpoint, arguments, std::move(authorized));
    }

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
                                                          "Read axis-aligned and oriented world-space bounds for visible Scene objects.",
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
        AppendInspection<&EditorInspectionCommandEndpoint::
                             readSurfaces>(tools, endpoint, "inspect.surfaces",
                                           "Sample the nearest committed mesh surface per visible object along a world-space ray.",
                                           SurfaceSchema, SurfaceInputSchema);
        AppendInspection<&EditorInspectionCommandEndpoint::
                             readCollisions>(tools, endpoint, "inspect.collisions",
                                             "Page through authored collider shapes and body-local placement for one visible object.",
                                             CollisionSchema, CollisionInputSchema);
        AppendInspection<&EditorInspectionCommandEndpoint::
                             readPenetrations>(tools, endpoint, "inspect.penetrations",
                                               "Read the deepest published Physics overlap between two visible authored bodies.",
                                               PenetrationSchema, PenetrationInputSchema);
        AppendInspection<&EditorInspectionCommandEndpoint::
                             readVisualIssues>(tools, endpoint, "inspect.visual_issues",
                                               "Read projected bounds clipping and overlap within a normalized viewport region.",
                                               VisualSchema, VisualInputSchema);
        if (endpoint->proposeEdits) {
            tools.push_back(ProposalRegistration(endpoint, SceneProposalKind::Transforms));
            tools.push_back(ProposalRegistration(endpoint, SceneProposalKind::Edits));
        }
        AppendEditorViewportTools(endpoint, tools);
        if (tools.empty())
            return Output::Failure(MakeError(Mcp::McpErrors::ConfigurationInvalid));
        return Output::Success(std::move(tools));
    }
}  // namespace Horo::Editor
