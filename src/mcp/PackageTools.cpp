#include "Horo/Mcp/PackageTools.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Mcp/McpErrors.h"

#include <array>
#include <charconv>

namespace Horo::Mcp {
    namespace {
        using Application::PackageOperationId;
        using Application::PackageOperationSnapshot;
        enum class Action {
            Restore,
            Install,
            Activate,
            Disable,
            Query,
            Cancel
        };
        constexpr std::array Names{"package.restore", "package.install",   "package.activate",
                                   "package.disable", "package.operation", "package.cancel"};

        nlohmann::json InputSchema(const Action action) {
            nlohmann::json properties = nlohmann::json::object();
            nlohmann::json required = nlohmann::json::array();
            if (action == Action::Query || action == Action::Cancel) {
                properties["operationId"] = {{"type", "string"}, {"minLength", 1}, {"maxLength", 20}};
                required.push_back("operationId");
            }
            if (action == Action::Activate) {
                properties["enabled"] = {{"type", "array"},
                                         {"maxItems", 64},
                                         {"items",
                                          {{"type", "object"},
                                           {"properties",
                                            {{"package", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
                                             {"descriptor", {{"type", "string"}, {"minLength", 1}, {"maxLength", 1024}}}}},
                                           {"required", {"package", "descriptor"}},
                                           {"additionalProperties", false}}}};
                required.push_back("enabled");
            }
            return {{"type", "object"},
                    {"properties", std::move(properties)},
                    {"required", std::move(required)},
                    {"additionalProperties", false}};
        }

        nlohmann::json OutputSchema() {
            const nlohmann::json integer{{"type", "integer"}, {"minimum", 0}};
            return {{"type", "object"},
                    {"properties",
                     {{"operationId", {{"type", "string"}, {"maxLength", 20}}},
                      {"revision", {{"type", "string"}, {"maxLength", 20}}},
                      {"installedRevision", {{"type", "string"}, {"maxLength", 20}}},
                      {"activeGeneration", {{"type", "string"}, {"maxLength", 20}}},
                      {"kind", integer},
                      {"phase", integer},
                      {"outcome", integer},
                      {"completion", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                      {"completedPackages", integer},
                      {"totalPackages", integer},
                      {"diagnostic", {{"type", "object"}, {"additionalProperties", true}}}}},
                    {"required",
                     {"operationId", "revision", "kind", "phase", "outcome", "completion", "completedPackages", "totalPackages",
                      "diagnostic"}},
                    {"additionalProperties", false}};
        }

        nlohmann::json Encode(const PackageOperationSnapshot &value) {
            return {{"operationId", std::to_string(value.id.value)},
                    {"revision", std::to_string(value.revision)},
                    {"installedRevision", value.installedRevision ? std::to_string(*value.installedRevision) : ""},
                    {"activeGeneration", value.activeGeneration ? std::to_string(*value.activeGeneration) : ""},
                    {"kind", static_cast<unsigned>(value.kind)},
                    {"phase", static_cast<unsigned>(value.phase)},
                    {"outcome", static_cast<unsigned>(value.outcome)},
                    {"completion", value.completion},
                    {"completedPackages", value.completedPackages},
                    {"totalPackages", value.totalPackages},
                    {"diagnostic", value.diagnostic ? SafeErrorData(*value.diagnostic) : nlohmann::json::object()}};
        }

        Result<PackageOperationId> ReadOperation(const nlohmann::json &arguments) {
            const auto value = arguments.at("operationId").get<std::string>();
            std::uint64_t number{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || number == 0 || std::to_string(number) != value)
                return Result<PackageOperationId>::Failure(MakeError(McpErrors::ToolInputInvalid));
            return Result<PackageOperationId>::Success({number});
        }

        Result<std::vector<Packages::EnabledPackageExtension>> ReadEnabled(const nlohmann::json &arguments) {
            std::vector<Packages::EnabledPackageExtension> result;
            result.reserve(arguments.at("enabled").size());
            for (const auto &item : arguments.at("enabled")) {
                auto package = Packages::HoroPackageId::Parse(item.at("package").get<std::string>());
                auto descriptor = Packages::PackagePath::Parse(item.at("descriptor").get<std::string>());
                if (package.HasError())
                    return Result<std::vector<Packages::EnabledPackageExtension>>::Failure(package.ErrorValue());
                if (descriptor.HasError())
                    return Result<std::vector<Packages::EnabledPackageExtension>>::Failure(descriptor.ErrorValue());
                result.push_back({std::move(package).Value(), std::move(descriptor).Value()});
            }
            return Result<std::vector<Packages::EnabledPackageExtension>>::Success(std::move(result));
        }

        class Tool final : public IMcpToolAdapter {
        public:
            Tool(std::shared_ptr<Application::PackageApplication> application, std::string project, const std::uint64_t generation,
                 std::function<std::uint64_t()> currentGeneration, const Action action)
                : application_(std::move(application)), project_(std::move(project)), generation_(generation),
                  currentGeneration_(std::move(currentGeneration)), action_(action) {}

            Result<nlohmann::json> Invoke(const nlohmann::json &arguments, const McpRequestContext &context) override {
                if (!context.projectIdentity || *context.projectIdentity != project_ || currentGeneration_() != generation_)
                    return Result<nlohmann::json>::Failure(MakeError(McpErrors::SessionUnavailable));
                if (context.IsStopRequested())
                    return Result<nlohmann::json>::Failure(MakeError(McpErrors::RequestCancelled));
                auto operation = Dispatch(arguments, context.cancellation);
                if (operation.HasError())
                    return Result<nlohmann::json>::Failure(operation.ErrorValue());
                // Observation may refresh nonblocking work, but a protocol caller cannot invent a quiescent boundary.
                if (const auto pumped = application_->Pump(Packages::PackageActivationBoundary::Running); pumped.HasError())
                    return Result<nlohmann::json>::Failure(pumped.ErrorValue());
                const auto snapshot = application_->Query(operation.Value());
                if (snapshot.HasError())
                    return Result<nlohmann::json>::Failure(snapshot.ErrorValue());
                return Result<nlohmann::json>::Success(Encode(snapshot.Value()));
            }

        private:
            Result<PackageOperationId> Dispatch(const nlohmann::json &arguments, const CancellationToken &cancellation) {
                switch (action_) {
                    case Action::Restore:
                        return application_->Restore(cancellation);
                    case Action::Install:
                        return application_->Install(cancellation);
                    case Action::Disable:
                        return application_->Disable(cancellation);
                    case Action::Activate: {
                        auto enabled = ReadEnabled(arguments);
                        if (enabled.HasError())
                            return Result<PackageOperationId>::Failure(enabled.ErrorValue());
                        return application_->Activate(enabled.Value(), cancellation);
                    }
                    case Action::Query:
                        return ReadOperation(arguments);
                    case Action::Cancel: {
                        auto operation = ReadOperation(arguments);
                        if (operation.HasError())
                            return operation;
                        auto cancelled = application_->RequestCancel(operation.Value());
                        if (cancelled.HasError())
                            return Result<PackageOperationId>::Failure(cancelled.ErrorValue());
                        return operation;
                    }
                }
                return Result<PackageOperationId>::Failure(MakeError(McpErrors::ToolUnavailable));
            }

            std::shared_ptr<Application::PackageApplication> application_;
            std::string project_;
            std::uint64_t generation_;
            std::function<std::uint64_t()> currentGeneration_;
            Action action_;
        };
    }  // namespace

    /** @copydoc MakePackageTools */
    Result<std::vector<McpToolRegistration>> MakePackageTools(std::shared_ptr<Application::PackageApplication> application,
                                                              std::string identity, const std::uint64_t generation,
                                                              std::function<std::uint64_t()> currentGeneration,
                                                              const McpOwnerContext owner) {
        if (!application || identity.empty() || identity.size() > 256 || !IsValidUtf8ScalarSequence(identity) || generation == 0 ||
            !currentGeneration || (owner != McpOwnerContext::Runtime && owner != McpOwnerContext::Editor))
            return Result<std::vector<McpToolRegistration>>::Failure(MakeError(McpErrors::ConfigurationInvalid));
        std::vector<McpToolRegistration> result;
        result.reserve(Names.size());
        for (std::size_t index = 0; index < Names.size(); ++index) {
            const auto action = static_cast<Action>(index);
            result.push_back({.descriptor = {.id = {Names[index]},
                                             .description = "Observe or request the shared project package application operation.",
                                             .inputSchema = InputSchema(action),
                                             .outputSchema = OutputSchema(),
                                             .effect = action == Action::Query ? McpToolEffect::Query : McpToolEffect::Mutation,
                                             .requiredCapabilities = {"horo.package.manage"},
                                             .bounds = {.maximumResultBytes = 64U << 10U, .maximumNodes = 4096}},
                              .adapter = std::make_shared<Tool>(application, identity, generation, currentGeneration, action),
                              .owner = owner});
        }
        return Result<std::vector<McpToolRegistration>>::Success(std::move(result));
    }
}  // namespace Horo::Mcp
