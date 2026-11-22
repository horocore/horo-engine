#include "../mcp/McpAuthorizationTestSupport.h"
#include "EditorInspectionMcpTools.h"
#include "Horo/Mcp/McpController.h"
#include "Horo/Mcp/McpInProcessAdapter.h"
#include "Horo/Mcp/McpLocalTransport.h"

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Mcp;
    using Json = nlohmann::json;
    constexpr InspectionSceneRevision Scene{7, 19};

    template <typename Page, typename Row> Result<Page> ReadFixture(const ObjectInspectionRequest &request, Row body) {
        using Output = Result<Page>;
        if (request.expected && *request.expected != Scene)
            return Output::Failure(MakeError(ErrorCodeDescriptor{.domain = ErrorDomainId{"editor.inspection"},
                                                                 .code = ErrorCode{"scene_stale"},
                                                                 .defaultSeverity = ErrorSeverity::Warning}));
        if (request.objects.front() == 9)
            return Output::Failure(MakeError(ErrorCodeDescriptor{.domain = ErrorDomainId{"editor.inspection"},
                                                                 .code = ErrorCode{"object_unavailable"},
                                                                 .defaultSeverity = ErrorSeverity::Warning}));
        if constexpr (requires { body.local.object; })
            body.local.object = request.objects.front();
        else
            body.object = request.objects.front();
        return Output::Success({.scene = Scene, .records = {std::move(body)}});
    }

    /** @brief Gives each registered query a schema-valid record with the same exact Scene fence. */
    EditorInspectionCommandEndpoint FixtureEndpoint() {
        return {.project = "project-one",
                .authorizationRevision = 3,
                .owner = std::this_thread::get_id(),
                .readTransforms =
                    [](const ObjectInspectionRequest &request) {
            InspectedTransform body;
            body.local.translation = {1, 2, 3};
            return ReadFixture<TransformInspectionPage>(request, body);
        },
                .readBounds =
                    [](const ObjectInspectionRequest &request) {
            return ReadFixture<BoundsInspectionPage>(request, InspectedBounds{.minimum = {-1, 0, 2},
                                                                              .maximum = {1, 4, 6},
                                                                              .oriented = InspectedOrientedBounds{.center = {0, 2, 4},
                                                                                                                  .axes = {{{1, 0, 0},
                                                                                                                            {0, 1, 0},
                                                                                                                            {0, 0, 1}}},
                                                                                                                  .halfExtents = {1, 2, 2},
                                                                                                                  .volume = 32}});
        },
                .readComponents =
                    [](const ObjectInspectionRequest &request) {
            return ReadFixture<ComponentInspectionPage>(request, InspectedComponents{});
        },
                .readGeometry =
                    [](const ObjectInspectionRequest &request) {
            return ReadFixture<GeometryInspectionPage>(request, InspectedGeometry{.instances = 1, .vertices = 3, .triangles = 1});
        },
                .readDistances =
                    [](const ObjectInspectionRequest &request) {
            return ReadFixture<DistanceInspectionPage>(request, InspectedDistance{.referenceObject = *request.referenceObject, .gap = 2});
        },
                .readGridOffsets =
                    [](const ObjectInspectionRequest &request) {
            return ReadFixture<GridInspectionPage>(request, InspectedGridOffset{.gridStep = *request.gridStep});
        },
                .readSurfaces =
                    [](const ObjectInspectionRequest &request) {
            return ReadFixture<SurfaceInspectionPage>(request, InspectedSurface{.sample = InspectionSurfaceSample{.distance = 2}});
        },
                .readCollisions =
                    [](const ObjectInspectionRequest &request) {
            return ReadFixture<CollisionInspectionPage>(request, InspectedCollider{.slot = 1,
                                                                                   .bodyObject = 8,
                                                                                   .bodySlot = 2,
                                                                                   .shape = InspectedColliderSphere{0.5F}});
        },
                .readPenetrations =
                    [](const ObjectInspectionRequest &request) {
            return ReadFixture<PenetrationInspectionPage>(request, InspectedPenetration{.referenceObject = request.objects[1],
                                                                                        .sample = InspectionPenetrationSample{.depthMeters =
                                                                                                                                  0.25F}});
        },
                .readVisualIssues = [](const ObjectInspectionRequest &request) {
            return ReadFixture<VisualInspectionPage>(request,
                                                     InspectedVisualIssues{.regionBounds = request.viewportRegion->rect, .clipped = true});
        }};
    }

    class InspectionAdapterPair {
    public:
        explicit InspectionAdapterPair(const std::shared_ptr<const EditorInspectionCommandEndpoint> &endpoint) {
            const auto policy = Test::Authorization();
            auto registry = std::make_shared<McpToolRegistry>(policy);
            const std::vector<std::string> grants{std::string{EditorSceneInspectionGrant}};
            auto registrations = MakeEditorInspectionMcpTools(endpoint);
            REQUIRE(registrations.HasValue());
            toolCount = registrations.Value().size();
            for (const auto &registered : registrations.Value())
                CHECK(registered.descriptor.effect == McpToolEffect::Query);
            const auto revision = registry->Publish(std::move(registrations).Value(), grants);
            REQUIRE(revision.HasValue());
            auto opened = McpController::Create(registry, {}, policy);
            REQUIRE(opened.HasValue());
            controller = std::move(opened).Value();
            REQUIRE(controller->BindOwner(McpOwnerContext::Editor).HasValue());
            const auto manager = McpSessionManager::Create(controller, {}, policy);
            REQUIRE(manager.HasValue());
            const auto admission = Test::Authenticate({.clientIdentity = "inspection-parity",
                                                       .capabilities = grants,
                                                       .projectIdentity = "project-one",
                                                       .authorizationRevision = 3,
                                                       .registryRevision = revision.Value()},
                                                      policy);
            auto inProcess = McpInProcessAdapter::Start(manager.Value(), admission);
            auto framed = McpLocalTransport::Start(manager.Value(), admission);
            REQUIRE(inProcess.HasValue());
            REQUIRE(framed.HasValue());
            embedded = std::move(inProcess).Value();
            local = std::move(framed).Value();
        }

        Json Compare(const std::string_view name, const Json &arguments) const {
            const McpRequest request{.id = "query", .method = "tools/call", .params = {{"name", name}, {"arguments", arguments}}};
            const auto first = embedded->Call(request);
            REQUIRE(first.HasValue());
            const auto peer = Framed(request);
            REQUIRE(controller->Pump(McpOwnerContext::Editor).Value() == 2);
            const auto get = [](const Json &accepted) {
                return McpRequest{.id = "result", .method = "operations/get", .params = {{"operationId", accepted.at("operationId")}}};
            };
            const auto settled = embedded->Call(get(first.Value()));
            REQUIRE(settled.HasValue());
            const auto other = Framed(get(peer));
            CHECK(settled.Value().at("status") == other.at("status"));
            CHECK(settled.Value().at("error") == other.at("error"));
            CHECK(settled.Value().at("result") == other.at("result"));
            return other;
        }

        std::size_t toolCount{};

    private:
        Json Framed(const McpRequest &request) const {
            const auto response = local->Feed(
                Json{{"jsonrpc", "2.0"}, {"id", request.id}, {"method", request.method}, {"params", request.params}}.dump() + "\n");
            REQUIRE(response.HasValue());
            REQUIRE(response.Value().size() == 1);
            return Json::parse(response.Value().front()).at("result");
        }

        std::shared_ptr<McpController> controller;
        std::shared_ptr<McpInProcessAdapter> embedded;
        std::shared_ptr<McpLocalTransport> local;
    };

    TEST_CASE("Every numeric inspection query agrees through both authenticated MCP transports", "[unit][editor][mcp][inspection]") {
        const auto endpoint = std::make_shared<const EditorInspectionCommandEndpoint>(FixtureEndpoint());
        const InspectionAdapterPair adapters(endpoint);
        const Json rect{{"minimum", {0, 0}}, {"maximum", {1, 1}}, {"aspect", 1}};
        const Json ray{{"origin", {0, 3, 0}}, {"direction", {0, -1, 0}}, {"maximumDistance", 5}};
        const std::array<std::pair<std::string_view, Json>, 10> queries{{
            {"inspect.transforms", {{"objects", {42}}}},
            {"inspect.bounds", {{"objects", {42}}}},
            {"inspect.components", {{"objects", {42}}}},
            {"inspect.rendering", {{"objects", {42}}}},
            {"inspect.distances", {{"objects", {42}}, {"referenceObject", 8}}},
            {"inspect.grid_offsets", {{"objects", {42}}, {"gridStep", 0.5}}},
            {"inspect.surfaces", {{"objects", {42}}, {"ray", ray}}},
            {"inspect.collisions", {{"objects", {42}}}},
            {"inspect.penetrations", {{"objects", {42, 8}}, {"channel", "5d45c3ed-ffa0-447e-a44f-a700b729d2f1"}}},
            {"inspect.visual_issues", {{"objects", {42}}, {"viewportRegion", rect}}},
        }};
        REQUIRE(queries.size() == adapters.toolCount);
        for (const auto &[name, arguments] : queries) {
            INFO(name);
            const auto success = adapters.Compare(name, arguments);
            REQUIRE(success.at("status") == "succeeded");
            CHECK(success["result"]["records"][0]["object"] == 42);
            auto hidden = arguments;
            hidden["objects"][0] = 9;
            const auto rejected = adapters.Compare(name, hidden);
            CHECK(rejected.at("status") == "failed");
            CHECK(rejected["error"]["code"] == "object_unavailable");
            auto stale = arguments;
            stale["scene"] = {{"epoch", Scene.epoch}, {"revision", Scene.revision - 1}};
            const auto outdated = adapters.Compare(name, stale);
            CHECK(outdated.at("status") == "failed");
            CHECK(outdated["error"]["code"] == "scene_stale");
        }
    }
}  // namespace
