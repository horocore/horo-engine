#include "EditorInspectionMcpTools.h"
#include "Horo/Mcp/McpAuthorization.h"
#include "Horo/Mcp/McpErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <thread>
#include <utility>

namespace Horo::Editor {
    namespace {
        using Json = nlohmann::json;
        using JsonResult = Result<Json>;

        /** @brief One bounded world-space stroke. It never discovers additional targets. */
        struct ViewportBrush final {
            static constexpr double MaximumRadiusMeters = 100;
            static constexpr double MaximumStrokeLengthMeters = 256;
            static constexpr std::size_t MaximumPoints = 16;
            std::vector<std::array<float, 3>> pointsMeters;
            double radiusMeters{};

            /** @brief Bound the full swept area as well as each segment's radius. */
            [[nodiscard]] bool HasBoundedLength() const noexcept {
                double length{};
                for (std::size_t index = 1; index < pointsMeters.size(); ++index) {
                    const auto &a = pointsMeters[index - 1];
                    const auto &b = pointsMeters[index];
                    length +=
                        std::hypot(static_cast<double>(b[0]) - a[0], static_cast<double>(b[1]) - a[1], static_cast<double>(b[2]) - a[2]);
                }
                return length <= MaximumStrokeLengthMeters;
            }

            /** @brief Check the committed pivot against the swept sphere, including its boundary. */
            [[nodiscard]] bool Contains(const std::array<float, 3> &pivot) const noexcept {
                for (std::size_t index = 0; index < pointsMeters.size(); ++index) {
                    const auto previous = index == 0 ? index : index - 1;
                    if (Distance(pivot, pointsMeters[previous], pointsMeters[index]) <= radiusMeters)
                        return true;
                }
                return false;
            }

        private:
            /** @brief Distance to a closed stroke segment, with repeated points treated as one dab. */
            static double Distance(const std::array<float, 3> &pivot, const std::array<float, 3> &start,
                                   const std::array<float, 3> &end) noexcept {
                std::array<double, 3> offset;
                std::array<double, 3> direction;
                double projection{};
                double lengthSquared{};
                for (std::size_t axis = 0; axis < offset.size(); ++axis) {
                    offset[axis] = static_cast<double>(pivot[axis]) - start[axis];
                    direction[axis] = static_cast<double>(end[axis]) - start[axis];
                    projection += offset[axis] * direction[axis];
                    lengthSquared += direction[axis] * direction[axis];
                }
                const double fraction = lengthSquared > 0 ? std::clamp(projection / lengthSquared, 0.0, 1.0) : 0.0;
                return std::hypot(offset[0] - fraction * direction[0], offset[1] - fraction * direction[1],
                                  offset[2] - fraction * direction[2]);
            }
        };

        /** @brief Typed input already bounded by the registry's exact JSON schema. */
        struct LightRequest final {
            ObjectInspectionRequest inspection;
            std::optional<ViewportBrush> brush;
            bool enabled{};
        };

        /** @brief A light tool requires explicit targets and supports an additional spatial constraint. */
        Json LightInputSchema() {
            const Json objectId{{"type", "integer"}, {"minimum", 1}};
            const Json scene{{"type", "object"},
                             {"properties", {{"epoch", objectId}, {"revision", {{"type", "integer"}, {"minimum", 0}}}}},
                             {"required", {"epoch", "revision"}},
                             {"additionalProperties", false}};
            constexpr auto CoordinateLimit = static_cast<double>(std::numeric_limits<float>::max());
            const Json point{{"type", "array"},
                             {"minItems", 3},
                             {"maxItems", 3},
                             {"items", {{"type", "number"}, {"minimum", -CoordinateLimit}, {"maximum", CoordinateLimit}}}};
            const Json points{{"type", "array"}, {"minItems", 1}, {"maxItems", ViewportBrush::MaximumPoints}, {"items", point}};
            const Json radius{{"type", "number"},
                              {"minimum", std::numeric_limits<double>::denorm_min()},
                              {"maximum", ViewportBrush::MaximumRadiusMeters}};
            const Json brush{{"type", "object"},
                             {"properties", {{"pointsMeters", points}, {"radiusMeters", radius}}},
                             {"required", {"pointsMeters", "radiusMeters"}},
                             {"additionalProperties", false}};
            const Json objects{{"type", "array"},
                               {"minItems", 1},
                               {"maxItems", ObjectInspectionRequest::MaximumObjects},
                               {"items", objectId}};
            return {{"type", "object"},
                    {"properties", {{"scene", scene}, {"objects", objects}, {"enabled", {{"type", "boolean"}}}, {"brush", brush}}},
                    {"required", {"scene", "objects", "enabled"}},
                    {"additionalProperties", false}};
        }

        /** @brief Uses the same complete, revision-bound read path for explicit and brush-scoped objects. */
        LightRequest ReadLightRequest(const Json &arguments) {
            LightRequest request;
            request.inspection.objects = arguments.at("objects").get<std::vector<std::uint64_t>>();
            const auto &scene = arguments.at("scene");
            request.inspection.expected =
                InspectionSceneRevision{scene.at("epoch").get<std::uint64_t>(), scene.at("revision").get<std::uint64_t>()};
            request.enabled = arguments.at("enabled").get<bool>();
            if (const auto brush = arguments.find("brush"); brush != arguments.end())
                request.brush = ViewportBrush{brush->at("pointsMeters").get<std::vector<std::array<float, 3>>>(),
                                              brush->at("radiusMeters").get<double>()};
            return request;
        }

