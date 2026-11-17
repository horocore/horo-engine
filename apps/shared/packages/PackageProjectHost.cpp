#include "PackageProjectHostInternal.h"

#include <limits>

namespace Horo::Application::Internal {
    namespace PackageHost {
        /** @brief Preserves distinct host binding/lifetime failures without translating canonical authority causes. */
        Error Failure(const char *code) {
            return Error{ErrorCode{code}, ErrorDomainId{"packages.host"}};
        }
    }  // namespace PackageHost

    using PackageHost::Failure;

    Result<void> PackageProjectHost::Impl::Lane() const {
        if (std::this_thread::get_id() != owner)
            return Result<void>::Failure(Failure("packages.host.owner_lane"));
        return Result<void>::Success();
    }

    Result<void> PackageProjectHost::Impl::AdvanceGeneration() {
        if (generation == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(Failure("packages.host.generation_exhausted"));
        ++generation;
        return Result<void>::Success();
    }

    bool PackageProjectHost::Impl::Fresh(const Duration now) const noexcept {
        return sampledAt && now >= *sampledAt && now - *sampledAt <= maximumAge;
    }

    /** @copydoc PackageProjectHost::Create */
    Result<std::unique_ptr<PackageProjectHost>> PackageProjectHost::Create(PackageProjectHostServices services, PackageProjectPolicy policy,
                                                                           const Duration metadataMaximumAge,
                                                                           Packages::IPackageRestoreSource *source,
                                                                           Packages::PackagePublisherVerificationService *publishers) {
        if (metadataMaximumAge <= Duration{} || metadataMaximumAge > Duration::FromMilliseconds(60'000) || policy.cacheRoot.empty() ||
            !policy.cacheRoot.is_absolute() || !policy.lifecycle.artifactGate)
            return Result<std::unique_ptr<PackageProjectHost>>::Failure(Failure("packages.host.invalid_policy"));
        auto impl = std::make_unique<Impl>(services, std::move(policy), metadataMaximumAge, source, publishers);
        return Result<std::unique_ptr<PackageProjectHost>>::Success(
            std::make_unique<PackageProjectHost>(ConstructionKey{}, std::move(impl)));
    }

    /** @copydoc PackageProjectHost::PackageProjectHost */
    PackageProjectHost::PackageProjectHost(const ConstructionKey &, std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    /** @copydoc PackageProjectHost::~PackageProjectHost */
    PackageProjectHost::~PackageProjectHost() noexcept {
        try {
            if (Close().HasError())
                std::terminate();
        } catch (...) {
            std::terminate();
        }
    }

    /** @copydoc PackageProjectHost::Acquire */
    Result<std::shared_ptr<PackageApplication>> PackageProjectHost::Acquire(const std::filesystem::path &root) {
        if (auto valid = impl_->Lane(); valid.HasError())
            return Result<std::shared_ptr<PackageApplication>>::Failure(valid.ErrorValue());
        if (impl_->closing || impl_->closed)
            return Result<std::shared_ptr<PackageApplication>>::Failure(Failure("packages.host.closed"));
        if (root.empty() || !root.is_absolute() || root != root.lexically_normal())
            return Result<std::shared_ptr<PackageApplication>>::Failure(Failure("packages.host.invalid_project"));
        if (root != impl_->policy.projectRoot || impl_->generation == 0) {
            if (auto advanced = impl_->AdvanceGeneration(); advanced.HasError())
                return Result<std::shared_ptr<PackageApplication>>::Failure(advanced.ErrorValue());
            impl_->policy.projectRoot = root;
            impl_->retiring = true;
            impl_->prepared.reset();
            impl_->sampledAt.reset();
            impl_->diagnostic.reset();
            if (impl_->metadataJob)
                static_cast<void>(impl_->metadataJob->RequestCancel());
        }
        if (auto pumped = Pump(Packages::PackageActivationBoundary::Running); pumped.HasError())
            return Result<std::shared_ptr<PackageApplication>>::Failure(pumped.ErrorValue());
        if (impl_->diagnostic)
            return Result<std::shared_ptr<PackageApplication>>::Failure(*impl_->diagnostic);
        if (!impl_->current || impl_->retiring || !impl_->Fresh(impl_->services.clock.MonotonicNow()))
            return Result<std::shared_ptr<PackageApplication>>::Failure(Failure("packages.host.preparing"));
        return impl_->current->Application();
    }

    /** @copydoc PackageProjectHost::Pump */
    Result<void> PackageProjectHost::Pump(const Packages::PackageActivationBoundary boundary) {
        if (auto valid = impl_->Lane(); valid.HasError())
            return valid;
        if (impl_->closed || impl_->closing)
            return Result<void>::Failure(Failure("packages.host.closed"));
        const auto now = impl_->services.clock.MonotonicNow();
        impl_->ReceiveMetadata(now);
        if (impl_->retiring) {
            if (auto retired = impl_->RetireCurrent(); retired.HasError())
                return retired;
        }
        if (impl_->prepared) {
            if (auto composed = impl_->ComposePrepared(); composed.HasError())
                return composed;
        }
        if (!impl_->policy.projectRoot.empty() && !impl_->metadataJob && !impl_->Fresh(now)) {
            if (auto started = impl_->StartMetadata(); started.HasError()) {
                impl_->diagnostic = started.ErrorValue();
                return started;
            }
        }
        if (impl_->current)
            return impl_->current->Pump(impl_->Fresh(now) ? boundary : Packages::PackageActivationBoundary::Running);
        return Result<void>::Success();
    }

    /** @copydoc PackageProjectHost::PumpProject */
    Result<void> PackageProjectHost::PumpProject(const std::filesystem::path &root, const Packages::PackageActivationBoundary boundary) {
        if (auto valid = impl_->Lane(); valid.HasError())
            return valid;
        if (root != impl_->policy.projectRoot)
            return Result<void>::Failure(Failure("packages.host.project_stale"));
        return Pump(boundary);
    }

    /** @copydoc PackageProjectHost::Retire */
    Result<void> PackageProjectHost::Retire(const std::filesystem::path &root) {
        if (auto valid = impl_->Lane(); valid.HasError())
            return valid;
        if (impl_->closed || impl_->closing)
            return Result<void>::Failure(Failure("packages.host.closed"));
        if (root != impl_->policy.projectRoot)
            return Result<void>::Failure(Failure("packages.host.project_stale"));
        if (auto advanced = impl_->AdvanceGeneration(); advanced.HasError())
            return advanced;
        impl_->retiring = true;
        impl_->policy.projectRoot.clear();
        impl_->sampledAt.reset();
        impl_->prepared.reset();
        if (impl_->metadataJob)
            static_cast<void>(impl_->metadataJob->RequestCancel());
        return impl_->RetireCurrent();
    }

    /** @copydoc PackageProjectHost::Generation */
    std::uint64_t PackageProjectHost::Generation() const {
        return std::this_thread::get_id() == impl_->owner && !impl_->closing && !impl_->closed && !impl_->retiring ? impl_->generation : 0;
    }

    /** @copydoc PackageProjectHost::InstalledRecord */
    Result<std::shared_ptr<const Packages::VerifiedPackageInstallRecord>> PackageProjectHost::InstalledRecord(
        const std::filesystem::path &root, const std::uint64_t generation) const {
        using Outcome = Result<std::shared_ptr<const Packages::VerifiedPackageInstallRecord>>;
        if (auto valid = impl_->Lane(); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        if (root != impl_->policy.projectRoot || generation == 0 || generation != impl_->generation)
            return Outcome::Failure(Failure("packages.host.project_stale"));
        if (auto admitted = impl_->Admission(); admitted.HasError())
            return Outcome::Failure(admitted.ErrorValue());
        if (!impl_->current)
            return Outcome::Failure(Failure("packages.host.preparing"));
        return impl_->current->InstalledRecord();
    }

    /** @copydoc PackageProjectHost::Close */
    Result<void> PackageProjectHost::Close() {
        if (auto valid = impl_->Lane(); valid.HasError())
            return valid;
        if (impl_->closed)
            return Result<void>::Success();
        impl_->closing = true;
        impl_->prepared.reset();
        if (impl_->metadataJob) {
            static_cast<void>(impl_->metadataJob->RequestCancel());
            if (const auto snapshot = impl_->metadataJob->Snapshot();
                !snapshot || snapshot->state == JobState::Queued || snapshot->state == JobState::Running)
                return Result<void>::Failure(Failure("packages.host.busy"));
            impl_->metadataJob.reset();
            impl_->completion.reset();
        }
        if (auto retired = impl_->RetireCurrent(); retired.HasError())
            return retired;
        impl_->closed = true;
        return Result<void>::Success();
    }
}  // namespace Horo::Application::Internal
