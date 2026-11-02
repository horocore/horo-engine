#include "ExtensionScriptCliHost.h"

#include <algorithm>

namespace Horo::Application::Internal {
    namespace {
        class ScriptCommand final : public Cli::ICliCommandAdapter {
        public:
            ScriptCommand(ExtensionScriptCliHost &script, Extensions::HeadlessExtensionHost &host)
                : script_(script), host_(host), descriptor_(DescribeExtensionScriptCommand()) {}

            const Cli::CliCommandDescriptor &GetDescriptor() const noexcept override {
                return descriptor_;
            }

            Result<Cli::CliCommandResult> Execute(const Cli::CliCommandRequest &, Cli::CliExecutionContext &context) override {
                return script_.Execute(host_, context);
            }

        private:
            ExtensionScriptCliHost &script_;
            Extensions::HeadlessExtensionHost &host_;
            Cli::CliCommandDescriptor descriptor_;
        };
    }  // namespace

    Cli::CliCommandDescriptor DescribeExtensionScriptCommand() {
        using namespace Cli;
        CliCommandDescriptor descriptor;
        descriptor.path = {{"extension", "script-run"}};
        descriptor.summary = "Run one explicitly approved signed packaged Lua service script.";
        descriptor.requiredCapabilities = {{"horo.extension.script-run"}};
        descriptor.output = {.id = "horo.cli.extension-script",
                             .version = 1,
                             .formats = CliOutputFormat::Human | CliOutputFormat::Json | CliOutputFormat::JsonLines};
        descriptor.interactive = CliInteractivePolicy::Forbidden;
        descriptor.hosts = CliHostAvailability::HoroEngine;
        descriptor.contractVersion = {1, 0, 0};
        descriptor.sideEffects = CliSideEffectPolicy::WritesFiles;
        descriptor.cancellation = CliCancellationPolicy::Cooperative;
        descriptor.ownerId = "horo.host.cli";
        for (const auto *name : {"extension-root", "host-trust", "temporary-root"})
            descriptor.options.push_back(
                {.name = name, .summary = "Explicit absolute host-owned path.", .valueKind = CliOptionValueKind::Path, .required = true});
        for (const auto *name : {"extension-id", "module", "import", "publisher", "script"})
            descriptor.options.push_back({.name = name,
                                          .summary = "Exact signed package identity or package-relative Lua path.",
                                          .valueKind = CliOptionValueKind::String,
                                          .required = true});
        for (const auto *name : {"approve-native", "approve-script"})
            descriptor.options.push_back({.name = name,
                                          .summary = "Explicit invocation-only consent; signatures and trust still required.",
                                          .valueKind = CliOptionValueKind::Flag,
                                          .required = true});
        // Registry admission canonicalizes option order; the actual adapter must present that same descriptor.
        std::ranges::sort(descriptor.options, {}, &Cli::CliOptionDescriptor::name);
        return descriptor;
    }

    Cli::CliCommandAdapterRegistration MakeExtensionScriptCommandAdapter(ExtensionScriptCliHost &script,
                                                                         Extensions::HeadlessExtensionHost &host) {
        return {.adapter = std::make_unique<ScriptCommand>(script, host), .capabilities = {{"horo.extension.script-run"}}};
    }
}  // namespace Horo::Application::Internal
