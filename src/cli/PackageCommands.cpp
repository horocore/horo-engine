#include "Horo/Cli/PackageCommands.h"

#include "Horo/Cli/CliErrors.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>

namespace Horo::Cli {
    namespace {
        enum class Action {
            Restore,
            Install,
            Activate,
            Disable
        };
        constexpr std::array Names{"restore", "install", "activate", "disable"};

        Result<std::vector<Packages::EnabledPackageExtension>> ReadEnabled(const CliCommandRequest &request) {
            std::vector<Packages::EnabledPackageExtension> enabled;
            const auto option = std::ranges::find(request.options, "enable", &CliParsedOption::name);
            if (option == request.options.end())
                return Result<std::vector<Packages::EnabledPackageExtension>>::Failure(MakeError(CliErrors::ParseFailed));
            if (option->values.size() > 64)
                return Result<std::vector<Packages::EnabledPackageExtension>>::Failure(MakeError(CliErrors::ParseFailed));
            for (const auto &value : option->values) {
                const auto *text = std::get_if<std::string>(&value.value);
                if (!text || text->size() > 1281)
                    return Result<std::vector<Packages::EnabledPackageExtension>>::Failure(MakeError(CliErrors::ParseFailed));
                const auto separator = text->find(':');
                if (separator == std::string::npos)
                    return Result<std::vector<Packages::EnabledPackageExtension>>::Failure(MakeError(CliErrors::ParseFailed));
                auto package = Packages::HoroPackageId::Parse(std::string_view{*text}.substr(0, separator));
                auto descriptor = Packages::PackagePath::Parse(std::string_view{*text}.substr(separator + 1));
                if (package.HasError())
                    return Result<std::vector<Packages::EnabledPackageExtension>>::Failure(package.ErrorValue());
                if (descriptor.HasError())
                    return Result<std::vector<Packages::EnabledPackageExtension>>::Failure(descriptor.ErrorValue());
                enabled.push_back({std::move(package).Value(), std::move(descriptor).Value()});
            }
            return Result<std::vector<Packages::EnabledPackageExtension>>::Success(std::move(enabled));
        }

        CliCommandResult Encode(const Application::PackageOperationSnapshot &snapshot) {
            return {.fields = {{"operationId", std::to_string(snapshot.id.value)},
                               {"revision", std::to_string(snapshot.revision)},
                               {"installedRevision", snapshot.installedRevision ? std::to_string(*snapshot.installedRevision) : ""},
                               {"activeGeneration", snapshot.activeGeneration ? std::to_string(*snapshot.activeGeneration) : ""},
                               {"kind", static_cast<std::int64_t>(snapshot.kind)},
                               {"phase", static_cast<std::int64_t>(snapshot.phase)},
                               {"outcome", static_cast<std::int64_t>(snapshot.outcome)},
                               {"completion", static_cast<double>(snapshot.completion)},
                               {"completedPackages", static_cast<std::int64_t>(snapshot.completedPackages)},
                               {"totalPackages", static_cast<std::int64_t>(snapshot.totalPackages)}}};
        }

        class Adapter final : public ICliCommandAdapter {
        public:
            Adapter(CliCommandDescriptor descriptor, PackageCommandApplication application,
                    std::function<Result<Packages::PackageActivationBoundary>()> boundary, const Action action)
                : descriptor_(std::move(descriptor)), applicationFactory_(std::move(application)), boundary_(std::move(boundary)),
                  action_(action) {}

            const CliCommandDescriptor &GetDescriptor() const noexcept override {
                return descriptor_;
            }

            Result<CliCommandResult> Execute(const CliCommandRequest &request, CliExecutionContext &context) override {
                if (!applicationFactory_ || !boundary_)
                    return Result<CliCommandResult>::Failure(MakeError(CliErrors::CommandUnavailable));
                auto acquired = applicationFactory_(request, context);
                if (acquired.HasError())
                    return Result<CliCommandResult>::Failure(acquired.ErrorValue());
                application_ = std::move(acquired).Value();
                if (!application_)
                    return Result<CliCommandResult>::Failure(MakeError(CliErrors::CommandUnavailable));
                const auto admittedBoundary = boundary_();
                if (admittedBoundary.HasError())
                    return Result<CliCommandResult>::Failure(admittedBoundary.ErrorValue());
                const auto boundary = admittedBoundary.Value();
                if (boundary != Packages::PackageActivationBoundary::Startup &&
                    boundary != Packages::PackageActivationBoundary::Quiescent && boundary != Packages::PackageActivationBoundary::Running)
                    return Result<CliCommandResult>::Failure(MakeError(CliErrors::ExecutionContextInvalid));
                auto operation = Start(request, context.Cancellation());
                if (operation.HasError())
                    return Result<CliCommandResult>::Failure(operation.ErrorValue());
                auto bound = context.BindOperation({operation.Value().value});
                if (bound.HasError())
                    static_cast<void>(application_->RequestCancel(operation.Value()));
                return Observe(operation.Value(), context, std::move(bound));
            }

