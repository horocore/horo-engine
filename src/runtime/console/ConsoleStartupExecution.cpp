#include "Horo/Debug/ConsoleStartupExecution.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>

namespace Horo::Debug {
    /** @copydoc PreparedConsoleStartup::PreparedConsoleStartup */
    PreparedConsoleStartup::PreparedConsoleStartup(std::shared_ptr<const CommandRegistry> registry, ConfigurationSnapshotRef configuration)
        : registry_(std::move(registry)), configuration_(std::move(configuration)) {}

    /** @copydoc PreparedConsoleStartup::Prepare */
    Result<PreparedConsoleStartup> PreparedConsoleStartup::Prepare(const ConsoleLaunchConfiguration &launch,
                                                                   const ConfigurationSnapshotRef &configuration,
                                                                   std::shared_ptr<const CommandRegistry> registry,
                                                                   const DebugCommandContext &principal, const ConsoleStartupFiles &files,
                                                                   ConsoleStartupLimits limits) {
        if (!registry || !registry->Profile() || launch.configurationRevision != configuration.Revision())
            return Result<PreparedConsoleStartup>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        if (limits.commands == 0 || limits.commands > 256 || limits.commandBytes == 0 || limits.commandBytes > (1U << 20U) ||
            limits.scripts > 16 || launch.startup.size() > 128 || files.approvedPaths.size() > 256)
            return Result<PreparedConsoleStartup>::Failure(MakeCommandError(CommandError::LimitExceeded));
        auto settings = ReadConsoleConfiguration(configuration);
        if (settings.HasError())
            return Result<PreparedConsoleStartup>::Failure(settings.ErrorValue());
        if (!launch.startup.empty() && !settings.Value().startupAllowed)
            return Result<PreparedConsoleStartup>::Failure(MakeCommandError(CommandError::StartupDenied));
        std::size_t scripts{};
        for (const auto &request : launch.startup) {
            if (request.text.size() > 4096)
                return Result<PreparedConsoleStartup>::Failure(MakeCommandError(CommandError::LimitExceeded));
            if (request.kind == ConsoleStartupKind::Command) {
                const auto checked = registry->Prepare(request.text, principal, true);
                if (checked.HasError())
                    return Result<PreparedConsoleStartup>::Failure(checked.ErrorValue());
            } else if (request.kind == ConsoleStartupKind::Script) {
                if (++scripts > limits.scripts)
                    return Result<PreparedConsoleStartup>::Failure(MakeCommandError(CommandError::LimitExceeded));
                if (!files.files || !files.context)
                    return Result<PreparedConsoleStartup>::Failure(MakeCommandError(CommandError::MissingCapability));
                if (!Platform::IsSafeProjectReadPath(request.text) ||
                    std::ranges::find(files.approvedPaths, request.text) == files.approvedPaths.end())
                    return Result<PreparedConsoleStartup>::Failure(MakeCommandError(CommandError::StartupDenied));
            } else
                return Result<PreparedConsoleStartup>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        }
        PreparedConsoleStartup plan(std::move(registry), configuration);
        if (files.context) {
            if (files.context->projectIdentity.empty() || files.context->projectIdentity.size() > 4096 ||
                !IsValidUtf8ScalarSequence(files.context->projectIdentity) || files.context->projectGeneration == 0)
                return Result<PreparedConsoleStartup>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
            plan.projectIdentity_ = files.context->projectIdentity;
            plan.projectGeneration_ = files.context->projectGeneration;
            plan.projectCancellation_ = files.context->cancellation;
            auto checked = plan.CheckProject(files.context);
            if (checked.HasError())
                return Result<PreparedConsoleStartup>::Failure(checked.ErrorValue());
        }
        plan.commands_.reserve(limits.commands);
        auto remaining = limits.commandBytes;
        const auto append = [&](std::string_view line) -> Result<void> {
            if (plan.commands_.size() == limits.commands || line.size() > remaining)
                return Result<void>::Failure(MakeCommandError(CommandError::LimitExceeded));
            remaining -= line.size();
            plan.commands_.emplace_back(line);
            return Result<void>::Success();
        };
        for (const auto &request : launch.startup) {
            if (request.kind == ConsoleStartupKind::Command) {
                const auto added = append(request.text);
                if (added.HasError())
                    return Result<PreparedConsoleStartup>::Failure(added.ErrorValue());
            } else {
                auto script =
                    ReadConsoleScript(request.text, files.approvedPaths, *files.files, *files.context, *plan.registry_, principal);
                if (script.HasError())
                    return Result<PreparedConsoleStartup>::Failure(script.ErrorValue());
                for (const auto &line : script.Value()) {
                    const auto added = append(line);
                    if (added.HasError())
                        return Result<PreparedConsoleStartup>::Failure(added.ErrorValue());
                }
            }
        }
        const auto current = plan.CheckProject(files.context);
        if (current.HasError())
            return Result<PreparedConsoleStartup>::Failure(current.ErrorValue());
        return Result<PreparedConsoleStartup>::Success(std::move(plan));
    }

    /** @copydoc PreparedConsoleStartup::CheckProject */
    Result<void> PreparedConsoleStartup::CheckProject(const Platform::ProjectReadContext *currentProject) const {
        if (projectCancellation_.IsCancellationRequested())
            return Result<void>::Failure(JobCancelled().ErrorValue());
        if (projectIdentity_.empty())
            return Result<void>::Success();
        if (!currentProject || currentProject->projectIdentity != projectIdentity_ ||
            currentProject->projectGeneration != projectGeneration_)
            return Result<void>::Failure(MakeCommandError(CommandError::StartupDenied));
        return Platform::CheckProjectReadContext(*currentProject);
    }

    /** @copydoc PreparedConsoleStartup::Submit */
    Result<ConsoleStartupDispatch> PreparedConsoleStartup::Submit(CommandExecutor &executor, const DebugCommandContext &principal,
                                                                  const Platform::ProjectReadContext *currentProject) {
        if (!executor.IsOwnerThread())
            return Result<ConsoleStartupDispatch>::Failure(MakeCommandError(CommandError::WrongThread));
        if (!registry_ || consumed_ || !executor.UsesRegistry(*registry_))
            return Result<ConsoleStartupDispatch>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        const auto current = CheckProject(currentProject);
        if (current.HasError())
            return Result<ConsoleStartupDispatch>::Failure(current.ErrorValue());
        for (const auto &line : commands_) {
            const auto allowed = registry_->Prepare(line, principal, true);
            if (allowed.HasError())
                return Result<ConsoleStartupDispatch>::Failure(allowed.ErrorValue());
        }
        std::vector<CommandReceipt> receipts;
        receipts.reserve(commands_.size());
        consumed_ = true;
        for (const auto &line : commands_) {
            auto live = CheckProject(currentProject);
            if (live.HasError())
                return Result<ConsoleStartupDispatch>::Success(
                    ConsoleStartupPartialFailure{std::move(receipts), std::move(live).ErrorValue()});
            auto submitted = executor.Submit(line, principal, true, configuration_);
            if (submitted.HasError())
                return Result<ConsoleStartupDispatch>::Success(
                    ConsoleStartupPartialFailure{std::move(receipts), std::move(submitted).ErrorValue()});
            receipts.push_back(std::move(submitted).Value());
        }
        return Result<ConsoleStartupDispatch>::Success(ConsoleStartupAccepted{std::move(receipts)});
    }

    /** @copydoc PreparedConsoleStartup::CommandCount */
    std::size_t PreparedConsoleStartup::CommandCount() const noexcept {
        return commands_.size();
    }
}  // namespace Horo::Debug
