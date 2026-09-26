#include "Horo/Packages/PackageRestore.h"

#include "Horo/Packages/PackagePublisherVerification.h"
#include "Horo/Packages/PackageRestoreErrors.h"
#include "PackageRestoreInternal.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <ranges>
#include <system_error>
#include <utility>

namespace Horo::Packages {
    using RestoreInternal::Completion;
    using RestoreInternal::RestoreContext;

    namespace {
        constexpr std::size_t MaximumLockfileBytes = 64U * 1024U * 1024U;
        constexpr std::size_t MaximumLockfilePackages = 4096U;
        constexpr std::size_t MaximumLockfileRoots = 4096U;
        constexpr std::size_t MaximumLockfileDependencies = 1024U;
        constexpr std::size_t MaximumLockfilePlatforms = 256U;
        constexpr std::size_t MaximumLockfileContributions = 1024U;
        constexpr std::uint64_t MaximumArchiveBytes = 1024ULL * 1024ULL * 1024ULL;
        constexpr std::uint64_t MaximumExpandedBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
        constexpr std::uint64_t MaximumFileBytes = 1024ULL * 1024ULL * 1024ULL;
        constexpr std::uint32_t MaximumArchiveEntries = 65'536U;

        [[nodiscard]] bool IsCode(const Error &error, const std::string_view code) noexcept {
            return error.code.Value() == code;
        }

        [[nodiscard]] bool IsCanonicalPlatformField(const std::string_view value) noexcept {
            return !value.empty() && value.size() <= 128U && std::ranges::all_of(value, [](const unsigned char character) {
                return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '.' ||
                       character == '-' || character == '_';
            });
        }

        [[nodiscard]] bool IsCanonicalPlatform(const PackagePlatform &platform) noexcept {
            return IsCanonicalPlatformField(platform.operatingSystem) && IsCanonicalPlatformField(platform.architecture) &&
                   IsCanonicalPlatformField(platform.sdkAbi);
        }

        [[nodiscard]] bool ValidLimitsInternal(const PackageRestoreLimits &limits) noexcept {
            const auto &lockfile = limits.lockfile;
            const auto &validation = limits.validation;
            const std::array<std::pair<std::uint64_t, std::uint64_t>, 11>
                bounds{std::pair{static_cast<std::uint64_t>(lockfile.documentBytes), MaximumLockfileBytes},
                       std::pair{static_cast<std::uint64_t>(lockfile.packages), MaximumLockfilePackages},
                       std::pair{static_cast<std::uint64_t>(lockfile.roots), MaximumLockfileRoots},
                       std::pair{static_cast<std::uint64_t>(lockfile.dependenciesPerPackage), MaximumLockfileDependencies},
                       std::pair{static_cast<std::uint64_t>(lockfile.platformsPerPackage), MaximumLockfilePlatforms},
                       std::pair{static_cast<std::uint64_t>(lockfile.contributionsPerPackage), MaximumLockfileContributions},
                       std::pair{static_cast<std::uint64_t>(validation.archiveBytes), MaximumArchiveBytes},
                       std::pair{static_cast<std::uint64_t>(validation.manifestBytes), MaximumLockfileBytes},
                       std::pair{static_cast<std::uint64_t>(validation.fileBytes), MaximumFileBytes},
                       std::pair{static_cast<std::uint64_t>(validation.expandedBytes), MaximumExpandedBytes},
                       std::pair{static_cast<std::uint64_t>(validation.entries), MaximumArchiveEntries}};
            return limits.supportedPackageFormatVersion != 0U && std::ranges::all_of(bounds, [](const auto bound) {
                return bound.first != 0U && bound.first <= bound.second;
            });
        }

        [[nodiscard]] Result<void> ValidateStartRequestInternal(const PackageRestoreRequest &request, const PackageRestoreLimits &limits) {
            if (request.lockfileJson.size() > limits.lockfile.documentBytes)
                return Result<void>::Failure(MakeError(PackageRestoreErrors::ResourceLimit));
            if (request.projectRoot.empty() && request.lockfileJson.empty())
                return Result<void>::Failure(
                    MakeError(PackageRestoreErrors::InvalidInput, "A project root or an inline lockfile is required for package restore."));
            if (!request.projectRoot.empty()) {
                std::error_code error;
                if (const auto status = std::filesystem::symlink_status(request.projectRoot, error);
                    error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status) ||
                    !request.projectRoot.is_absolute() || request.projectRoot != request.projectRoot.lexically_normal())
                    return Result<void>::Failure(MakeError(PackageRestoreErrors::InvalidInput,
                                                           "Package restore requires an existing absolute canonical project root."));
            }
            if (!IsCanonicalPlatform(request.platform))
                return Result<void>::Failure(
                    MakeError(PackageRestoreErrors::InvalidInput, "Package restore requires a canonical operating-system platform tuple."));
            return Result<void>::Success();
        }

