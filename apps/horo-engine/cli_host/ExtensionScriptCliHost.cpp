#include "ExtensionScriptCliInternal.h"
#include "Horo/Cli/CliErrors.h"
#include "Horo/Extensions/LuaExtensionServiceBinding.h"

namespace Horo::Application::Internal {
    struct ExtensionScriptCliHost::Impl final {
        std::optional<ExtensionScriptOptions> options;
        std::shared_ptr<const AuthenticatedExtensionScript> packet;
        std::filesystem::path staging;
        std::vector<Extensions::Discovery::PackageLocation> locations;
        Extensions::ScriptCapabilityScope project{Extensions::ScriptCapabilityScopeKind::Project};
        bool settled{};
        bool activated{};
        bool executed{};
    };

    namespace {
        Extensions::ExtensionAdmissionPolicy ServicePolicy() {
            return {.revision = 1,
                    .knownPermissions = {{HORO_EXTENSION_SERVICE_PERMISSION}},
                    .approvedPermissions = {{HORO_EXTENSION_SERVICE_PERMISSION}},
                    .availableCapabilities = {{HORO_EXTENSION_SERVICE_CAPABILITY}}};
        }

        Extensions::ScriptCapabilityContextDescriptor ScriptPolicy(const ExtensionScriptOptions &options,
                                                                   const AuthenticatedExtensionScript &packet,
                                                                   const Extensions::ScriptCapabilityScope &project) {
            const Extensions::ExtensionCapabilityId capability{packet.service};
            return {.scriptId = options.package + ".script-run",
                    .policyRevision = 1,
                    .profile = Extensions::ScriptCapabilityProfile::Tooling,
                    .packageVerified = true,
                    .packageTrusted = true,
                    .scriptTrusted = true,
                    .runtimeCompatible = true,
                    .packageCapabilities = {capability},
                    .scriptCapabilities = {capability},
                    .projectCapabilities = {capability},
                    .contextCapabilities = {capability},
                    .scopes = {project.Lease()}};
        }
    }  // namespace

    ExtensionScriptCliHost::ExtensionScriptCliHost() : impl_(std::make_unique<Impl>()) {}

    ExtensionScriptCliHost::~ExtensionScriptCliHost() {
        if (!impl_->staging.empty() && impl_->settled) {
            std::error_code error;
            std::filesystem::remove_all(impl_->staging, error);
        }
    }

    Result<Extensions::HeadlessExtensionHostConfiguration> ExtensionScriptCliHost::Prepare(const Cli::CliCommandRequest &request,
                                                                                           const CancellationToken &cancellation) {
        using Configuration = Result<Extensions::HeadlessExtensionHostConfiguration>;
        if (request.command.segments != std::vector<std::string>{"extension", "script-run"})
            return Configuration::Success({});
        auto options = ParseExtensionScriptOptions(request);
        if (options.HasError())
            return Configuration::Failure(options.ErrorValue());
        auto packet = AuthenticateExtensionScript(options.Value(), cancellation);
        if (packet.HasError())
            return Configuration::Failure(packet.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Configuration::Failure(MakeError(Cli::CliErrors::ExecutionCancelled));
        auto staging = StageExtensionScript(options.Value(), *packet.Value());
        if (staging.HasError())
            return Configuration::Failure(staging.ErrorValue());
        impl_->options.emplace(std::move(options).Value());
        impl_->packet = std::move(packet).Value();
        impl_->staging = std::move(staging).Value();
        impl_->settled = true;  // No native module has entered yet; failed host construction can safely clean the owned copy.
        impl_->locations.push_back({impl_->options->package, "script-invocation", impl_->staging.filename(), {}});
        Extensions::HeadlessExtensionHostConfiguration configuration;
        configuration.roots.push_back({"script-invocation", impl_->staging.parent_path(), Extensions::Discovery::RootKind::User,
                                       Extensions::Discovery::RootApproval::Approved,
                                       Extensions::Discovery::ConfigurationOrigin::UserLocal});
        configuration.artifactGate = std::make_shared<ExtensionScriptArtifactGate>(impl_->staging, impl_->packet);
        configuration.servicePolicy = ServicePolicy();
        return Configuration::Success(std::move(configuration));
    }

    void ExtensionScriptCliHost::BeginActivation() noexcept {
        if (impl_->options)
            impl_->settled = false;
    }

    void ExtensionScriptCliHost::ActivationSucceeded() noexcept {
        impl_->activated = true;
    }

    std::span<const Extensions::Discovery::PackageLocation> ExtensionScriptCliHost::Locations() const noexcept {
        return impl_->locations;
    }

    Result<Cli::CliCommandResult> ExtensionScriptCliHost::Execute(Extensions::HeadlessExtensionHost &host,
                                                                  Cli::CliExecutionContext &context) {
        if (!impl_->options || !impl_->packet || impl_->executed || context.IsStopRequested())
            return Result<Cli::CliCommandResult>::Failure(MakeError(Cli::CliErrors::ExecutionContextInvalid));
        auto trusted = RevalidateExtensionScript(*impl_->packet);
        if (trusted.HasError())
            return Result<Cli::CliCommandResult>::Failure(trusted.ErrorValue());
        impl_->executed = true;
        const auto &options = *impl_->options;
        auto result = Extensions::ExecuteHeadlessLuaExtensionService(host, options.package, options.module, options.import,
                                                                     ScriptPolicy(options, *impl_->packet, impl_->project),
                                                                     impl_->packet->source, context.Cancellation());
        if (result.HasError())
            return Result<Cli::CliCommandResult>::Failure(result.ErrorValue());
        return Result<Cli::CliCommandResult>::Success({.fields = {{"value", result.Value()}, {"extension", options.package}}});
    }

    void ExtensionScriptCliHost::ObserveShutdown(const Extensions::HeadlessExtensionHostSnapshot &snapshot) noexcept {
        if (!impl_->options || !impl_->activated)
            return;
        impl_->settled = snapshot.state == Extensions::HeadlessExtensionHostState::Shutdown && snapshot.retirements.size() == 1 &&
                         snapshot.retirements.front().extensionId == impl_->options->package && snapshot.outstandingWork.empty() &&
                         std::ranges::all_of(snapshot.retirements, [](const auto &retirement) {
            return retirement.disposition == Extensions::ExtensionRetirementDisposition::Complete;
        });
    }
}  // namespace Horo::Application::Internal
