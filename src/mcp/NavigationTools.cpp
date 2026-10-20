#include "Horo/Mcp/NavigationTools.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Mcp/McpErrors.h"

#include <algorithm>
#include <array>
#include <charconv>

namespace Horo::Mcp {
    namespace {
        namespace App = Application;
        namespace Nav = Navigation;
        enum class Action {
            Bake,
            Query,
            Cancel
        };
        constexpr std::array Names{"navigation.bake", "navigation.operation", "navigation.cancel"};

        /** @brief Reads canonical nonzero decimal identities without floating point or coercion. */
        Result<std::uint64_t> Identity(const nlohmann::json &value) {
            if (!value.is_string())
                return Result<std::uint64_t>::Failure(MakeError(McpErrors::ToolInputInvalid));
            const auto &text = value.get_ref<const std::string &>();
            std::uint64_t number{};
            if (const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
                text.empty() || text.size() > 20 || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || number == 0 ||
                std::to_string(number) != text)
                return Result<std::uint64_t>::Failure(MakeError(McpErrors::ToolInputInvalid));
            return Result<std::uint64_t>::Success(number);
        }

        /** @brief Preserves exact Scene-qualified selection; no matching-surface fallback is available. */
        Result<std::vector<Nav::NavigationBakeSurfaceKey>> Surfaces(const nlohmann::json &values) {
            std::vector<Nav::NavigationBakeSurfaceKey> result;
            if (!values.is_array() || values.empty() || values.size() > 4096)
                return Result<std::vector<Nav::NavigationBakeSurfaceKey>>::Failure(MakeError(McpErrors::ToolInputInvalid));
            for (const auto &item : values) {
                const auto sceneText = item.at("scene").get<std::string>();
                const auto scene = Assets::AssetId::Parse(sceneText);
                const auto surface = Identity(item.at("surface"));
                if (scene.HasError() || surface.HasError() || scene.Value().ToString() != sceneText)
                    return Result<std::vector<Nav::NavigationBakeSurfaceKey>>::Failure(MakeError(McpErrors::ToolInputInvalid));
                const auto scope = Nav::NavigationBakeScopeId::ForScene(scene.Value().Bytes());
                if (scope.HasError())
                    return Result<std::vector<Nav::NavigationBakeSurfaceKey>>::Failure(MakeError(McpErrors::ToolInputInvalid));
                result.emplace_back(Nav::SurfaceId::Create(surface.Value()).Value(), scope.Value());
            }
            std::ranges::sort(result);
            if (std::ranges::adjacent_find(result) != result.end())
                return Result<std::vector<Nav::NavigationBakeSurfaceKey>>::Failure(MakeError(McpErrors::ToolInputInvalid));
            return Result<std::vector<Nav::NavigationBakeSurfaceKey>>::Success(std::move(result));
        }

        /** @brief Describes bounded typed command arguments; operation controls cannot introduce project paths. */
        nlohmann::json InputSchema(Action action) {
            const nlohmann::json identity{{"type", "string"}, {"minLength", 1}, {"maxLength", 20}};
            nlohmann::json properties;
            nlohmann::json required = nlohmann::json::array();
            if (action == Action::Bake)
                properties = {{"validateOnly", {{"type", "boolean"}}},
                              {"surfaces",
                               {{"type", "array"},
                                {"minItems", 1},
                                {"maxItems", 4096},
                                {"items",
                                 {{"type", "object"},
                                  {"properties",
                                   {{"scene", {{"type", "string"}, {"minLength", 36}, {"maxLength", 36}}}, {"surface", identity}}},
                                  {"required", {"scene", "surface"}},
                                  {"additionalProperties", false}}}}}};
            else {
                properties = {{"operationId", identity}};
                required.push_back("operationId");
            }
            return {{"type", "object"},
                    {"properties", std::move(properties)},
                    {"required", std::move(required)},
                    {"additionalProperties", false}};
        }

