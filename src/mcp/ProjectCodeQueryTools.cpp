#include "Horo/Mcp/ProjectCodeQueryTools.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Mcp/McpErrors.h"

#include <array>
#include <new>

namespace Horo::Mcp {
    namespace {
        using Application::CodeQueryKind;
        constexpr std::array Names{"project.files",       "project.text",         "project.search",     "project.symbols",
                                   "project.diagnostics", "project.build_status", "project.test_status"};

        /** @brief Closed schemas admit only bounded UTF-8 portable names and numeric continuation offsets. */
        nlohmann::json InputSchema(const CodeQueryKind kind) {
            using enum CodeQueryKind;
            nlohmann::json properties{{"path", {{"type", "string"}, {"maxLength", 1024}}},
                                      {"offset", {{"type", "integer"}, {"minimum", 0}, {"maximum", 1U << 20U}}},
                                      {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", kind == Text ? 4096 : 128}}},
                                      {"revision", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}}};
            nlohmann::json required = nlohmann::json::array();
            if (kind == Text || kind == Search || kind == Symbols)
                required.push_back("path");
            if (kind == Search) {
                properties["pattern"] = {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
                required.push_back("pattern");
            }
            return {{"type", "object"},
                    {"properties", std::move(properties)},
                    {"required", std::move(required)},
                    {"additionalProperties", false}};
        }

        /** @brief Every query shares an explicit bounded result envelope, including revision and continuation availability. */
        nlohmann::json OutputSchema() {
            const nlohmann::json position{{"type", "integer"}, {"minimum", 0}};
            const nlohmann::json row{{"type", "object"},
                                     {"properties",
                                      {{"kind", {{"type", "integer"}, {"minimum", 0}, {"maximum", 5}}},
                                       {"path", {{"type", "string"}, {"maxLength", 1024}}},
                                       {"label", {{"type", "string"}, {"maxLength", 4096}}},
                                       {"line", position},
                                       {"column", position},
                                       {"byteOffset", position},
                                       {"byteCount", position}}},
                                     {"required", {"kind", "path", "label", "line", "column", "byteOffset", "byteCount"}},
                                     {"additionalProperties", false}};
            return {{"type", "object"},
                    {"properties",
                     {{"revision", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
                      {"projectGeneration", {{"type", "integer"}, {"minimum", 1}}},
                      {"records", {{"type", "array"}, {"items", row}, {"maxItems", 128}}},
                      {"text", {{"type", "string"}, {"maxLength", 4096}}},
                      {"hasMore", {{"type", "boolean"}}},
                      {"nextOffset", position},
                      {"droppedRecords", position}}},
                    {"required", {"revision", "projectGeneration", "records", "text", "hasMore", "nextOffset", "droppedRecords"}},
                    {"additionalProperties", false}};
        }

        /** @brief Protocol-only conversion of already typed owned application values. */
        nlohmann::json Encode(const Application::CodeQueryPage &page) {
            nlohmann::json records = nlohmann::json::array();
            for (const auto &row : page.records)
                records.push_back({{"kind", static_cast<unsigned>(row.kind)},
                                   {"path", row.path},
                                   {"label", row.label},
                                   {"line", row.line},
                                   {"column", row.column},
                                   {"byteOffset", row.byteOffset},
                                   {"byteCount", row.byteCount}});
            return {{"revision", page.revision},
                    {"projectGeneration", page.projectGeneration},
                    {"records", std::move(records)},
                    {"text", page.text},
                    {"hasMore", page.nextOffset.has_value()},
                    {"nextOffset", page.nextOffset.value_or(0)},
                    {"droppedRecords", page.droppedRecords}};
        }

        /** @brief Retains the application lease; it has no editor, process, build-control or mutable store access. */
        class Tool final : public IMcpToolAdapter {
        public:
            Tool(std::shared_ptr<Application::ProjectCodeQuery> capability, std::string identity, const std::uint64_t generation,
                 const CodeQueryKind kind, std::function<std::uint64_t()> currentGeneration)
                : capability_(std::move(capability)), identity_(std::move(identity)), generation_(generation), kind_(kind),
                  currentGeneration_(std::move(currentGeneration)) {}

            Result<nlohmann::json> Invoke(const nlohmann::json &arguments, const McpRequestContext &context) override {
                if (!context.projectIdentity || *context.projectIdentity != identity_ || currentGeneration_() != generation_)
                    return Result<nlohmann::json>::Failure(MakeError(Application::CodeQueryErrors::Stale));
                if (context.IsStopRequested())
                    return Result<nlohmann::json>::Failure(MakeError(McpErrors::RequestCancelled));
                const Application::CodeQueryRequest request{kind_,
                                                            arguments.value("path", std::string{}),
                                                            arguments.value("pattern", std::string{}),
                                                            arguments.value("offset", std::size_t{}),
                                                            arguments.value("limit", std::size_t{64}),
                                                            arguments.contains("revision")
                                                                ? std::optional{arguments.at("revision").get<std::string>()}
                                                                : std::nullopt};
                auto result =
                    capability_->Query(request, {identity_, generation_, context.cancellation, context.deadline, [&context, this] {
                    return context.IsStopRequested() || currentGeneration_() != generation_;
                }});
                if (currentGeneration_() != generation_)
                    return Result<nlohmann::json>::Failure(MakeError(Application::CodeQueryErrors::Stale));
                if (result.HasError())
                    return Result<nlohmann::json>::Failure(result.ErrorValue());
                auto encoded = Encode(result.Value());
                if (encoded.dump().size() > (256U << 10U))
                    return Result<nlohmann::json>::Failure(MakeError(Application::CodeQueryErrors::Capacity));
                return Result<nlohmann::json>::Success(std::move(encoded));
            }

        private:
            std::shared_ptr<Application::ProjectCodeQuery> capability_;
            std::string identity_;
            std::uint64_t generation_;
            CodeQueryKind kind_;
            std::function<std::uint64_t()> currentGeneration_;
        };
    }  // namespace

    /** @copydoc MakeProjectCodeQueryTools */
    Result<std::vector<McpToolRegistration>> MakeProjectCodeQueryTools(std::shared_ptr<Application::ProjectCodeQuery> capability,
                                                                       std::string identity, const std::uint64_t generation,
                                                                       std::function<std::uint64_t()> currentGeneration,
                                                                       const McpOwnerContext owner) {
        if (!capability || identity.empty() || identity.size() > 256 || !IsValidUtf8ScalarSequence(identity) || generation == 0 ||
            !currentGeneration || (owner != McpOwnerContext::Background && owner != McpOwnerContext::Editor))
            return Result<std::vector<McpToolRegistration>>::Failure(MakeError(Application::CodeQueryErrors::Invalid));
        try {
            std::vector<McpToolRegistration> tools;
            tools.reserve(Names.size());
            for (std::size_t i = 0; i < Names.size(); ++i) {
                const auto kind = static_cast<CodeQueryKind>(i);
                tools.push_back(
                    {.descriptor = {.id = {Names[i]},
                                    .description = "Read bounded project code observations without document or build side effects.",
                                    .inputSchema = InputSchema(kind),
                                    .outputSchema = OutputSchema(),
                                    .effect = McpToolEffect::Query,
                                    .requiredCapabilities = {"horo.project.code.query"},
                                    .bounds = {.maximumResultBytes = 256U << 10U, .maximumNodes = 4096}},
                     .adapter = std::make_shared<Tool>(capability, identity, generation, kind, currentGeneration),
                     .owner = owner});
            }
            return Result<std::vector<McpToolRegistration>>::Success(std::move(tools));
        } catch (const std::bad_alloc &) {
            return Result<std::vector<McpToolRegistration>>::Failure(MakeError(Application::CodeQueryErrors::Capacity));
        }
    }
}  // namespace Horo::Mcp
