#include "Horo/Application/HostObservability.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/HeadlessExtensionHost.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Telemetry/Telemetry.h"
#include "HostModuleComposition.h"

#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>

namespace {
    struct Options {
        bool help{};
        bool emitSmoke{};
        std::filesystem::path diagnosticBundle;
    };

    class DenyToolchainPolicy final : public Horo::Extensions::IToolchainInvocationPolicy {
    public:
        [[nodiscard]] Horo::Result<Horo::ExternalProcessRequest> Resolve(
            const Horo::Extensions::ToolchainProviderDescriptor &, const Horo::Extensions::ToolchainInvocationIntent &) const override {
            return Horo::Result<Horo::ExternalProcessRequest>::Failure(
                Horo::MakeError(Horo::Extensions::ExtensionErrors::ToolchainPolicyRejected,
                                "No toolchain invocation policy is configured for this command."));
        }
    };

    [[nodiscard]] Horo::Application::HostObservabilityConfiguration ObservabilityConfiguration() {
        return {.logging = {.logDirectory = "~/.horo/logs",
                            .baseName = "horo-engine",
                            .hostName = "horo-engine",
                            .hostVersion = HORO_ENGINE_VERSION_STRING},
                .identity = {.processRole = "cli",
                             .engineVersion = HORO_ENGINE_VERSION_STRING,
                             .buildConfiguration = HORO_BUILD_CONFIGURATION,
                             .sourceRevision = HORO_SOURCE_REVISION}};
    }

    [[nodiscard]] Options ParseOptions(const std::span<char *> arguments) {
        Options options;
        std::size_t index = 1;
        while (index < arguments.size()) {
            const std::string_view argument{arguments[index]};
            if (argument == "--help" || argument == "-h") {
                options.help = true;
                ++index;
            } else if (argument == "--emit-observability-smoke") {
                options.emitSmoke = true;
                ++index;
            } else if (argument == "--diagnostic-bundle" && index + 1 < arguments.size()) {
                options.diagnosticBundle = arguments[index + 1];
                index += 2;
            } else {
                ++index;
            }
        }
        return options;
    }

}  // namespace

int main(const int argc, char **argv) {
    const Options options = ParseOptions(std::span{argv, static_cast<std::size_t>(argc)});
    if (options.help) {
        std::cout << "Usage: horo-engine [--emit-observability-smoke] [--diagnostic-bundle <absolute-output.zip>]\n";
        return 0;
    }

    auto composedModules = Horo::Application::Internal::ComposeHostModules({.host = Horo::Application::Internal::HostKind::Headless});
    if (composedModules.HasError()) {
        std::cerr << "horo-engine: module composition failed: " << composedModules.ErrorValue().message << '\n';
        return 2;
    }
    std::unique_ptr<Horo::ModuleHost> moduleHost = std::move(composedModules).Value();

    auto observability = Horo::Application::HostObservabilitySession::Start(ObservabilityConfiguration());
    if (observability == nullptr) {
        std::cerr << "horo-engine: observability initialization failed\n";
        return 2;
    }

    DenyToolchainPolicy toolchainPolicy;
    Horo::NativeExternalProcessRunner externalProcesses;
    auto extensionHost = Horo::Extensions::HeadlessExtensionHost::Create({}, toolchainPolicy, externalProcesses);
    if (extensionHost.HasError() || extensionHost.Value()->Start({}).HasError()) {
        std::cerr << "horo-engine: headless extension composition failed\n";
        return 2;
    }

    HORO_LOG_INFO("foundation.host", "Headless host initialized");
    if (options.emitSmoke) {
        const auto gameCounter =
            Horo::Telemetry::Runtime::RegisterCounter({.name = "game.smoke.completed", .subsystem = "Game.Smoke", .unit = "operations"});
        const auto pluginGauge =
            Horo::Telemetry::Runtime::RegisterGauge({.name = "plugin.example.active", .subsystem = "Plugin.example", .unit = "instances"});
        gameCounter.Add();
        pluginGauge.Set(1.0);
        static_cast<void>(Horo::Telemetry::Runtime::EmitEvent("Game.Smoke", "game.smoke.completed", Horo::Log::Level::Info,
                                                              "Headless observability smoke completed"));
    }

    if (!options.diagnosticBundle.empty()) {
        const auto result = observability->GenerateDiagnosticBundle({.outputPath = options.diagnosticBundle});
        if (result.HasError()) {
            std::cerr << "horo-engine: " << result.ErrorValue().message << '\n';
            return 3;
        }
        std::cout << result.Value().outputPath.string() << '\n';  // NOSONAR(cpp:S5145)
    }
    extensionHost.Value()->Shutdown();
    moduleHost->DeactivateAll();
    return 0;
}
