#pragma once

/** @file ExtensionScriptCliHost.h @brief Explicit authenticated package-script invocation over the real headless owner. */
#include "Horo/Cli/CliDispatcher.h"
#include "Horo/Extensions/HeadlessExtensionHost.h"

namespace Horo::Application::Internal {
    /** @brief Owns one immutable authenticated invocation and its private staging until actual native retirement. */
    class ExtensionScriptCliHost final {
    public:
        ExtensionScriptCliHost();
        ~ExtensionScriptCliHost();
        ExtensionScriptCliHost(const ExtensionScriptCliHost &) = delete;
        ExtensionScriptCliHost &operator=(const ExtensionScriptCliHost &) = delete;
        /** @brief Authenticates only the exact script-run command, before constructing or loading native modules. */
        [[nodiscard]] Result<Extensions::HeadlessExtensionHostConfiguration> Prepare(const Cli::CliCommandRequest &request,
                                                                                     const CancellationToken &cancellation);
        /** @brief Marks the boundary immediately before native Start; unknown failure retains private staging. */
        [[nodiscard]] Result<void> ConfigureModuleOperations(Extensions::HeadlessExtensionHost &);
        void BeginActivation() noexcept;
        /** @brief Records actual successful Start, enabling evidence-based cleanup after settled shutdown. */
        void ActivationSucceeded() noexcept;
        /** @brief Returns the single verified invocation package; empty for every other CLI command. */
        [[nodiscard]] std::span<const Extensions::Discovery::PackageLocation> Locations() const noexcept;
        /** @brief Executes the authenticated packaged source using the existing headless host, not a second extension manager. */
        [[nodiscard]] Result<Cli::CliCommandResult> Execute(Extensions::HeadlessExtensionHost &host, Cli::CliExecutionContext &context);
        /** @brief Records actual settled shutdown; cleanup failures retain owned staging and never change a committed result. */
        void ObserveShutdown(const Extensions::HeadlessExtensionHostSnapshot &snapshot) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    /** @brief Inert metadata for the concrete existing CLI dispatcher. */
    [[nodiscard]] Cli::CliCommandDescriptor DescribeExtensionScriptCommand();
    [[nodiscard]] Cli::CliCommandDescriptor DescribeModuleScriptCommand();
    /** @brief Binds the actual invocation host and authenticated packet; neither registry nor VM enters protocol arguments. */
    [[nodiscard]] Cli::CliCommandAdapterRegistration MakeExtensionScriptCommandAdapter(ExtensionScriptCliHost &script,
                                                                                       Extensions::HeadlessExtensionHost &host);
    [[nodiscard]] Cli::CliCommandAdapterRegistration MakeModuleScriptCommandAdapter(ExtensionScriptCliHost &,
                                                                                    Extensions::HeadlessExtensionHost &);
}  // namespace Horo::Application::Internal
