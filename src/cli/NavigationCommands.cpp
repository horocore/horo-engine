#include "Horo/Cli/NavigationCommands.h"

#include "Horo/Cli/CliErrors.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <thread>

namespace Horo::Cli {
    namespace {
        namespace App = Application;
        namespace Nav = Navigation;

        /** @brief Decodes one exact Scene UUID and nonzero surface identity; no path or unscoped fallback is accepted. */
        Result<Nav::NavigationBakeSurfaceKey> Surface(std::string_view text) {
            if (text.size() < 38 || text.size() > 57 || text[36] != ':')
                return Result<Nav::NavigationBakeSurfaceKey>::Failure(MakeError(CliErrors::ParseFailed));
            auto scene = Assets::AssetId::Parse(text.substr(0, 36));
            const auto number = text.substr(37);
            std::uint64_t value{};
            if (const auto parsed = std::from_chars(number.data(), number.data() + number.size(), value);
                scene.HasError() || parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || value == 0 ||
                std::to_string(value) != number || scene.Value().ToString() != text.substr(0, 36))
                return Result<Nav::NavigationBakeSurfaceKey>::Failure(MakeError(CliErrors::ParseFailed));
            const auto scope = Nav::NavigationBakeScopeId::ForScene(scene.Value().Bytes());
            if (scope.HasError())
                return Result<Nav::NavigationBakeSurfaceKey>::Failure(MakeError(CliErrors::ParseFailed));
            return Result<Nav::NavigationBakeSurfaceKey>::Success({Nav::SurfaceId::Create(value).Value(), scope.Value()});
        }

        /** @brief Validates the exact definition before the host acquires any project authority. */
        Result<void> ValidateDefinition(const CliParsedOption &option) {
            if (option.values.size() != 1)
                return Result<void>::Failure(MakeError(CliErrors::ParseFailed));
            const auto *text = std::get_if<std::string>(&option.values.front().value);
            if (!text)
                return Result<void>::Failure(MakeError(CliErrors::ParseFailed));
            if (const auto definition = Assets::AssetId::Parse(*text);
                definition.HasError() || !definition.Value().IsValid() || definition.Value().ToString() != *text)
                return Result<void>::Failure(MakeError(CliErrors::ParseFailed));
            return Result<void>::Success();
        }

        /** @brief Reads a bounded complete selection and rejects duplicate exact identities. */
        Result<std::vector<Nav::NavigationBakeSurfaceKey>> ReadSurfaces(const CliParsedOption &option) {
            using Return = Result<std::vector<Nav::NavigationBakeSurfaceKey>>;
            if (option.values.empty() || option.values.size() > 4096)
                return Return::Failure(MakeError(CliErrors::ParseFailed));
            std::vector<Nav::NavigationBakeSurfaceKey> surfaces;
            surfaces.reserve(option.values.size());
            for (const auto &value : option.values) {
                const auto *text = std::get_if<std::string>(&value.value);
                if (!text)
                    return Return::Failure(MakeError(CliErrors::ParseFailed));
                auto surface = Surface(*text);
                if (surface.HasError())
                    return Return::Failure(surface.ErrorValue());
                surfaces.push_back(surface.Value());
            }
            std::ranges::sort(surfaces);
            if (std::ranges::adjacent_find(surfaces) != surfaces.end())
                return Return::Failure(MakeError(CliErrors::ParseFailed));
            return Return::Success(std::move(surfaces));
        }

        /** @brief Translates parser-owned option values into the shared typed request before project acquisition. */
        Result<App::NavigationBakeCommandRequest> ReadRequest(const CliCommandRequest &request, const CliExecutionContext &context) {
            App::NavigationBakeCommandRequest result{.invocation = {.cancellation = context.Cancellation()}};
            if (const auto deadline = context.AbsoluteDeadline())
                result.invocation.deadline = *deadline;
            for (const auto &option : request.options) {
                if (option.name == "definition") {
                    if (auto valid = ValidateDefinition(option); valid.HasError())
                        return Result<App::NavigationBakeCommandRequest>::Failure(valid.ErrorValue());
                } else if (option.name == "validate-only") {
                    if (option.values.size() != 1 || !std::holds_alternative<bool>(option.values.front().value))
                        return Result<App::NavigationBakeCommandRequest>::Failure(MakeError(CliErrors::ParseFailed));
                    if (std::get<bool>(option.values.front().value))
                        result.execution = Nav::NavigationBakeExecutionMode::ValidateOnly;
                } else if (option.name == "surface") {
                    auto surfaces = ReadSurfaces(option);
                    if (surfaces.HasError())
                        return Result<App::NavigationBakeCommandRequest>::Failure(surfaces.ErrorValue());
                    result.surfaces = std::move(surfaces).Value();
                }
            }
            return Result<App::NavigationBakeCommandRequest>::Success(std::move(result));
        }

