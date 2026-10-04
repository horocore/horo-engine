#include "CliHost.h"

#include "CliHostCommands.h"
#include "Horo/Cli/CliErrors.h"
#include "Horo/Cli/CliOptionParser.h"
#include "Horo/Cli/CliOutputPresenter.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/HeadlessExtensionHost.h"
#include "Horo/Foundation/Utf8.h"
#include "Horo/Runtime/Save/SaveTelemetry.h"
#include "HostModuleComposition.h"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        using namespace Cli;

        /** @brief Closed legacy compatibility normalization at the host boundary. */
        std::vector<std::string_view> NormalizeArguments(const std::span<const std::string_view> arguments) {
            std::vector<std::string_view> normalized;
            if (arguments.empty())
                return {"host", "inspect"};
            if (arguments.front() == "--help" || arguments.front() == "-h")
                normalized = {"host", "help"};
            else if (arguments.front() == "--emit-observability-smoke")
                normalized = {"observability", "smoke"};
            else if (arguments.front() == "--diagnostic-bundle")
                normalized = {"diagnostics", "bundle", "--output-path"};
            else
                return {arguments.begin(), arguments.end()};
            normalized.insert(normalized.end(), arguments.begin() + 1, arguments.end());
            return normalized;
        }

        /** @brief Selects only transport encoding before parsing so syntax failures retain machine framing. */
        CliProgressOutputMode RequestedMode(const std::span<const std::string_view> arguments) {
            using enum CliProgressOutputMode;
            CliProgressOutputMode mode = Human;
            auto remaining = arguments;
            while (!remaining.empty()) {
                const auto token = remaining.front();
                remaining = remaining.subspan(1);
                if (token == "--")
                    break;
                std::string_view value;
                if ((token == "--output" || token == "-o") && !remaining.empty()) {
                    value = remaining.front();
                    remaining = remaining.subspan(1);
                } else if (token.starts_with("--output="))
                    value = token.substr(9);
                else
                    continue;
                if (value == "json")
                    mode = Json;
                else if (value == "jsonl")
                    mode = JsonLines;
                else if (value == "human")
                    mode = Human;
            }
            return mode;
        }

        /** @brief Pure filesystem spelling adapter; validation does not perform I/O or select a root. */
        class NativePathNormalizer final : public ICliPathNormalizer {
        public:
            Result<std::string> Normalize(const std::string_view text) const override {
                if (text.empty() || text.find('\0') != std::string_view::npos || !IsValidUtf8ScalarSequence(text))
                    return Result<std::string>::Failure(MakeError(CliErrors::ParseFailed));
                const auto spelling = std::filesystem::path{std::u8string{text.begin(), text.end()}}.lexically_normal().generic_u8string();
                return Result<std::string>::Success(std::string{spelling.begin(), spelling.end()});
            }
        };

        /** @brief Exact CLI mappings composed with the original application-domain identities. */
        std::optional<Hosts::ErrorTranslator> Translator() {
            using enum Hosts::ExitCategory;
            const auto cliErrors = CliErrors::ErrorDomain();
            const auto bundleErrors = Diagnostics::DiagnosticBundleErrorDomain();
            const std::vector<ModuleDescriptor> modules{{.id = {"horo.cli"}, .version = {1, 0, 0}, .errorDomains = {cliErrors}},
                                                        {.id = {"horo.foundation"}, .version = {1, 0, 0}, .errorDomains = {bundleErrors}}};
            auto registry = BuildErrorCodeRegistry(modules);
            if (registry.HasError())
                return std::nullopt;
            const std::vector<std::pair<const ErrorCodeDescriptor *, Hosts::ExitCategory>>
                scope{{&CliErrors::DescriptorInvalid, Invariant},
                      {&CliErrors::RegistryCapacityExceeded, Invariant},
                      {&CliErrors::CommandPathDuplicate, Invariant},
                      {&CliErrors::OptionNameDuplicate, Invariant},
                      {&CliErrors::OptionSchemaIncompatible, Invariant},
                      {&CliErrors::OutputSchemaIncompatible, Invariant},
                      {&CliErrors::CapabilityUnauthorized, Permission},
                      {&CliErrors::HostUnsupported, Capability},
                      {&CliErrors::ContractVersionIncompatible, Capability},
                      {&CliErrors::ParserPolicyInvalid, Invariant},
                      {&CliErrors::CommandUnknown, Usage},
                      {&CliErrors::ParseFailed, Usage},
                      {&CliErrors::InputModeUnsupported, Usage},
                      {&CliErrors::InputCapacityExceeded, Usage},
                      {&CliErrors::InteractiveInputUnavailable, Usage},
                      {&CliErrors::DispatchRegistrationInvalid, Invariant},
                      {&CliErrors::CommandUnavailable, Capability},
                      {&CliErrors::SideEffectUnauthorized, Permission},
                      {&CliErrors::ExecutionContextInvalid, Invariant},
                      {&CliErrors::ExecutionCancelled, Cancelled},
                      {&CliErrors::ExecutionTimedOut, Timeout},
                      {&CliErrors::ExecutionCapacityExceeded, Operation},
                      {&CliErrors::HostFailure, Invariant}};
            std::vector<Hosts::ErrorMapping> mappings;
            for (const auto &[descriptor, category] : scope)
                mappings.emplace_back(descriptor->domain, descriptor->code, category);
            mappings.emplace_back(bundleErrors.id, ErrorCode{"observability.bundle.invalid_request"}, Validation);
            mappings.emplace_back(bundleErrors.id, ErrorCode{"observability.bundle.read_failed"}, Operation);
            mappings.emplace_back(bundleErrors.id, ErrorCode{"observability.bundle.write_failed"}, Operation);
            mappings.emplace_back(bundleErrors.id, ErrorCode{"observability.bundle.size_exceeded"}, Validation);
            return Hosts::ErrorTranslator::Create(std::move(registry).Value(), mappings);
        }

        /** @brief Conservative toolchain authority unchanged from the existing headless host. */
        class DenyToolchainPolicy final : public Extensions::IToolchainInvocationPolicy {
        public:
            Result<ExternalProcessRequest> Resolve(const Extensions::ToolchainProviderDescriptor &,
                                                   const Extensions::ToolchainInvocationIntent &) const override {
                return Result<ExternalProcessRequest>::Failure(MakeError(Extensions::ExtensionErrors::ToolchainPolicyRejected));
            }
        };

        /** @brief Reverse shutdown on every terminal path; borrowed adapters are destroyed first. */
        struct HostSession final {
            std::unique_ptr<ModuleHost> modules;
            std::unique_ptr<HostObservabilitySession> observability;
            std::unique_ptr<Runtime::SaveTelemetryRegistration> saveTelemetry;
            std::unique_ptr<Extensions::HeadlessExtensionHost> extensions;

            ~HostSession() {
                if (extensions)
                    extensions->Shutdown();
                saveTelemetry.reset();
                if (modules)
                    modules->DeactivateAll();
            }
        };

        /** @brief Provides an immutable invocation configuration for typed dispatch. */
        Result<ConfigurationSnapshot> Configuration() {
            ConfigurationSchema schema;
            if (const auto sealed = schema.Seal(); sealed.HasError())
                return Result<ConfigurationSnapshot>::Failure(sealed.ErrorValue());
            return ConfigurationResolver::Resolve(schema, {}, 1);
        }

        /** @brief Reserved declaration for syntax failure before a command is admitted. */
        CliCommandDescriptor InvocationDescriptor() {
            using enum CliOutputFormat;
            return {.path = {{"cli", "invocation"}},
                    .summary = "CLI invocation admission.",
                    .output = {.id = "horo.cli.invocation", .version = 1, .formats = Human | Json | JsonLines}};
        }

        /** @brief Completes an original typed error, distinguishing pre-initialization host failure. */
        int CompleteError(CliOutputPresenter &presenter, const Error &error) {
            if (error.domain.Value() == CliErrors::HostFailure.domain.Value() && error.code.Value() == CliErrors::HostFailure.code.Value())
                return presenter.HostFailure().exitCode;
            return presenter.Complete(CliTerminalResult::Failure({.invocation = {1}}, error)).exitCode;
        }

        /** @brief Starts the existing application session with explicit process-owned dependencies. */
        Result<void> ActivateSession(HostSession &session, HostObservabilityIdentity identity, const DenyToolchainPolicy &toolchain,
                                     NativeExternalProcessRunner &processes) {
            auto composed = ComposeHostModules({.host = HostKind::Headless});
            if (composed.HasError())
                return Result<void>::Failure(composed.ErrorValue());
            session.modules = std::move(composed).Value();
            session.observability = HostObservabilitySession::Start({.logging = {.logDirectory = "~/.horo/logs",
                                                                                 .baseName = "horo-engine",
                                                                                 .hostName = "horo-engine",
                                                                                 .hostVersion = identity.engineVersion},
                                                                     .identity = std::move(identity),
                                                                     .summaries = {&Runtime::SummarizeSaveTelemetry}});
            if (!session.observability)
                return Result<void>::Failure(MakeError(CliErrors::HostFailure));
            auto saveTelemetry = Runtime::SaveTelemetryRegistration::Create();
            if (saveTelemetry.HasError())
                return Result<void>::Failure(saveTelemetry.ErrorValue());
            session.saveTelemetry = std::move(saveTelemetry).Value();
            auto extensions = Extensions::HeadlessExtensionHost::Create({}, toolchain, processes);
            if (extensions.HasError())
                return Result<void>::Failure(extensions.ErrorValue());
            session.extensions = std::move(extensions).Value();
            return session.extensions->Start({});
        }

        /** @brief Owns a mutable admitted dispatcher before invoking its borrowed application services. */
        int ExecuteCommand(CliCommandRegistry registry, const std::span<const CliCommandDescriptor> descriptors,
                           const CliCommandRequest &request, HostObservabilityIdentity identity, CliOutputPresenter &presenter) {
            DenyToolchainPolicy toolchain;
            NativeExternalProcessRunner processes;
            HostSession session;
            if (const auto activated = ActivateSession(session, std::move(identity), toolchain, processes); activated.HasError())
                return CompleteError(presenter, activated.ErrorValue());
            auto configuration = Configuration();
            if (configuration.HasError())
                return CompleteError(presenter, configuration.ErrorValue());
            auto admitted =
                CliDispatcher::Create(std::move(registry), HostCommandAdapters(descriptors, *session.observability, {}),
                                      {.activeHost = CliHostKind::HoroEngine, .maximumSideEffects = CliSideEffectPolicy::WritesFiles});
            if (admitted.HasError())
                return CompleteError(presenter, admitted.ErrorValue());
            CliDispatcher dispatcher = std::move(admitted).Value();
            CliProgressMailbox mailbox;
            const auto result =
                dispatcher.Dispatch(request, {.invocation = {1}, .configuration = &configuration.Value(), .progress = &mailbox});
            if (const auto progress = mailbox.Take(); progress)
                static_cast<void>(presenter.Progress(*progress));
            return presenter.Complete(result).exitCode;
        }
    }  // namespace

    /** @copydoc RunCli */
    int RunCli(const std::span<const std::string_view> arguments, HostObservabilityIdentity identity, std::ostream &output,
               std::ostream &diagnostics, const bool terminal, std::optional<Error> admissionFailure) {
        const auto descriptors = DescribeHostCommands();
        auto translator = Translator();
        if (!translator)
            return 1;
        const auto normalized = NormalizeArguments(arguments);
        NativePathNormalizer paths;
        auto registry =
            CliCommandRegistry::Create(descriptors, {.activeHost = CliHostKind::HoroEngine, .supportedContractVersion = {1, 0, 0}});
        const CliCommandDescriptor *selected = nullptr;
        auto parsed = [&]() {
            if (admissionFailure)
                return Result<CliCommandRequest>::Failure(std::move(*admissionFailure));
            if (registry.HasError())
                return Result<CliCommandRequest>::Failure(registry.ErrorValue());
            return CliOptionParser::Parse(normalized, registry.Value(), {.pathNormalizer = &paths, .terminalAvailable = terminal});
        }();
        if (parsed.HasValue())
            selected = registry.Value().Find(parsed.Value().command);
        CliOutputPresenter presenter(selected == nullptr ? InvocationDescriptor() : *selected, {1}, std::move(*translator), output,
                                     diagnostics, RequestedMode(arguments), terminal);
        if (parsed.HasError())
            return presenter.Complete(CliTerminalResult::Failure({.invocation = {1}}, parsed.ErrorValue())).exitCode;
        if (parsed.Value().command.segments == std::vector<std::string>{"host", "help"})
            return presenter
                .Complete(
                    CliTerminalResult::Success({.invocation = {1}}, {.fields = {{"help", registry.Value().GenerateHelp("horo-engine")}}}))
                .exitCode;
        // Closed adapters return domain failures as Results. Native lifecycle/path operations and container
        // construction can still fail through these standard exception contracts at the process boundary.
        try {
            return ExecuteCommand(std::move(registry).Value(), descriptors, parsed.Value(), std::move(identity), presenter);
        } catch (const std::system_error &) {
            return presenter.HostFailure().exitCode;
        } catch (const std::length_error &) {
            return presenter.HostFailure().exitCode;
        } catch (const std::bad_alloc &) {
            return presenter.HostFailure().exitCode;
        }
    }
}  // namespace Horo::Application::Internal
