#pragma once

/** @file ConsoleSupportBindings.h @brief Explicit user-approved diagnostic bundle binding over the Foundation bundle owner. */
#include "Horo/Debug/CommandExecutor.h"
#include "Horo/Foundation/Diagnostics/DiagnosticBundle.h"

namespace Horo::Debug {
    /** @brief Host consent authority; the console cannot discover files, choose output paths or manufacture consent. */
    class IConsoleSupportBundleRequests {
    public:
        virtual ~IConsoleSupportBundleRequests() = default;
        /** @brief Consumes one request previously approved through the host's user-consent workflow.
         * @param principal Captured authenticated principal, which the host must match to the approval.
         * @param cancellation Current request/host cancellation.
         * @return One bounded explicit allowlist/output request, or typed missing/expired/revoked approval failure.
         * @note Called on an I/O worker; must not wait for UI interaction. Concurrent calls must not reuse a single approval.
         */
        [[nodiscard]] virtual Result<Diagnostics::DiagnosticBundleRequest> TakeApproved(const DebugCommandContext &principal,
                                                                                        const CancellationToken &cancellation) = 0;
    };

    /** @brief Binds admitted diag.support_bundle to the existing atomic diagnostic ZIP generator.
     * @param registry Immutable composed inventory for the compiled artifact.
     * @param approvals Existing host consent authority, retained through outstanding jobs.
     * @return Owned I/O-worker binding or typed composition failure; no approval/file access occurs during binding.
     * @note Requires the common executor's audit adapter. Only Editor/Development/Diagnostics compile the implementation.
     * Cancellation is checked before approval and before generation. The existing bounded atomic generator is non-cancellable
     * once started; cancellation at that stage may still leave the user-approved committed output. No output is deleted by this adapter.
     */
    [[nodiscard]] Result<std::vector<CommandBinding>> BindConsoleSupportBundleCommands(
        std::shared_ptr<const CommandRegistry> registry, std::shared_ptr<IConsoleSupportBundleRequests> approvals);
}  // namespace Horo::Debug