        /** @brief Emits stable machine manifest fields; a validation cannot claim an older published generation. */
        CliCommandResult Encode(const App::NavigationBakeCommandSnapshot &snapshot) {
            CliCommandResult result{
                .fields = {{"operationId", std::to_string(snapshot.id)},
                           {"revision", std::to_string(snapshot.revision)},
                           {"validated", snapshot.state == OperationState::Succeeded},
                           {"published", snapshot.publication != nullptr},
                           {"surfaceCount", static_cast<std::int64_t>(snapshot.surfaceCount)},
                           {"tileCount", static_cast<std::int64_t>(snapshot.tileCount)},
                           {"inputFingerprint", snapshot.inputFingerprint ? FormatSha256(*snapshot.inputFingerprint) : ""}}};
            if (snapshot.publication) {
                result.fields.emplace_back("manifestDigest", FormatSha256(snapshot.publication->generation.manifestDigest));
                result.fields.emplace_back("target", snapshot.publication->generation.target.Value());
                result.fields.emplace_back("rebuiltTiles", static_cast<std::int64_t>(snapshot.publication->rebuiltTiles));
                result.fields.emplace_back("reusedTiles", static_cast<std::int64_t>(snapshot.publication->reusedTiles));
            }
            return result;
        }

        class Adapter final : public ICliCommandAdapter {
        public:
            explicit Adapter(NavigationCommandApplication factory) : factory_(std::move(factory)) {}

            const CliCommandDescriptor &GetDescriptor() const noexcept override {
                return descriptor_;
            }

            Result<CliCommandResult> Execute(const CliCommandRequest &request, CliExecutionContext &context) override {
                auto input = ReadRequest(request, context);
                if (input.HasError())
                    return Result<CliCommandResult>::Failure(input.ErrorValue());
                if (!factory_)
                    return Result<CliCommandResult>::Failure(MakeError(CliErrors::CommandUnavailable));
                auto acquired = factory_(request, context);
                if (acquired.HasError())
                    return Result<CliCommandResult>::Failure(acquired.ErrorValue());
                auto application = std::move(acquired).Value();
                if (!application)
                    return Result<CliCommandResult>::Failure(MakeError(CliErrors::CommandUnavailable));
                const auto started = application->Start(std::move(input).Value());
                if (started.HasError())
                    return Result<CliCommandResult>::Failure(started.ErrorValue());
                return Observe(*application, started.Value(), context);
            }

        private:
            /** @brief Drains the accepted operation even when cancellation, deadline or progress presentation fails. */
            static Result<CliCommandResult> Observe(App::NavigationBakeApplication &application, std::uint64_t id,
                                                    CliExecutionContext &context) {
                auto presentation = context.BindOperation({id});
                std::uint64_t revision{};
                while (true) {
                    if (context.IsStopRequested() || presentation.HasError())
                        static_cast<void>(application.RequestCancel(id));
                    if (auto pumped = application.Pump(); pumped.HasError())
                        return Result<CliCommandResult>::Failure(pumped.ErrorValue());
                    const auto current = application.Query(id);
                    if (current.HasError())
                        return Result<CliCommandResult>::Failure(current.ErrorValue());
                    const auto &snapshot = current.Value();
                    if (snapshot.IsTerminal()) {
                        if (presentation.HasError())
                            return Result<CliCommandResult>::Failure(presentation.ErrorValue());
                        if (snapshot.diagnostic)
                            return Result<CliCommandResult>::Failure(*snapshot.diagnostic);
                        if (snapshot.state != OperationState::Succeeded)
                            return Result<CliCommandResult>::Failure(MakeError(CliErrors::HostFailure));
                        return Result<CliCommandResult>::Success(Encode(snapshot));
                    }
                    if (snapshot.revision != revision && presentation.HasValue()) {
                        presentation = context.ReportProgress({.phase = snapshot.phase, .completion = snapshot.progress.value_or(0)});
                        revision = snapshot.revision;
                    }
                    // One-shot tooling observation on the actual owner lane; never a runtime-frame wait.
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            }

            CliCommandDescriptor descriptor_{DescribeNavigationBakeCommand()};
            NavigationCommandApplication factory_;
        };
    }  // namespace

    /** @copydoc DescribeNavigationBakeCommand */
    CliCommandDescriptor DescribeNavigationBakeCommand() {
        return {.path = {{"navigation", "bake"}},
                .summary = "Bake or validate complete project navigation through the shared application service.",
                .options = {{.name = "definition",
                             .summary = "Exact project navigation definition UUID.",
                             .valueKind = CliOptionValueKind::String,
                             .required = true},
                            {.name = "surface",
                             .summary = "Select an exact scene-uuid:surface-id; repeat for multiple surfaces.",
                             .valueKind = CliOptionValueKind::String,
                             .repeatable = true},
                            {.name = "validate-only",
                             .summary = "Validate without building or writing output.",
                             .valueKind = CliOptionValueKind::Flag}},
                .requiredCapabilities = {{"horo.navigation.bake"}},
                .output = {.id = "horo.navigation.bake",
                           .version = 1,
                           .formats = CliOutputFormat::Human | CliOutputFormat::Json | CliOutputFormat::JsonLines,
                           .progressRecords = true},
                .interactive = CliInteractivePolicy::Forbidden,
                .hosts = CliHostAvailability::HoroEngine,
                .contractVersion = {1, 0, 0},
                .sideEffects = CliSideEffectPolicy::WritesFiles,
                .cancellation = CliCancellationPolicy::Cooperative,
                .ownerId = "horo.navigation.cli"};
    }

    /** @copydoc MakeNavigationBakeCommandAdapter */
    CliCommandAdapterRegistration MakeNavigationBakeCommandAdapter(NavigationCommandApplication application) {
        return {.adapter = std::make_unique<Adapter>(std::move(application)), .capabilities = {{"horo.navigation.bake"}}};
    }
}  // namespace Horo::Cli
