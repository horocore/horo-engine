#pragma once
#include "PackageProjectHost.h"

#include <mutex>

namespace Horo::Application::Internal {
    namespace PackageHost {
        [[nodiscard]] Error Failure(const char *code);

        struct Completion final {
            std::mutex mutex; /**< Protects only worker result transfer; no owner-lane/native callback runs under this lock. */
            std::optional<Result<PreparedPackageProject>> result;
        };
    }  // namespace PackageHost

    struct PackageProjectHost::Impl final {
        PackageProjectHostServices services;
        PackageProjectPolicy policy;
        Duration maximumAge;
        Packages::IPackageRestoreSource *source;
        Packages::PackagePublisherVerificationService *publishers;
        const std::thread::id owner{std::this_thread::get_id()};
        std::uint64_t generation{};
        bool closing{};
        bool closed{};
        bool retiring{};
        std::optional<Duration> sampledAt;
        std::optional<Error> diagnostic;
        std::shared_ptr<PackageProjectSession> current;
        std::optional<PreparedPackageProject> prepared;
        std::optional<JobHandle> metadataJob;
        std::filesystem::path jobRoot;
        Duration jobSampleTime;
        std::shared_ptr<PackageHost::Completion> completion;
        Sha256Digest requestDigest;
        Sha256Digest lockDigest;

        Impl(const PackageProjectHostServices &hostServices, PackageProjectPolicy p, Duration age, Packages::IPackageRestoreSource *s,
             Packages::PackagePublisherVerificationService *v)
            : services(hostServices), policy(std::move(p)), maximumAge(age), source(s), publishers(v) {}

        /** @brief Revokes old admission before attempting retirement of rejected metadata. */
        void RejectMetadata(Error error);
        [[nodiscard]] Result<void> Lane() const;
        [[nodiscard]] Result<void> AdvanceGeneration();
        [[nodiscard]] bool Fresh(Duration now) const noexcept;
        [[nodiscard]] Result<void> Admission() const;
        [[nodiscard]] Result<void> StartMetadata();
        void ReceiveMetadata(Duration now);
        [[nodiscard]] Result<void> RetireCurrent();
        [[nodiscard]] Result<void> ComposePrepared();
    };
}  // namespace Horo::Application::Internal
