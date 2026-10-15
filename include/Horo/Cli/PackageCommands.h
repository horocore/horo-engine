#pragma once
/** @file PackageCommands.h @brief Package CLI metadata and adapters over the shared application operation owner. */
#include "Horo/Application/PackageApplication.h"
#include "Horo/Cli/CliDispatcher.h"

#include <functional>

namespace Horo::Cli {
    /** @brief Returns inert package command declarations without reading or mutating project state.
     * @return Restore, install, activate and disable descriptors for the existing dispatcher/parser. */
    [[nodiscard]] std::vector<CliCommandDescriptor> DescribePackageCommands();
    /** @brief Explicit invocation-bound project acquisition; preparation observes the admitted command cancellation/deadline. */
    using PackageCommandApplication =
        std::function<Result<std::shared_ptr<Application::PackageApplication>>(const CliCommandRequest &, CliExecutionContext &)>;
    /** @brief Binds presentation adapters to one explicit project owner; no package operation runs during construction.
     * @param application Explicit invocation-bound factory issuing a retained project owner, drained before borrowed authorities are
     * destroyed.
     * @param boundary Host-only safe-point projection; an invocation cannot declare itself quiescent.
     * @return Owned registrations requiring exact horo.package.manage capability.
     * Commands run on the application's owner lane and observe terminal state before returning. Cancellation
     * requests cooperative source/job teardown; the host keeps services alive through that acknowledgement.
     */
    [[nodiscard]] std::vector<CliCommandAdapterRegistration> MakePackageCommandAdapters(
        PackageCommandApplication application, std::function<Result<Packages::PackageActivationBoundary>()> boundary);
}  // namespace Horo::Cli