        struct ProgressCounters final {
            std::size_t completedPackages{};
            std::size_t totalPackages{};
            std::size_t cacheHits{};
            std::size_t downloadedPackages{};
        };

        void Report(const std::shared_ptr<Completion> &completion, const PackageRestorePhase phase, const float progress,
                    const ProgressCounters &counters = {}, const std::string_view currentPackage = {}) {
            std::lock_guard lock(completion->Mutex());
            completion->phase = phase;
            completion->progress = std::max(completion->progress, std::clamp(progress, 0.0F, 1.0F));
            completion->completedPackages = counters.completedPackages;
            completion->totalPackages = counters.totalPackages;
            completion->cacheHits = counters.cacheHits;
            completion->downloadedPackages = counters.downloadedPackages;
            completion->currentPackage.assign(currentPackage);
            ++completion->revision;
        }

        [[nodiscard]] Result<std::string> ReadLockfile(const PackageRestoreRequest &request, const PackageRestoreLimits &limits) {
            if (!request.lockfileJson.empty())
                return Result<std::string>::Success(request.lockfileJson);

            const auto path = request.projectRoot / ".horo/packages.lock";
            std::error_code error;
            if (const auto status = std::filesystem::symlink_status(path, error);
                error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status))
                return Result<std::string>::Failure(MakeError(PackageRestoreErrors::LockfileReadFailed));
            const auto size = std::filesystem::file_size(path, error);
            if (error)
                return Result<std::string>::Failure(MakeError(PackageRestoreErrors::LockfileReadFailed));
            if (size > limits.lockfile.documentBytes || size > std::numeric_limits<std::size_t>::max() ||
                size > std::numeric_limits<std::streamsize>::max())
                return Result<std::string>::Failure(MakeError(PackageRestoreErrors::ResourceLimit));

