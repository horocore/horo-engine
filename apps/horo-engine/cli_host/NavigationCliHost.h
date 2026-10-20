#pragma once

/** @file NavigationCliHost.h @brief Explicit native navigation composition for one CLI/MCP invocation. */
#include "Horo/Cli/NavigationCommands.h"
#include "McpServe.h"

namespace Horo::Application::Internal {
    /** @brief Owns scheduler, source authority and selected native provider through reverse shutdown. */
    class NavigationCliHost final {
    public:
        NavigationCliHost() = default;
        NavigationCliHost(const NavigationCliHost &) = delete;
        NavigationCliHost &operator=(const NavigationCliHost &) = delete;
        NavigationCliHost(NavigationCliHost &&) = delete;
        NavigationCliHost &operator=(NavigationCliHost &&) = delete;
        ~NavigationCliHost() noexcept;
        /** @brief Binds parser-owned project/definition options without executing project code.
         * @param request Canonical CLI options with an explicit admitted project and definition.
         * @param context Invocation authority, original deadline and cancellation view.
         * @return Retained shared application, or typed source/capability/admission failure.
         */
        [[nodiscard]] Result<std::shared_ptr<NavigationBakeApplication>> Bind(const Cli::CliCommandRequest &request,
                                                                              const Cli::CliExecutionContext &context);
        /** @brief Issues an explicitly project-bound MCP lease.
         * @param request Canonical host CLI options; protocol arguments cannot supply a replacement root.
         * @param context Original admitted invocation authority and deadline.
         * @return Owned application binding and safe-point pump, or typed failure.
         */
        [[nodiscard]] Result<McpNavigationBinding> BindMcp(const Cli::CliCommandRequest &request, const Cli::CliExecutionContext &context);

    private:
        std::optional<JobSystem> jobs_;
        OperationStore operations_{8, 16};
        std::shared_ptr<std::atomic<bool>> closed_{std::make_shared<std::atomic<bool>>(false)};
        std::shared_ptr<NavigationBakeApplication> application_;
        std::string identity_;
    };
}  // namespace Horo::Application::Internal
