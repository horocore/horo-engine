#include "Horo/Debug/ConsoleSupportBindings.h"

#include "Horo/Debug/ConsoleProduct.h"

#if !defined(HORO_PROFILE_EDITOR) && !defined(HORO_PROFILE_DEVELOPMENT) && !defined(HORO_PROFILE_DIAGNOSTICS)
#error "Support bundle execution must be omitted from other product artifacts."
#endif

namespace Horo::Debug {
    namespace {
        /** @brief Retains the consent authority and performs bounded bundle work on the assigned I/O executor. */
        class SupportCommand final : public ICommandHandler {
        public:
            explicit SupportCommand(std::shared_ptr<IConsoleSupportBundleRequests> approvals) : approvals_(std::move(approvals)) {}

            /** @copydoc ICommandHandler::Execute */
            Result<DebugCommandOutput> Execute(const DebugCommandContext &principal, std::span<const ArgumentValue>,
                                               const CancellationToken &cancellation, const JobExecutionContext *job) override {
                if (!job)
                    return Result<DebugCommandOutput>::Failure(MakeCommandError(CommandError::WrongThread));
                if (cancellation.IsCancellationRequested())
                    return Result<DebugCommandOutput>::Failure(JobCancelled().ErrorValue());
                auto approved = approvals_->TakeApproved(principal, cancellation);
                if (approved.HasError())
                    return Result<DebugCommandOutput>::Failure(approved.ErrorValue());
                const auto &request = approved.Value();
                if (request.maxInputBytes == 0 || request.maxInputBytes > (64U << 20U) || request.maxEntries == 0 ||
                    request.maxEntries > 256 || request.entries.size() > request.maxEntries || request.maxMetadataEntries > 64 ||
                    request.metadata.size() > request.maxMetadataEntries)
                    return Result<DebugCommandOutput>::Failure(MakeCommandError(CommandError::LimitExceeded));
                if (cancellation.IsCancellationRequested())
                    return Result<DebugCommandOutput>::Failure(JobCancelled().ErrorValue());
                (void)job->UpdateProgress("generating_bundle", 0.1F);
                auto generated = Diagnostics::GenerateDiagnosticBundle(request);
                if (generated.HasError())
                    return Result<DebugCommandOutput>::Failure(generated.ErrorValue());
                (void)job->UpdateProgress("bundle_committed", 1.0F);
                return Result<DebugCommandOutput>::Success({"Diagnostic bundle committed."});
            }

        private:
            std::shared_ptr<IConsoleSupportBundleRequests> approvals_;
        };
    }  // namespace

    /** @copydoc BindConsoleSupportBundleCommands */
    Result<std::vector<CommandBinding>> BindConsoleSupportBundleCommands(std::shared_ptr<const CommandRegistry> registry,
                                                                         std::shared_ptr<IConsoleSupportBundleRequests> approvals) {
        if (!registry || registry->Profile() != CompiledConsoleProfile())
            return Result<std::vector<CommandBinding>>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        const auto descriptor = registry->Resolve(SupportBundleCommandId, {15, true, true});
        if (descriptor.HasError())
            return Result<std::vector<CommandBinding>>::Success({});
        const auto &value = descriptor.Value();
        if (value.name != "diag.support_bundle" || value.permissions != CommandPermission::Restricted ||
            value.threadPolicy != CommandThreadPolicy::WorkerJob || value.availability != CommandAvailability::DiagnosticsOnly ||
            !value.arguments.empty() || value.flags != (CommandFlags::AuditLogged | CommandFlags::RedactArguments))
            return Result<std::vector<CommandBinding>>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        if (!approvals)
            return Result<std::vector<CommandBinding>>::Failure(MakeCommandError(CommandError::MissingCapability));
        return Result<std::vector<CommandBinding>>::Success(
            {{SupportBundleCommandId, std::make_shared<SupportCommand>(std::move(approvals)), JobResource::Io}});
    }
}  // namespace Horo::Debug