            std::string bytes(static_cast<std::size_t>(size), '\0');
            if (std::ifstream input{path, std::ios::binary};
                !input || (size != 0U && !input.read(bytes.data(), static_cast<std::streamsize>(size))))
                return Result<std::string>::Failure(MakeError(PackageRestoreErrors::LockfileReadFailed));
            return Result<std::string>::Success(std::move(bytes));
        }

        [[nodiscard]] Result<std::optional<ExclusiveFileLock>> AcquireProjectLock(DurableFileSystem &files,
                                                                                  const PackageRestoreRequest &request,
                                                                                  const PackageRestoreOperationId operation) {
            if (request.projectRoot.empty())
                return Result<std::optional<ExclusiveFileLock>>::Success(std::nullopt);
            const auto path = request.projectRoot / ".horo/local/package-restore.lock";
            auto acquired = files.TryAcquireExclusive(path, std::format("package-restore:{}", operation.value));
            if (acquired.HasError()) {
                if (IsCode(acquired.ErrorValue(), "filesystem.lock_busy"))
                    return Result<std::optional<ExclusiveFileLock>>::Failure(MakeError(PackageRestoreErrors::Busy));
                return Result<std::optional<ExclusiveFileLock>>::Failure(
                    WrapError(PackageRestoreErrors::LockUnavailable, acquired.ErrorValue()));
            }
            return Result<std::optional<ExclusiveFileLock>>::Success(std::optional<ExclusiveFileLock>{std::move(acquired).Value()});
        }

        [[nodiscard]] bool EvidenceMatches(const LockedPackage &locked, const ValidatedPackageArchive &archive) noexcept {
            return locked.artifactDigest == archive.Digest() && locked.manifestDigest == archive.PackageManifestDigest() &&
                   locked.fileManifestDigest == archive.Manifest().Digest();
        }

        [[nodiscard]] Error PackageMessage(const ErrorCodeDescriptor &descriptor, const LockedPackage &package) {
            return MakeError(descriptor, std::format("Package '{}' at version '{}' could not be restored.", package.package.Value(),
                                                     package.version.ToString()));
        }

        [[nodiscard]] Error PackageCause(const ErrorCodeDescriptor &descriptor, const LockedPackage &package, Error cause) {
            return WrapError(descriptor, std::move(cause),
                             std::format("Package '{}' at version '{}' could not be restored.", package.package.Value(),
                                         package.version.ToString()));
        }

        [[nodiscard]] Error QuarantineCause(const ErrorCodeDescriptor &primary, const LockedPackage &package, Error cause,
                                            Error quarantine) {
            auto quarantineFailure = WithCause(std::move(quarantine), std::move(cause));
            auto quarantineContext = WrapError(PackageRestoreErrors::QuarantineFailed, std::move(quarantineFailure),
                                               "Failed package bytes could not be isolated from the active cache.");
            return WrapError(primary, std::move(quarantineContext),
                             std::format("Package '{}' at version '{}' failed restore and quarantine.", package.package.Value(),
                                         package.version.ToString()));
        }

        [[nodiscard]] Result<void> Isolate(PackageCacheStore &cache, const std::vector<std::byte> &bytes,
                                           const PackageQuarantineReason reason, const LockedPackage &package) {
            if (auto result = cache.Quarantine(bytes, reason, package.artifactDigest); result.HasError())
                return Result<void>::Failure(result.ErrorValue());
            return Result<void>::Success();
        }

        struct RestoredArtifact final {
            ValidatedPackageArchive archive;
            bool cacheHit{};
        };

        [[nodiscard]] Error QuarantineOrOriginal(const ErrorCodeDescriptor &primary, const LockedPackage &package, PackageCacheStore &cache,
                                                 const std::vector<std::byte> &bytes, const PackageQuarantineReason reason,
                                                 Error original) {
            if (auto isolated = Isolate(cache, bytes, reason, package); isolated.HasError())
                return QuarantineCause(primary, package, std::move(original), std::move(isolated).ErrorValue());
            return original;
        }

        [[nodiscard]] Result<std::optional<RestoredArtifact>> LoadCachedArtifact(const LockedPackage &package,
                                                                                 const PackageRestoreRequest &request,
                                                                                 PackageCacheStore &cache) {
            auto cached = cache.Load(package.artifactDigest);
            if (cached.HasError())
                return Result<std::optional<RestoredArtifact>>::Failure(
                    PackageCause(PackageRestoreErrors::CacheFailure, package, cached.ErrorValue()));
            auto cachedArchive = std::move(cached).Value();
            if (!cachedArchive.has_value())
                return Result<std::optional<RestoredArtifact>>::Success(std::nullopt);

            auto archive = std::move(*cachedArchive);
            if (!EvidenceMatches(package, archive))
                return Result<std::optional<RestoredArtifact>>::Failure(
                    PackageMessage(PackageRestoreErrors::ArtifactEvidenceMismatch, package));
            if (request.requirePublisherVerification)
                return Result<std::optional<RestoredArtifact>>::Failure(
                    PackageMessage(PackageRestoreErrors::PublisherEvidenceUnavailable, package));
            return Result<std::optional<RestoredArtifact>>::Success(
                std::optional<RestoredArtifact>{RestoredArtifact{std::move(archive), true}});
        }

        [[nodiscard]] Result<PackageRestoreArtifact> FetchArtifact(const LockedPackage &package, const PackageRestoreRequest &request,
                                                                   IPackageRestoreSource *source, const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Result<PackageRestoreArtifact>::Failure(MakeError(PackageRestoreErrors::Cancelled));
            if (request.mode == PackageRestoreMode::Offline)
                return Result<PackageRestoreArtifact>::Failure(PackageMessage(PackageRestoreErrors::OfflineArtifactUnavailable, package));
            if (source == nullptr)
                return Result<PackageRestoreArtifact>::Failure(PackageMessage(PackageRestoreErrors::SourceUnavailable, package));

            auto fetched = source->Fetch(PackageRestoreArtifactRequest{package}, cancellation);
            if (fetched.HasError()) {
                if (cancellation.IsCancellationRequested())
                    return Result<PackageRestoreArtifact>::Failure(MakeError(PackageRestoreErrors::Cancelled));
                return Result<PackageRestoreArtifact>::Failure(
                    PackageCause(PackageRestoreErrors::SourceUnavailable, package, fetched.ErrorValue()));
            }
            if (cancellation.IsCancellationRequested())
                return Result<PackageRestoreArtifact>::Failure(MakeError(PackageRestoreErrors::Cancelled));
            return Result<PackageRestoreArtifact>::Success(std::move(fetched).Value());
        }

        [[nodiscard]] Result<ValidatedPackageArchive> ValidateFetchedArtifact(const LockedPackage &package, const RestoreContext &context,
                                                                              PackageRestoreArtifact &artifact,
                                                                              const CancellationToken &cancellation) {
            if (artifact.bytes.size() > context.limits.validation.archiveBytes)
                return Result<ValidatedPackageArchive>::Failure(PackageMessage(PackageRestoreErrors::ResourceLimit, package));

            if (const Sha256Digest actualDigest = ComputeSha256(artifact.bytes); actualDigest != package.artifactDigest) {
                auto failure =
                    MakeError(PackageRestoreErrors::ArtifactHashMismatch,
                              std::format("Package '{}' supplied digest '{}' but the lockfile requires '{}'.", package.package.Value(),
                                          FormatSha256(actualDigest), FormatSha256(package.artifactDigest)));
                return Result<ValidatedPackageArchive>::Failure(
                    QuarantineOrOriginal(PackageRestoreErrors::ArtifactHashMismatch, package, context.cache, artifact.bytes,
                                         PackageQuarantineReason::HashMismatch, std::move(failure)));
            }

            auto verified = ValidatedPackageArchive::Verify(artifact.bytes, context.limits.validation);
            if (verified.HasError()) {
                auto failure = PackageCause(PackageRestoreErrors::ArtifactInvalid, package, verified.ErrorValue());
                return Result<ValidatedPackageArchive>::Failure(
                    QuarantineOrOriginal(PackageRestoreErrors::ArtifactInvalid, package, context.cache, artifact.bytes,
                                         PackageQuarantineReason::InvalidArchive, std::move(failure)));
            }
            auto archive = std::move(verified).Value();
            if (!EvidenceMatches(package, archive)) {
                auto failure = PackageMessage(PackageRestoreErrors::ArtifactEvidenceMismatch, package);
                return Result<ValidatedPackageArchive>::Failure(
                    QuarantineOrOriginal(PackageRestoreErrors::ArtifactEvidenceMismatch, package, context.cache, artifact.bytes,
                                         PackageQuarantineReason::VerificationFailure, std::move(failure)));
            }
            if (cancellation.IsCancellationRequested())
                return Result<ValidatedPackageArchive>::Failure(MakeError(PackageRestoreErrors::Cancelled));
            return Result<ValidatedPackageArchive>::Success(std::move(archive));
        }

        [[nodiscard]] Result<void> VerifyPublisher(const LockedPackage &package, const PackageRestoreRequest &request,
                                                   const RestoreContext &context, PackageRestoreArtifact &artifact,
                                                   const CancellationToken &cancellation) {
            if (context.publisherVerification == nullptr)
                return Result<void>::Success();

            PackagePublisherVerificationRequest verificationRequest{.package = package.package,
                                                                    .artifact = artifact.bytes,
                                                                    .signature = std::move(artifact.signature),
                                                                    .expectedPublisher = std::nullopt,
                                                                    .nowUnixMilliseconds = request.nowUnixMilliseconds,
                                                                    .cancellation = cancellation};
            auto decision = context.publisherVerification->Verify(verificationRequest);
            if (decision.HasError()) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(PackageRestoreErrors::Cancelled));
                auto failure = PackageCause(PackageRestoreErrors::PublisherRejected, package, decision.ErrorValue());
                return Result<void>::Failure(QuarantineOrOriginal(PackageRestoreErrors::PublisherRejected, package, context.cache,
                                                                  artifact.bytes, PackageQuarantineReason::VerificationFailure,
                                                                  std::move(failure)));
            }
            if (decision.Value().decision.installPermitted)
                return Result<void>::Success();

            auto failure = PackageMessage(PackageRestoreErrors::PublisherRejected, package);
            return Result<void>::Failure(QuarantineOrOriginal(PackageRestoreErrors::PublisherRejected, package, context.cache,
                                                              artifact.bytes, PackageQuarantineReason::VerificationFailure,
                                                              std::move(failure)));
        }

        [[nodiscard]] Result<RestoredArtifact> RestoreArtifact(const LockedPackage &package, const PackageRestoreRequest &request,
                                                               const RestoreContext &context, const CancellationToken &cancellation) {
            auto cached = LoadCachedArtifact(package, request, context.cache);
            if (cached.HasError())
                return Result<RestoredArtifact>::Failure(std::move(cached).ErrorValue());
            if (auto cachedArtifact = std::move(cached).Value(); cachedArtifact.has_value())
                return Result<RestoredArtifact>::Success(std::move(*cachedArtifact));

            auto fetched = FetchArtifact(package, request, context.source, cancellation);
            if (fetched.HasError())
                return Result<RestoredArtifact>::Failure(std::move(fetched).ErrorValue());
            auto artifact = std::move(fetched).Value();
            auto verified = ValidateFetchedArtifact(package, context, artifact, cancellation);
            if (verified.HasError())
                return Result<RestoredArtifact>::Failure(std::move(verified).ErrorValue());
            auto archive = std::move(verified).Value();
            if (auto publisher = VerifyPublisher(package, request, context, artifact, cancellation); publisher.HasError())
                return Result<RestoredArtifact>::Failure(std::move(publisher).ErrorValue());
            if (cancellation.IsCancellationRequested())
                return Result<RestoredArtifact>::Failure(MakeError(PackageRestoreErrors::Cancelled));
            if (auto published = context.cache.Publish(archive); published.HasError())
                return Result<RestoredArtifact>::Failure(PackageCause(PackageRestoreErrors::CacheFailure, package, published.ErrorValue()));
            return Result<RestoredArtifact>::Success({std::move(archive), false});
        }

        [[nodiscard]] Result<PackageRestoreGraph> RestorePackages(const RestoreContext &context, const ValidatedPackageLockfileV1 &lockfile,
                                                                  const PackageRestoreRequest &request,
                                                                  const CancellationToken &cancellation,
                                                                  const std::shared_ptr<Completion> &completion) {
            const auto packages = lockfile.Packages();
            PackageRestoreGraph graph{.requestHash = lockfile.RequestHash(), .platform = request.platform};
            graph.packages.reserve(packages.size());
            std::size_t cacheHits = 0U;
            std::size_t downloadedPackages = 0U;
            Report(completion, PackageRestorePhase::RestoringPackages, 0.20F, {.totalPackages = packages.size()});
            for (std::size_t index = 0; index < packages.size(); ++index) {
                const LockedPackage &package = packages[index];
                if (cancellation.IsCancellationRequested())
                    return Result<PackageRestoreGraph>::Failure(MakeError(PackageRestoreErrors::Cancelled));
                Report(completion, PackageRestorePhase::FetchingArtifact,
                       0.20F + 0.70F * static_cast<float>(index) / static_cast<float>(packages.size()),
                       {.completedPackages = index,
                        .totalPackages = packages.size(),
                        .cacheHits = cacheHits,
                        .downloadedPackages = downloadedPackages},
                       package.package.Value());
                auto restored = RestoreArtifact(package, request, context, cancellation);
                if (restored.HasError())
                    return Result<PackageRestoreGraph>::Failure(restored.ErrorValue());
                if (restored.Value().cacheHit)
                    ++cacheHits;
                else
                    ++downloadedPackages;
                Report(completion, PackageRestorePhase::VerifyingArtifact,
                       0.20F + 0.70F * static_cast<float>(index + 1U) / static_cast<float>(packages.size()),
                       {.completedPackages = index + 1U,
                        .totalPackages = packages.size(),
                        .cacheHits = cacheHits,
                        .downloadedPackages = downloadedPackages},
                       package.package.Value());
                RestoredArtifact restoredArtifact = std::move(restored).Value();
                const bool cacheHit = restoredArtifact.cacheHit;
                auto archive = std::make_shared<const ValidatedPackageArchive>(std::move(restoredArtifact.archive));
                graph.packages.emplace_back(package, std::move(archive), cacheHit);
            }
            Report(completion, PackageRestorePhase::Committing, 0.95F,
                   {.completedPackages = packages.size(),
                    .totalPackages = packages.size(),
                    .cacheHits = cacheHits,
                    .downloadedPackages = downloadedPackages});
            return Result<PackageRestoreGraph>::Success(std::move(graph));
        }

        [[nodiscard]] Result<PackageRestoreGraph> RestoreAfterProjectLock(const RestoreContext &context,
                                                                          const PackageRestoreRequest &request,
                                                                          [[maybe_unused]] std::optional<ExclusiveFileLock> projectLock,
                                                                          const CancellationToken &cancellation,
                                                                          const std::shared_ptr<Completion> &completion) {
            Report(completion, PackageRestorePhase::ReadingLockfile, 0.10F);
            auto encoded = ReadLockfile(request, context.limits);
            if (encoded.HasError())
                return Result<PackageRestoreGraph>::Failure(encoded.ErrorValue());
            if (cancellation.IsCancellationRequested())
                return Result<PackageRestoreGraph>::Failure(MakeError(PackageRestoreErrors::Cancelled));

            Report(completion, PackageRestorePhase::ValidatingLockfile, 0.16F);
            auto lockfile = ValidatedPackageLockfileV1::Parse(encoded.Value(), context.limits.lockfile);
            if (lockfile.HasError())
                return Result<PackageRestoreGraph>::Failure(lockfile.ErrorValue());
            if (auto compatible = lockfile.Value().ValidateForRestore(request.expectedRequestHash, request.platform,
                                                                      context.limits.supportedPackageFormatVersion);
                compatible.HasError())
                return Result<PackageRestoreGraph>::Failure(compatible.ErrorValue());
            return RestorePackages(context, lockfile.Value(), request, cancellation, completion);
        }

        [[nodiscard]] Result<PackageRestoreGraph> ExecuteRestore(const RestoreContext &context, const PackageRestoreRequest &request,
                                                                 const PackageRestoreOperationId operation,
                                                                 const CancellationToken &cancellation,
                                                                 const std::shared_ptr<Completion> &completion) {
            Report(completion, PackageRestorePhase::ValidatingRequest, 0.02F);
            if (auto valid = ValidateStartRequestInternal(request, context.limits); valid.HasError())
                return Result<PackageRestoreGraph>::Failure(valid.ErrorValue());
            if (request.requirePublisherVerification && context.publisherVerification == nullptr)
                return Result<PackageRestoreGraph>::Failure(
                    MakeError(PackageRestoreErrors::InvalidInput,
                              "Publisher verification is required but no policy service was composed."));
            if (cancellation.IsCancellationRequested())
                return Result<PackageRestoreGraph>::Failure(MakeError(PackageRestoreErrors::Cancelled));

            Report(completion, PackageRestorePhase::AcquiringProjectLock, 0.05F);
            if (auto projectLock = AcquireProjectLock(context.files, request, operation); projectLock.HasError())
                return Result<PackageRestoreGraph>::Failure(projectLock.ErrorValue());
            else
                return RestoreAfterProjectLock(context, request, std::move(projectLock).Value(), cancellation, completion);
        }

    }  // namespace

    namespace RestoreInternal {
        bool ValidLimits(const PackageRestoreLimits &limits) noexcept {
            return ValidLimitsInternal(limits);
        }

        Result<void> ValidateStartRequest(const PackageRestoreRequest &request, const PackageRestoreLimits &limits) {
            return ValidateStartRequestInternal(request, limits);
        }

        Result<void> RunRestoreJob(const RestoreContext &context, const PackageRestoreRequest &request,
                                   const PackageRestoreOperationId operation, const CancellationToken &cancellation,
                                   const std::shared_ptr<Completion> &completion) {
            auto result = ExecuteRestore(context, request, operation, cancellation, completion);
            if (result.HasError()) {
                std::lock_guard lock(completion->Mutex());
                completion->error = result.ErrorValue();
                ++completion->revision;
                if (ErrorChainContains(result.ErrorValue(), PackageRestoreErrors::Cancelled.domain, PackageRestoreErrors::Cancelled.code))
                    return JobCancelled(std::move(result).ErrorValue());
                return Result<void>::Failure(std::move(result).ErrorValue());
            }
            {
                std::lock_guard lock(completion->Mutex());
                completion->candidate = std::move(result).Value();
                completion->phase = PackageRestorePhase::Committing;
                completion->progress = std::max(completion->progress, 0.96F);
                ++completion->revision;
            }
            return Result<void>::Success();
        }
    }  // namespace RestoreInternal

}  // namespace Horo::Packages