        /** @brief Returns the same bounded fields for admission, observation and cancellation. */
        nlohmann::json OutputSchema() {
            const nlohmann::json text{{"type", "string"}, {"maxLength", 128}};
            const nlohmann::json count{{"type", "integer"}, {"minimum", 0}};
            return {{"type", "object"},
                    {"properties",
                     {{"operationId", text},
                      {"revision", text},
                      {"phase", text},
                      {"state", count},
                      {"completion", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                      {"validateOnly", {{"type", "boolean"}}},
                      {"published", {{"type", "boolean"}}},
                      {"surfaceCount", count},
                      {"tileCount", count},
                      {"rebuiltTiles", count},
                      {"reusedTiles", count},
                      {"inputFingerprint", text},
                      {"manifestDigest", text},
                      {"diagnostic", {{"type", "object"}, {"additionalProperties", true}}}}},
                    {"required",
                     {"operationId", "revision", "phase", "state", "completion", "validateOnly", "published", "surfaceCount", "tileCount",
                      "rebuiltTiles", "reusedTiles", "inputFingerprint", "manifestDigest", "diagnostic"}},
                    {"additionalProperties", false}};
        }

        /** @brief Encodes actual operation evidence; validation never borrows a prior generation's receipt. */
        nlohmann::json Encode(const App::NavigationBakeCommandSnapshot &value) {
            return {{"operationId", std::to_string(value.id)},
                    {"revision", std::to_string(value.revision)},
                    {"phase", value.phase},
                    {"state", static_cast<unsigned>(value.state)},
                    {"completion", value.progress.value_or(0)},
                    {"validateOnly", value.execution == Nav::NavigationBakeExecutionMode::ValidateOnly},
                    {"published", value.publication != nullptr},
                    {"surfaceCount", value.surfaceCount},
                    {"tileCount", value.tileCount},
                    {"rebuiltTiles", value.publication ? value.publication->rebuiltTiles : 0},
                    {"reusedTiles", value.publication ? value.publication->reusedTiles : 0},
                    {"inputFingerprint", value.inputFingerprint ? FormatSha256(*value.inputFingerprint) : ""},
                    {"manifestDigest", value.publication ? FormatSha256(value.publication->generation.manifestDigest) : ""},
                    {"diagnostic", value.diagnostic ? SafeErrorData(*value.diagnostic) : nlohmann::json::object()}};
        }

        class Tool final : public IMcpToolAdapter {
        public:
            Tool(std::shared_ptr<App::NavigationBakeApplication> application, std::string project, std::uint64_t generation,
                 std::function<std::uint64_t()> current, Action action)
                : application_(std::move(application)), project_(std::move(project)), generation_(generation), current_(std::move(current)),
                  action_(action) {}

            Result<nlohmann::json> Invoke(const nlohmann::json &arguments, const McpRequestContext &context) override {
                if (!context.projectIdentity || *context.projectIdentity != project_ || current_() != generation_)
                    return Result<nlohmann::json>::Failure(MakeError(McpErrors::SessionUnavailable));
                if (context.IsStopRequested())
                    return Result<nlohmann::json>::Failure(MakeError(McpErrors::RequestCancelled));
                auto operation = Dispatch(arguments, context);
                if (operation.HasError())
                    return Result<nlohmann::json>::Failure(operation.ErrorValue());
                auto snapshot = application_->Query(operation.Value());
                if (snapshot.HasError())
                    return Result<nlohmann::json>::Failure(snapshot.ErrorValue());
                return Result<nlohmann::json>::Success(Encode(snapshot.Value()));
            }

        private:
            /** @brief Dispatches only typed application operations after normal MCP registry authorization. */
            Result<std::uint64_t> Dispatch(const nlohmann::json &arguments, const McpRequestContext &context) const {
                if (action_ != Action::Bake) {
                    auto id = Identity(arguments.at("operationId"));
                    if (id.HasValue() && action_ == Action::Cancel) {
                        auto cancelled = application_->RequestCancel(id.Value());
                        if (cancelled.HasError())
                            return Result<std::uint64_t>::Failure(cancelled.ErrorValue());
                    }
                    return id;
                }
                auto retained = context;
                retained.reportProgress = {};
                App::NavigationBakeCommandRequest request{.invocation = {.cancellation = context.cancellation,
                                                                         .deadline = context.deadline,
                                                                         .authorityStopped = [retained = std::move(retained)] {
                    return retained.IsStopRequested();
                }}};
                if (arguments.value("validateOnly", false))
                    request.execution = Nav::NavigationBakeExecutionMode::ValidateOnly;
                if (arguments.contains("surfaces")) {
                    auto surfaces = Surfaces(arguments.at("surfaces"));
                    if (surfaces.HasError())
                        return Result<std::uint64_t>::Failure(surfaces.ErrorValue());
                    request.surfaces = std::move(surfaces).Value();
                }
                return application_->Start(std::move(request));
            }

            std::shared_ptr<App::NavigationBakeApplication> application_;
            std::string project_;
            std::uint64_t generation_;
            std::function<std::uint64_t()> current_;
            Action action_;
        };
    }  // namespace

    /** @copydoc MakeNavigationTools */
    Result<std::vector<McpToolRegistration>> MakeNavigationTools(std::shared_ptr<App::NavigationBakeApplication> application,
                                                                 std::string projectIdentity, std::uint64_t projectGeneration,
                                                                 std::function<std::uint64_t()> currentGeneration, McpOwnerContext owner) {
        if (!application || projectIdentity.empty() || projectIdentity.size() > 256 || !IsValidUtf8ScalarSequence(projectIdentity) ||
            projectGeneration == 0 || !currentGeneration || (owner != McpOwnerContext::Runtime && owner != McpOwnerContext::Editor))
            return Result<std::vector<McpToolRegistration>>::Failure(MakeError(McpErrors::ConfigurationInvalid));
        std::vector<McpToolRegistration> result;
        for (std::size_t index = 0; index < Names.size(); ++index) {
            const auto action = static_cast<Action>(index);
            result.push_back({.descriptor = {.id = {Names[index]},
                                             .description = "Request or observe the shared project navigation bake operation.",
                                             .inputSchema = InputSchema(action),
                                             .outputSchema = OutputSchema(),
                                             .effect = action == Action::Query ? McpToolEffect::Query : McpToolEffect::Mutation,
                                             .requiredCapabilities = {"horo.navigation.bake"},
                                             .bounds = {.maximumResultBytes = 64U << 10U, .maximumNodes = 4096}},
                              .adapter = std::make_shared<Tool>(application, projectIdentity, projectGeneration, currentGeneration, action),
                              .owner = owner});
        }
        return Result<std::vector<McpToolRegistration>>::Success(std::move(result));
    }
}  // namespace Horo::Mcp
