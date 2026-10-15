#pragma once
#include "Horo/Application/PackageApplication.h"

#include <mutex>

namespace Horo::Application {
    namespace PackageOperations {
        [[nodiscard]] Error Failure(std::string_view code);

        struct Completion final {
            std::mutex mutex;
            std::optional<Result<void>> result;
        };
    }  // namespace PackageOperations

    struct PackageApplication::Impl final {
        Impl(JobSystem &scheduler, Packages::PackageRestoreService &restores, Packages::PackageInstallService &installs,
             Packages::PackageLifecycleService &activations, PackageApplicationConfiguration settings)
            : jobs(scheduler), restore(restores), install(installs), lifecycle(activations), configuration(std::move(settings)),
              owner(std::this_thread::get_id()) {}

        [[nodiscard]] Result<void> CheckLane() const;
        [[nodiscard]] Result<void> CheckAdmission() const;
        [[nodiscard]] Result<PackageOperationId> Admit(PackageOperationKind kind, const CancellationToken &parent);
        void Finish(Result<void> result, bool cancelled = false);
        [[nodiscard]] Result<PackageOperationId> BeginRestore(PackageOperationKind kind, const CancellationToken &parent);
        void SubmitInstall(std::shared_ptr<const Packages::PackageRestoreGraph> graph);
        [[nodiscard]] Result<void> PublishPlan(std::shared_ptr<const Packages::PackageRestoreGraph> graph);
        void PumpRestore();
        void PumpInstall();
        void PumpActivation(Packages::PackageActivationBoundary boundary);
        JobSystem &jobs;
        Packages::PackageRestoreService &restore;
        Packages::PackageInstallService &install;
        Packages::PackageLifecycleService &lifecycle;
        PackageApplicationConfiguration configuration;
        const std::thread::id owner;
        std::uint64_t nextId{1};
        std::shared_ptr<const PackagePlanSnapshot> plan;
        bool closing{};
        bool closed{};
        std::optional<PackageOperationSnapshot> operation;
        CancellationSource cancellation;
        std::optional<Packages::PackageRestoreOperationHandle> restoreOperation;
        std::optional<JobHandle> installJob;
        std::shared_ptr<PackageOperations::Completion> completion;
        std::vector<Packages::EnabledPackageExtension> enabled;
    };
}  // namespace Horo::Application