        private:
            Result<Application::PackageOperationId> Start(const CliCommandRequest &request, const CancellationToken &cancellation) {
                switch (action_) {
                    case Action::Restore:
                        return application_->Restore(cancellation);
                    case Action::Install:
                        return application_->Install(cancellation);
                    case Action::Disable:
                        return application_->Disable(cancellation);
                    case Action::Activate: {
                        auto enabled = ReadEnabled(request);
                        if (enabled.HasError())
                            return Result<Application::PackageOperationId>::Failure(enabled.ErrorValue());
                        return application_->Activate(enabled.Value(), cancellation);
                    }
                }
                return Result<Application::PackageOperationId>::Failure(MakeError(CliErrors::CommandUnavailable));
            }

            Result<CliCommandResult> Observe(const Application::PackageOperationId operation, CliExecutionContext &context,
                                             Result<void> presentation) {
                std::uint64_t revision{};
                while (true) {
                    const auto boundary = boundary_();
                    if (boundary.HasError() && presentation.HasValue())
                        presentation = Result<void>::Failure(boundary.ErrorValue());
                    if (context.IsStopRequested() || presentation.HasError())
                        static_cast<void>(application_->RequestCancel(operation));
                    if (auto pumped =
                            application_->Pump(boundary.HasValue() ? boundary.Value() : Packages::PackageActivationBoundary::Running);
                        pumped.HasError())
                        return Result<CliCommandResult>::Failure(presentation.HasError() ? presentation.ErrorValue() : pumped.ErrorValue());
                    const auto current = application_->Query(operation);
                    if (current.HasError())
                        return Result<CliCommandResult>::Failure(current.ErrorValue());
                    const auto &snapshot = current.Value();
                    if (snapshot.outcome != Application::PackageOperationOutcome::Running) {
                        if (presentation.HasError())
                            return Result<CliCommandResult>::Failure(presentation.ErrorValue());
                        if (snapshot.diagnostic)
                            return Result<CliCommandResult>::Failure(*snapshot.diagnostic);
                        return Result<CliCommandResult>::Success(Encode(snapshot));
                    }
                    if (snapshot.revision != revision && presentation.HasValue()) {
                        presentation = context.ReportProgress({.phase = "package." + std::to_string(static_cast<unsigned>(snapshot.phase)),
                                                               .completion = snapshot.completion});
                        revision = snapshot.revision;
                    }
                    // Tooling-only observation; no UI/runtime frame waits or unrelated job pumping.
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            }

            CliCommandDescriptor descriptor_;
            PackageCommandApplication applicationFactory_;
            std::shared_ptr<Application::PackageApplication> application_;
            std::function<Result<Packages::PackageActivationBoundary>()> boundary_;
            Action action_;
        };
    }  // namespace

    /** @copydoc DescribePackageCommands */
    std::vector<CliCommandDescriptor> DescribePackageCommands() {
        std::vector<CliCommandDescriptor> result;
        for (const auto name : Names) {
            CliCommandDescriptor descriptor;
            descriptor.path = {{"package", name}};
            descriptor.summary = "Run the shared project package application operation.";
            descriptor.output = {.id = std::string{"horo.package."} + name,
                                 .version = 1,
                                 .formats = CliOutputFormat::Human | CliOutputFormat::Json | CliOutputFormat::JsonLines,
                                 .progressRecords = true};
            descriptor.hosts = CliHostAvailability::HoroEngine;
            descriptor.contractVersion = {1, 0, 0};
            descriptor.ownerId = "horo.package.cli";
            descriptor.requiredCapabilities = {{"horo.package.manage"}};
            descriptor.cancellation = CliCancellationPolicy::Cooperative;
            descriptor.interactive = CliInteractivePolicy::Forbidden;
            descriptor.sideEffects = CliSideEffectPolicy::WritesFiles;
            for (const char *path : {"package-cache-root", "package-temporary-root"})
                descriptor.options.push_back({.name = path,
                                              .summary = "Explicit absolute host-owned package storage root.",
                                              .valueKind = CliOptionValueKind::Path,
                                              .required = true});
            if (std::string_view{name} == "activate")
                descriptor.options.push_back({.name = "enable",
                                              .summary = "Exact package-id:declared-descriptor selection.",
                                              .valueKind = CliOptionValueKind::String,
                                              .required = true,
                                              .repeatable = true});
            // The registry canonicalizes options before exact adapter-schema admission.
            // Publish that same order so activation cannot invalidate every host registration.
            std::ranges::sort(descriptor.options, {}, &CliOptionDescriptor::name);
            result.push_back(std::move(descriptor));
        }
        return result;
    }

    /** @copydoc MakePackageCommandAdapters */
    std::vector<CliCommandAdapterRegistration> MakePackageCommandAdapters(
        PackageCommandApplication application, std::function<Result<Packages::PackageActivationBoundary>()> boundary) {
        auto descriptors = DescribePackageCommands();
        std::vector<CliCommandAdapterRegistration> result;
        for (std::size_t index = 0; index < descriptors.size(); ++index)
            result.push_back(
                {.adapter = std::make_unique<Adapter>(std::move(descriptors[index]), application, boundary, static_cast<Action>(index)),
                 .capabilities = {{"horo.package.manage"}}});
        return result;
    }
}  // namespace Horo::Cli