        /** @brief Verify exact membership for the whole stroke before creating any review. */
        Result<std::vector<AgentSceneLightUpdate>> PrepareLightUpdates(const LightRequest &request,
                                                                       const TransformInspectionPage &observed) {
            using Updates = Result<std::vector<AgentSceneLightUpdate>>;
            const auto &objects = request.inspection.objects;
            if (observed.scene != request.inspection.expected || observed.nextOffset || observed.records.size() != objects.size())
                return Updates::Failure(MakeError(Mcp::McpErrors::ToolInputInvalid));
            std::vector<AgentSceneLightUpdate> updates;
            updates.reserve(objects.size());
            for (const auto &object : observed.records) {
                if (object.local.object != objects[updates.size()] || (request.brush && !request.brush->Contains(object.worldPivot)))
                    return Updates::Failure(MakeError(Mcp::McpErrors::ToolInputInvalid));
                updates.push_back({object.local.object, request.enabled});
            }
            return Updates::Success(std::move(updates));
        }

        /** @brief Reference viewport tool. The existing Scene review owner performs any eventual edit. */
        class ViewportLightTool final : public Mcp::IMcpToolAdapter {
        public:
            explicit ViewportLightTool(std::weak_ptr<const EditorInspectionCommandEndpoint> endpoint) : endpoint_(std::move(endpoint)) {}

            JsonResult Invoke(const Json &arguments, const Mcp::McpRequestContext &context) override {
                const auto endpoint = endpoint_.lock();
                if (!endpoint || endpoint->owner != std::this_thread::get_id())
                    return JsonResult::Failure(MakeError(Mcp::McpErrors::OwnerUnavailable));
                if (!context.authority || context.projectIdentity != endpoint->project ||
                    context.authorizationRevision != endpoint->authorizationRevision)
                    return JsonResult::Failure(MakeError(Mcp::McpErrors::AuthorizationDenied));
                if (context.IsStopRequested())
                    return JsonResult::Failure(MakeError(Mcp::McpErrors::RequestCancelled));
                const auto request = ReadLightRequest(arguments);
                if (request.brush && !request.brush->HasBoundedLength())
                    return JsonResult::Failure(MakeError(Mcp::McpErrors::ToolInputInvalid));
                const auto observed = endpoint->readTransforms(request.inspection);
                if (observed.HasError())
                    return JsonResult::Failure(observed.ErrorValue());
                const auto updates = PrepareLightUpdates(request, observed.Value());
                if (updates.HasError())
                    return JsonResult::Failure(updates.ErrorValue());
                if (context.IsStopRequested())
                    return JsonResult::Failure(MakeError(Mcp::McpErrors::RequestCancelled));
                auto authorized = [owner = endpoint_, principal = std::weak_ptr<const Mcp::McpAuthority>{context.authority},
                                   cancellation = context.cancellation] {
                    const auto current = principal.lock();
                    return !cancellation.IsCancellationRequested() && !owner.expired() && current && !current->IsStopRequested();
                };
                const auto review = endpoint->proposeEdits(observed.Value().scene, {}, {}, updates.Value(), std::move(authorized));
                if (review.HasError())
                    return JsonResult::Failure(review.ErrorValue());
                return JsonResult::Success({{"reviewId", std::to_string(review.Value())}});
            }

        private:
            std::weak_ptr<const EditorInspectionCommandEndpoint> endpoint_;
        };
    }  // namespace

    /** @copydoc AppendEditorViewportTools */
    void AppendEditorViewportTools(const std::shared_ptr<const EditorInspectionCommandEndpoint> &endpoint,
                                   std::vector<Mcp::McpToolRegistration> &tools) {
        if (!endpoint->readTransforms || !endpoint->proposeEdits)
            return;
        tools.push_back(
            {.descriptor = {.id = {"viewport.propose_light_state"},
                            .description =
                                "Review light state changes for explicit visible Scene objects, optionally inside a world-space brush.",
                            .inputSchema = LightInputSchema(),
                            .outputSchema = {{"type", "object"},
                                             {"properties", {{"reviewId", {{"type", "string"}, {"minLength", 1}, {"maxLength", 20}}}}},
                                             {"required", {"reviewId"}},
                                             {"additionalProperties", false}},
                            .effect = Mcp::McpToolEffect::PresentationSideEffect,
                            .requiredCapabilities = {std::string{EditorSceneInspectionGrant}, std::string{EditorSceneProposalGrant}},
                            .bounds = {.maximumInputBytes = 2048, .maximumResultBytes = 128, .maximumDepth = 6, .maximumNodes = 112}},
             .adapter = std::make_shared<ViewportLightTool>(endpoint),
             .owner = Mcp::McpOwnerContext::Editor});
    }
}  // namespace Horo::Editor
