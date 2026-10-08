#include "CliHostCommands.h"

#include "Horo/Cli/CliErrors.h"
#include "Horo/Foundation/Telemetry/Telemetry.h"
#include "McpServe.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Uses a portable UTF-8 path spelling on every supported platform. */
        std::string Utf8Path(const std::filesystem::path &path) {
            const auto spelling = path.generic_u8string();
            return {spelling.begin(), spelling.end()};
        }
        /** @brief Existing production operation selected by accepted inert metadata. */
        enum class HostCommand {
            Inspect,
            Smoke,
            Bundle,
            Help,
            McpServe,
            Unknown
        };

        /** @brief Selects the command without exposing business dispatch to protocol output. */
        HostCommand SelectCommand(const Cli::CommandPath &path) {
            using enum HostCommand;
            if (path.segments == std::vector<std::string>{"mcp", "serve"})
                return McpServe;
            if (path.segments == std::vector<std::string>{"observability", "smoke"})
                return Smoke;
            if (path.segments == std::vector<std::string>{"diagnostics", "bundle"})
                return Bundle;
            if (path.segments == std::vector<std::string>{"host", "help"})
                return Help;
            if (path.segments == std::vector<std::string>{"host", "inspect"})
                return Inspect;
            return Unknown;
        }

        /** @brief Names the distinct versioned result shape owned by each production command. */
        std::string OutputSchemaId(const HostCommand command) {
            using enum HostCommand;
            switch (command) {
                case Inspect:
                    return "horo.cli.host-inspection";
                case Smoke:
                    return "horo.cli.observability-smoke";
                case Bundle:
                    return "horo.cli.diagnostic-bundle";
                case Help:
                    return "horo.cli.help";
                case McpServe:
                    return "horo.cli.mcp-service";
                case Unknown:
                    return "horo.cli.invocation";
            }
            return "horo.cli.invocation";
        }

        /** @brief Adapter over shared host observability operations; never writes protocol streams. */
        class HostCommandAdapter final : public Cli::ICliCommandAdapter {
        public:
            HostCommandAdapter(Cli::CliCommandDescriptor descriptor, std::shared_ptr<HostObservabilitySession> session, std::string help)
                : descriptor_(std::move(descriptor)), session_(std::move(session)), help_(std::move(help)),
                  command_(SelectCommand(descriptor_.path)) {}

            const Cli::CliCommandDescriptor &GetDescriptor() const noexcept override {
                return descriptor_;
            }

            Result<Cli::CliCommandResult> Execute(const Cli::CliCommandRequest &request, Cli::CliExecutionContext &context) override {
                if (context.IsStopRequested())
                    return Result<Cli::CliCommandResult>::Failure(MakeError(Cli::CliErrors::ExecutionCancelled));
                using enum HostCommand;
                switch (command_) {
                    case Help:
                        return Result<Cli::CliCommandResult>::Success({.fields = {{"help", help_}}});
                    case Inspect:
                        return Result<Cli::CliCommandResult>::Success({.fields = {{"initialized", true}}});
                    case Smoke:
                        return SmokeResult();
                    case Bundle:
                        return BundleResult(request, context);
                    case McpServe: {
                        if (const auto served = ServeNativeMcp(context, session_); served.HasError())
                            return Result<Cli::CliCommandResult>::Failure(served.ErrorValue());
                        return Result<Cli::CliCommandResult>::Success({});
                    }
                    case Unknown:
                        return Result<Cli::CliCommandResult>::Failure(MakeError(Cli::CliErrors::CommandUnavailable));
                }
                return Result<Cli::CliCommandResult>::Failure(MakeError(Cli::CliErrors::ExecutionContextInvalid));
            }

        private:
            /** @brief Calls the existing observability smoke operation without changing its metrics. */
            Result<Cli::CliCommandResult> SmokeResult() const {
                if (const auto emitted = EmitHostObservabilitySmoke(); emitted.HasError())
                    return Result<Cli::CliCommandResult>::Failure(emitted.ErrorValue());
                return Result<Cli::CliCommandResult>::Success({.fields = {{"completed", true}}});
            }

            /** @brief Calls the same diagnostic bundle service used by other application hosts. */
            Result<Cli::CliCommandResult> BundleResult(const Cli::CliCommandRequest &request, Cli::CliExecutionContext &context) const {
                const auto option = std::ranges::find(request.options, "output-path", &Cli::CliParsedOption::name);
                if (option == request.options.end() || option->values.empty())
                    return Result<Cli::CliCommandResult>::Failure(MakeError(Cli::CliErrors::ParseFailed));
                const auto *path = std::get_if<Cli::CliPathValue>(&option->values.front().value);
                if (path == nullptr)
                    return Result<Cli::CliCommandResult>::Failure(MakeError(Cli::CliErrors::ParseFailed));
                if (const auto progress =
                        context.ReportProgress({.phase = "bundle", .completion = 0.0F, .message = "Collecting diagnostics"});
                    progress.HasError())
                    return Result<Cli::CliCommandResult>::Failure(progress.ErrorValue());
                const auto result = session_->GenerateDiagnosticBundle(
                    {.outputPath = std::filesystem::path{std::u8string{path->value.begin(), path->value.end()}}});
                if (result.HasError())
                    return Result<Cli::CliCommandResult>::Failure(result.ErrorValue());
                return Result<Cli::CliCommandResult>::Success(
                    {.fields = {{"outputPath", Utf8Path(result.Value().outputPath)},
                                {"fileCount", static_cast<std::int64_t>(result.Value().fileCount)},
                                {"missingOptionalCount", static_cast<std::int64_t>(result.Value().missingOptionalCount)}}});
            }

            Cli::CliCommandDescriptor descriptor_;
            std::shared_ptr<HostObservabilitySession> session_;
            std::string help_;
            HostCommand command_;
        };
    }  // namespace

    /** @copydoc EmitHostObservabilitySmoke */
    Result<void> EmitHostObservabilitySmoke() {
        const auto counter = Telemetry::Runtime::RegisterCounter(
            {.name = "game.smoke.completed", .subsystem = "Game.Smoke", .unit = Telemetry::MetricUnit::Count});
        const auto gauge = Telemetry::Runtime::RegisterGauge(
            {.name = "plugin.example.active", .subsystem = "Plugin.example", .unit = Telemetry::MetricUnit::Count});
        counter.Add();
        gauge.Set(1.0);
        static_cast<void>(Telemetry::Runtime::EmitEvent("Game.Smoke", "game.smoke.completed", Log::Level::Info,
                                                        "Headless observability smoke completed"));
        return Result<void>::Success();
    }

    /** @copydoc DescribeHostCommands */
    std::vector<Cli::CliCommandDescriptor> DescribeHostCommands() {
        using namespace Cli;
        std::vector<CliCommandDescriptor> descriptors;
        for (const auto &path : std::vector<CommandPath>{{{"host", "inspect"}},
                                                         {{"host", "help"}},
                                                         {{"observability", "smoke"}},
                                                         {{"diagnostics", "bundle"}},
                                                         {{"mcp", "serve"}}}) {
            CliCommandDescriptor descriptor;
            descriptor.path = path;
            descriptor.summary = "Run the declared headless host operation.";
            descriptor.output = {.id = OutputSchemaId(SelectCommand(path)),
                                 .version = 1,
                                 .formats = CliOutputFormat::Human | CliOutputFormat::Json | CliOutputFormat::JsonLines,
                                 .progressRecords = SelectCommand(path) == HostCommand::Bundle};
            descriptor.hosts = CliHostAvailability::HoroEngine;
            descriptor.contractVersion = {1, 0, 0};
            descriptor.ownerId = "horo.host.cli";
            descriptor.cancellation = CliCancellationPolicy::Cooperative;
            descriptor.interactive = CliInteractivePolicy::Forbidden;
            descriptor.sideEffects = CliSideEffectPolicy::ReadsState;
            if (SelectCommand(path) == HostCommand::Smoke)
                descriptor.requiredCapabilities = {{"horo.observability.smoke"}};
            if (SelectCommand(path) == HostCommand::McpServe) {
                descriptor.summary = "Serve the MCP-owned local protocol on exclusive stdio.";
                descriptor.requiredCapabilities = {{"horo.mcp.serve"}, {"horo.observability.smoke"}};
                descriptor.sideEffects = CliSideEffectPolicy::MutatesState;
                descriptor.stdinPolicy = CliStdinPolicy::BinaryStream;
            }
            if (SelectCommand(path) == HostCommand::Bundle) {
                descriptor.sideEffects = CliSideEffectPolicy::WritesFiles;
                descriptor.options.push_back({.name = "output-path",
                                              .summary = "Absolute destination for a new diagnostic ZIP.",
                                              .valueKind = CliOptionValueKind::Path,
                                              .required = true});
            }
            descriptors.push_back(std::move(descriptor));
        }
        return descriptors;
    }

    /** @copydoc HostCommandAdapters */
    std::vector<Cli::CliCommandAdapterRegistration> HostCommandAdapters(const std::span<const Cli::CliCommandDescriptor> descriptors,
                                                                        std::shared_ptr<HostObservabilitySession> session,
                                                                        std::string help) {
        std::vector<Cli::CliCommandAdapterRegistration> adapters;
        for (const auto &descriptor : descriptors)
            adapters.push_back({.adapter = std::make_unique<HostCommandAdapter>(descriptor, session, help),
                                .capabilities = descriptor.requiredCapabilities});
        return adapters;
    }
}  // namespace Horo::Application::Internal
