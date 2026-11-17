#include "PackageProjectSession.h"

namespace Horo::Application::Internal {
    namespace {
        /** @brief Explicit owner lifetime failures, distinct from portable package diagnostics. */
        Error SessionError(const char *code) {
            return Error{ErrorCode{code}, ErrorDomainId{"packages.host"}};
        }
    }  // namespace

    struct PackageProjectSession::Impl final {
        std::thread::id owner{std::this_thread::get_id()};
        bool closed{};
        std::optional<Packages::PackageCacheStore> cache;
        std::optional<Packages::PackageRestoreService> restore;
        std::optional<Packages::PackageInstallService> install;
        std::unique_ptr<Packages::PackageLifecycleService> lifecycle;
        std::unique_ptr<PackageApplication> application;
    };

    /** @copydoc PackageProjectSession::Create */
    Result<std::shared_ptr<PackageProjectSession>> PackageProjectSession::Create(
        PackageProjectHostServices services, PackageProjectPolicy policy, PreparedPackageProject prepared,
        Packages::IPackageRestoreSource *source, Packages::PackagePublisherVerificationService *publishers) {
        if (const auto &request = prepared.application.restore; request.projectRoot != policy.projectRoot ||
                                                                request.platform != policy.platform || request.mode != policy.mode ||
                                                                request.requirePublisherVerification != policy.requirePublisherVerification)
            return Result<std::shared_ptr<PackageProjectSession>>::Failure(SessionError("packages.host.policy_mismatch"));
        auto impl = std::make_unique<Impl>();
        auto cache = Packages::PackageCacheStore::Create(services.files, policy.cacheRoot, policy.limits.validation);
        if (cache.HasError())
            return Result<std::shared_ptr<PackageProjectSession>>::Failure(cache.ErrorValue());
        impl->cache.emplace(std::move(cache).Value());
        auto restore =
            Packages::PackageRestoreService::Create(services.jobs, services.files, *impl->cache, source, publishers, policy.limits);
        if (restore.HasError())
            return Result<std::shared_ptr<PackageProjectSession>>::Failure(restore.ErrorValue());
        impl->restore.emplace(std::move(restore).Value());
        auto install = Packages::PackageInstallService::Create(services.files, policy.projectRoot);
        if (install.HasError())
            return Result<std::shared_ptr<PackageProjectSession>>::Failure(install.ErrorValue());
        impl->install.emplace(std::move(install).Value());
        auto lifecycle = Packages::PackageLifecycleService::Create(*impl->install, services.trust, std::move(policy.lifecycle));
        if (lifecycle.HasError())
            return Result<std::shared_ptr<PackageProjectSession>>::Failure(lifecycle.ErrorValue());
        impl->lifecycle = std::move(lifecycle).Value();
        prepared.application.wallClock = &services.wallClock;
        auto application =
            PackageApplication::Create(services.jobs, *impl->restore, *impl->install, *impl->lifecycle, std::move(prepared.application));
        if (application.HasError())
            return Result<std::shared_ptr<PackageProjectSession>>::Failure(application.ErrorValue());
        impl->application = std::move(application).Value();
        return Result<std::shared_ptr<PackageProjectSession>>::Success(
            std::make_shared<PackageProjectSession>(ConstructionKey{}, std::move(impl)));
    }

    /** @copydoc PackageProjectSession::PackageProjectSession */
    PackageProjectSession::PackageProjectSession(const ConstructionKey &, std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    /** @copydoc PackageProjectSession::~PackageProjectSession */
    PackageProjectSession::~PackageProjectSession() noexcept {
        try {
            // Reuse the existing final-owner affinity/lifetime violation policy; normal roots cancel, pump and Close explicitly.
            if (Close().HasError())
                std::terminate();
        } catch (...) {
            std::terminate();
        }
    }

    /** @copydoc PackageProjectSession::Application */
    Result<std::shared_ptr<PackageApplication>> PackageProjectSession::Application() {
        if (std::this_thread::get_id() != impl_->owner)
            return Result<std::shared_ptr<PackageApplication>>::Failure(SessionError("packages.host.owner_lane"));
        if (impl_->closed)
            return Result<std::shared_ptr<PackageApplication>>::Failure(SessionError("packages.host.closed"));
        return Result<std::shared_ptr<PackageApplication>>::Success({shared_from_this(), impl_->application.get()});
    }

    /** @copydoc PackageProjectSession::InstalledRecord */
    Result<std::shared_ptr<const Packages::VerifiedPackageInstallRecord>> PackageProjectSession::InstalledRecord() const {
        using Outcome = Result<std::shared_ptr<const Packages::VerifiedPackageInstallRecord>>;
        if (std::this_thread::get_id() != impl_->owner)
            return Outcome::Failure(SessionError("packages.host.owner_lane"));
        if (impl_->closed)
            return Outcome::Failure(SessionError("packages.host.closed"));
        return Outcome::Success(impl_->install->InstalledRecord());
    }

    /** @copydoc PackageProjectSession::Pump */
    Result<void> PackageProjectSession::Pump(const Packages::PackageActivationBoundary boundary) {
        if (std::this_thread::get_id() != impl_->owner)
            return Result<void>::Failure(SessionError("packages.host.owner_lane"));
        if (impl_->closed)
            return Result<void>::Failure(SessionError("packages.host.closed"));
        return impl_->application->Pump(boundary);
    }

    /** @copydoc PackageProjectSession::Close */
    Result<void> PackageProjectSession::Close() {
        if (std::this_thread::get_id() != impl_->owner)
            return Result<void>::Failure(SessionError("packages.host.owner_lane"));
        if (impl_->closed)
            return Result<void>::Success();
        if (auto closed = impl_->application->Close(); closed.HasError())
            return closed;
        if (auto closed = impl_->restore->Shutdown(); closed.HasError())
            return closed;
        impl_->lifecycle->Shutdown();
        impl_->lifecycle->FinalizeRetirements();
        if (impl_->lifecycle->State().restartRequired)
            return Result<void>::Failure(SessionError("packages.host.retirement_busy"));
        impl_->closed = true;
        return Result<void>::Success();
    }
}  // namespace Horo::Application::Internal
