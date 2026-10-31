#include "Horo/Packages/PackageCache.h"

#include "Horo/Foundation/Logging/Logger.h"
#include "PackageCachePublisherEnvelope.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

namespace Horo::Packages {
    namespace {
        const ErrorDomainId CacheDomain{"horo.packages.cache"};
        const ErrorCodeDescriptor InvalidConfiguration{CacheDomain, ErrorCode{"packages.cache.invalid_configuration"}, ErrorSeverity::Error,
                                                       "Package cache configuration is invalid.",
                                                       "Use an absolute cache root and nonzero resource limits."};
        const ErrorCodeDescriptor IoFailure{CacheDomain,
                                            ErrorCode{"packages.cache.io_failed"},
                                            ErrorSeverity::Error,
                                            "Package cache storage failed.",
                                            "Check cache permissions and available storage.",
                                            true,
                                            true};
        const ErrorCodeDescriptor Busy{CacheDomain,
                                       ErrorCode{"packages.cache.busy"},
                                       ErrorSeverity::Error,
                                       "Another package cache operation owns this artifact.",
                                       "Retry after the operation completes.",
                                       true};
        const ErrorCodeDescriptor Corrupt{CacheDomain,
                                          ErrorCode{"packages.cache.corrupt"},
                                          ErrorSeverity::Error,
                                          "A package cache entry failed integrity verification.",
                                          "Restore the package again from its authoritative source.",
                                          true,
                                          true};
        const ErrorCodeDescriptor InvalidReason{CacheDomain, ErrorCode{"packages.cache.invalid_quarantine_reason"}, ErrorSeverity::Error,
                                                "The quarantine reason is invalid.", "Use a declared package quarantine reason."};
        const ErrorCodeDescriptor ResourceLimit{CacheDomain, ErrorCode{"packages.cache.resource_limit"}, ErrorSeverity::Error,
                                                "Package cache input exceeds its configured resource limit.",
                                                "Reject the artifact or raise the host-owned bound deliberately."};

        [[nodiscard]] std::uint64_t NextQuarantineSequence() noexcept {
            static std::atomic_uint64_t sequence{0};
            return ++sequence;
        }

        [[nodiscard]] std::string DigestHex(const Sha256Digest &digest) {
            constexpr std::string_view Prefix{"sha256:"};
            return FormatSha256(digest).substr(Prefix.size());
        }

        [[nodiscard]] const char *ReasonName(const PackageQuarantineReason reason) noexcept {
            static constexpr std::array Names{"hash-mismatch", "invalid-archive", "verification-failure", "corrupt-cache-entry"};
            const auto index = static_cast<std::size_t>(reason);
            return index < Names.size() ? Names[index] : nullptr;
        }

        [[nodiscard]] std::filesystem::path ArchivePath(const std::filesystem::path &root, const Sha256Digest &digest) {
            return root / "by-hash" / "sha256" / DigestHex(digest) / "archive.horopkg";
        }

        [[nodiscard]] std::filesystem::path LockPath(const std::filesystem::path &root, const Sha256Digest &digest) {
            return root / "locks" / (DigestHex(digest) + ".lock");
        }

        [[nodiscard]] Error StorageError(const Error &cause, const std::string_view operation) {
            return WrapError(IoFailure, cause, "Package cache failed to " + std::string{operation} + '.');
        }

        /** @brief Owns only an exclusively-created private cache staging entry; existing paths are never touched. */
        class CacheStage final {
            DurableFileSystem &files_;
            std::filesystem::path path_;
            bool created_{};

        public:
            CacheStage(DurableFileSystem &authority, const std::filesystem::path &root, const std::string_view identity)
                : files_(authority), path_(root / "staging" / std::format("{}.{}.tmp", identity, NextQuarantineSequence())) {}

            ~CacheStage() {
                if (created_) {
                    try {
                        // Unlink only; never chmod an unowned/hard-linked path.
                        if (files_.RemoveDurable(path_).HasError())
                            Log::Logger::WriteEmergency("packages.cache", Log::Level::Warn,
                                                        "Private cache staging cleanup failed; the active cache is unchanged.");
                    } catch (...) {
                        // Destruction must preserve the original publication failure across foreign filesystem callbacks.
                        Log::Logger::WriteEmergency("packages.cache", Log::Level::Warn,
                                                    "Private cache staging cleanup threw; the active cache is unchanged.");
                    }
                }
            }

            CacheStage(const CacheStage &) = delete;
            CacheStage &operator=(const CacheStage &) = delete;
            CacheStage(CacheStage &&) = delete;
            CacheStage &operator=(CacheStage &&) = delete;

            /** @brief Transfers the exclusively-created entry to the existing quarantine transaction, ending local cleanup ownership. */
            [[nodiscard]] std::filesystem::path ReleasePath() noexcept {
                created_ = false;
                return std::move(path_);
            }

            Result<void> Write(const std::span<const std::byte> bytes) {
                if (auto parent = files_.CreateDirectoryPath(path_.parent_path(), DirectoryCreationMode::Parents); parent.HasError())
                    return Result<void>::Failure(parent.ErrorValue());
                return files_.WritePrivateDurable(path_, bytes, created_,
                                                  std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
            }

            Result<void> Publish(const std::filesystem::path &destination) {
                auto published = files_.AtomicReplace(path_, destination);
                if (published.HasValue())
                    created_ = false;
                return published;
            }
        };

        [[nodiscard]] Result<ExclusiveFileLock> Acquire(DurableFileSystem &files, const std::filesystem::path &root,
                                                        const Sha256Digest &digest) {
            auto lock = files.TryAcquireExclusive(LockPath(root, digest), "horo-package-cache");
            if (lock.HasError())
                return Result<ExclusiveFileLock>::Failure(WrapError(Busy, lock.ErrorValue()));
            return lock;
        }

        [[nodiscard]] Result<std::vector<std::byte>> ReadBounded(const std::filesystem::path &path, const std::uint64_t limit) {
            std::error_code error;
            if (const auto status = std::filesystem::symlink_status(path, error);
                error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
                return Result<std::vector<std::byte>>::Failure(MakeError(IoFailure, "Package cache entry is not a regular file."));
            }
            const std::uintmax_t size = std::filesystem::file_size(path, error);
            if (error)
                return Result<std::vector<std::byte>>::Failure(MakeError(IoFailure, "Package cache entry size could not be read."));
            if (const auto readableLimit =
                    std::min<std::uintmax_t>({limit, std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::streamsize>::max()});
                size > readableLimit) {
                return Result<std::vector<std::byte>>::Failure(MakeError(ResourceLimit));
            }
            std::vector<std::byte> bytes(static_cast<std::size_t>(size));
            auto *const byteStorage =
                reinterpret_cast<char *>(bytes.data());  // NOSONAR: the standard stream byte API requires char storage.
            if (std::ifstream stream(path, std::ios::binary);
                !stream || (size != 0 && !stream.read(byteStorage, static_cast<std::streamsize>(size)))) {
                return Result<std::vector<std::byte>>::Failure(MakeError(IoFailure, "Package cache entry could not be read completely."));
            }
            return Result<std::vector<std::byte>>::Success(std::move(bytes));
        }

        [[nodiscard]] Result<ValidatedPackageArchive> VerifyExpected(std::span<const std::byte> bytes, const Sha256Digest &expected,
                                                                     const PackageValidationLimits &limits) {
            auto archive = ValidatedPackageArchive::Verify(bytes, limits);
            if (archive.HasError())
                return archive;
            if (archive.Value().Digest() != expected)
                return Result<ValidatedPackageArchive>::Failure(MakeError(Corrupt));
            return archive;
        }

        [[nodiscard]] Result<void> MakeReadOnly(const std::filesystem::path &path) {
            std::error_code error;
            std::filesystem::permissions(path, std::filesystem::perms::owner_read, std::filesystem::perm_options::replace, error);
            return error ? Result<void>::Failure(MakeError(IoFailure, "Package cache could not restrict artifact permissions."))
                         : Result<void>::Success();
        }

        [[nodiscard]] Result<void> MakeOwnerWritable(const std::filesystem::path &path) {
            std::error_code error;
            std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add, error);
            return error ? Result<void>::Failure(MakeError(IoFailure, "Package cache could not prepare an entry for cleanup."))
                         : Result<void>::Success();
        }

        void BestEffortRemove(DurableFileSystem &files, const std::filesystem::path &path) {
            static_cast<void>(files.RemoveDurable(path));  // Remove only the owned entry; never mutate linked-file permissions.
        }

        [[nodiscard]] std::filesystem::path EnvelopePath(const std::filesystem::path &root, const Sha256Digest &digest) {
            return ArchivePath(root, digest).parent_path() / "publisher-envelope.bin";
        }

        /** @brief Rejects redirected cache ancestors and hostile final entries before evidence I/O. */
        [[nodiscard]] Result<void> AdmitEnvelopePath(const std::filesystem::path &root, const std::filesystem::path &path) {
            if (std::error_code rootError; std::filesystem::weakly_canonical(root, rootError) != root || rootError)
                return Result<void>::Failure(MakeError(IoFailure, "Package publisher evidence requires its exact host-owned cache root."));
            auto current = root;
            for (const auto &part : path.lexically_relative(root)) {
                current /= part;
                std::error_code error;
                const auto status = std::filesystem::symlink_status(current, error);
                if (error == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found)
                    return Result<void>::Success();
                if (error || std::filesystem::is_symlink(status) ||
                    (current == path ? !std::filesystem::is_regular_file(status) : !std::filesystem::is_directory(status)))
                    return Result<void>::Failure(
                        MakeError(IoFailure, "Package publisher evidence rejected a redirected or non-regular location."));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> RemoveEnvelope(DurableFileSystem &files, const std::filesystem::path &root, const Sha256Digest &digest) {
            const auto path = EnvelopePath(root, digest);
            std::error_code error;
            if (const auto status = std::filesystem::symlink_status(path, error);
                error == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found)
                return Result<void>::Success();  // Legacy archive-only cleanup keeps its existing behavior.
            if (error)
                return Result<void>::Failure(MakeError(IoFailure));
            if (auto admitted = AdmitEnvelopePath(root, path); admitted.HasError())
                return admitted;
            return files.RemoveDurable(path);  // Private evidence stays owner-writable; cleanup never chmods linked victims.
        }

        /** @brief Publishes inert detached bytes while the caller retains the archive's existing digest lock. */
        [[nodiscard]] Result<void> PublishEnvelope(DurableFileSystem &files, const std::filesystem::path &root, const Sha256Digest &digest,
                                                   const std::span<const std::byte> bytes) {
            const auto destination = EnvelopePath(root, digest);
            CacheStage staging{files, root, DigestHex(digest) + ".publisher"};
            if (auto admitted = AdmitEnvelopePath(root, destination); admitted.HasError())
                return admitted;
            if (auto write = staging.Write(bytes); write.HasError())
                return Result<void>::Failure(StorageError(write.ErrorValue(), "stage publisher evidence"));
            if (auto publish = staging.Publish(destination); publish.HasError())
                return Result<void>::Failure(StorageError(publish.ErrorValue(), "publish publisher evidence"));
            auto stored = ReadBounded(destination, CacheDetail::MaximumPublisherEnvelopeBytes);
            if (stored.HasError())
                return Result<void>::Failure(stored.ErrorValue());
            if (!std::ranges::equal(stored.Value(), bytes))
                return Result<void>::Failure(MakeError(Corrupt, "Published publisher evidence differs from the staged bytes."));
            return Result<void>::Success();
        }

        [[nodiscard]] std::string QuarantineJson(const PackageQuarantineRecord &record) {
            nlohmann::json document{{"schemaVersion", 1},
                                    {"quarantineId", record.quarantineId},
                                    {"reason", ReasonName(record.reason)},
                                    {"actualDigest", FormatSha256(record.actualDigest)},
                                    {"byteSize", record.byteSize}};
            document["expectedDigest"] =
                record.expectedDigest.has_value() ? nlohmann::json(FormatSha256(*record.expectedDigest)) : nlohmann::json(nullptr);
            return document.dump(2) + '\n';
        }

        [[nodiscard]] Result<std::string> AvailableQuarantineId(const std::filesystem::path &root, const char *reason,
                                                                const Sha256Digest &identity) {
            const std::string prefix = DigestHex(identity) + '-';
            constexpr std::uint32_t Attempts = 1024;
            for (std::uint32_t attempt = 0; attempt < Attempts; ++attempt) {
                const std::string candidate = std::format("{}{}", prefix, NextQuarantineSequence());
                std::error_code error;
                const bool exists = std::filesystem::exists(root / "quarantine" / reason / candidate, error);
                if (error)
                    return Result<std::string>::Failure(MakeError(IoFailure, "Package quarantine destination could not be inspected."));
                if (!exists)
                    return Result<std::string>::Success(candidate);
            }
            return Result<std::string>::Failure(MakeError(IoFailure, "Package quarantine identity capacity was exhausted."));
        }

        /** @brief Publishes the inert diagnostic before its quarantined artifact can become visible. */
        [[nodiscard]] Result<void> PublishQuarantineDiagnostic(DurableFileSystem &files, const std::filesystem::path &root,
                                                               const std::filesystem::path &diagnostic,
                                                               const PackageQuarantineRecord &record) {
            CacheStage staging{files, root, record.quarantineId + ".diagnostic"};
            const std::string json = QuarantineJson(record);
            if (auto write = staging.Write(std::as_bytes(std::span{json})); write.HasError())
                return Result<void>::Failure(StorageError(write.ErrorValue(), "write quarantine diagnostics"));
            if (auto publish = staging.Publish(diagnostic); publish.HasError())
                return Result<void>::Failure(StorageError(publish.ErrorValue(), "publish quarantine diagnostics"));
            return Result<void>::Success();
        }

        /** @brief Selects an existing corrupt entry or durably stages caller-provided failed bytes. */
        [[nodiscard]] Result<std::filesystem::path> PrepareQuarantineArtifact(DurableFileSystem &files, const std::filesystem::path &root,
                                                                              const std::string_view quarantineId,
                                                                              const std::span<const std::byte> bytes,
                                                                              const std::optional<std::filesystem::path> &existingPath) {
            if (existingPath.has_value())
                return Result<std::filesystem::path>::Success(*existingPath);
            CacheStage staging{files, root, std::string{quarantineId} + ".artifact"};
            if (auto write = staging.Write(bytes); write.HasError())
                return Result<std::filesystem::path>::Failure(StorageError(write.ErrorValue(), "write quarantined bytes"));
            return Result<std::filesystem::path>::Success(staging.ReleasePath());
        }

        [[nodiscard]] Result<PackageQuarantineRecord> QuarantineLocked(
            DurableFileSystem &files, const std::filesystem::path &root, std::span<const std::byte> bytes,
            const PackageQuarantineReason reason, const std::optional<Sha256Digest> &expectedDigest,
            const std::optional<std::filesystem::path> &existingPath = std::nullopt) {
            const char *reasonName = ReasonName(reason);
            const Sha256Digest actualDigest = ComputeSha256(bytes);
            const Sha256Digest identity = expectedDigest.value_or(actualDigest);
            auto quarantineId = AvailableQuarantineId(root, reasonName, identity);
            if (quarantineId.HasError())
                return Result<PackageQuarantineRecord>::Failure(quarantineId.ErrorValue());
            PackageQuarantineRecord record{.quarantineId = std::move(quarantineId).Value(),
                                           .reason = reason,
                                           .expectedDigest = expectedDigest,
                                           .actualDigest = actualDigest,
                                           .byteSize = bytes.size()};
            const std::filesystem::path destination = root / "quarantine" / reasonName / record.quarantineId;
            const std::filesystem::path diagnostic = destination / "diagnostic.json";
            if (auto publish = PublishQuarantineDiagnostic(files, root, diagnostic, record); publish.HasError())
                return Result<PackageQuarantineRecord>::Failure(publish.ErrorValue());
            auto prepared = PrepareQuarantineArtifact(files, root, record.quarantineId, bytes, existingPath);
            if (prepared.HasError()) {
                BestEffortRemove(files, diagnostic);
                return Result<PackageQuarantineRecord>::Failure(prepared.ErrorValue());
            }
            if (auto publish = files.AtomicReplace(prepared.Value(), destination / "artifact.horopkg"); publish.HasError()) {
                if (!existingPath.has_value())
                    BestEffortRemove(files, prepared.Value());
                BestEffortRemove(files, diagnostic);
                return Result<PackageQuarantineRecord>::Failure(StorageError(publish.ErrorValue(), "isolate quarantined bytes"));
            }
            if (!existingPath.has_value())
                if (auto permissions = MakeReadOnly(destination / "artifact.horopkg"); permissions.HasError())
                    return Result<PackageQuarantineRecord>::Failure(permissions.ErrorValue());
            // Moving an untrusted existing entry must not chmod another name of a hard-linked victim.
            if (existingPath.has_value())
                if (auto removed = RemoveEnvelope(files, root, identity); removed.HasError())
                    return Result<PackageQuarantineRecord>::Failure(removed.ErrorValue());
            return Result<PackageQuarantineRecord>::Success(std::move(record));
        }
    }  // namespace

    /** @copydoc PackageCacheStore::Create */
    Result<PackageCacheStore> PackageCacheStore::Create(DurableFileSystem &files, const std::filesystem::path &root,
                                                        const PackageValidationLimits &limits) {
        if (root.empty() || !root.is_absolute() || root != root.lexically_normal() || limits.archiveBytes == 0 ||
            limits.manifestBytes == 0 || limits.fileBytes == 0 || limits.expandedBytes == 0 || limits.entries == 0)
            return Result<PackageCacheStore>::Failure(MakeError(InvalidConfiguration));
        return Result<PackageCacheStore>::Success(PackageCacheStore{files, root, limits});
    }

    /** @copydoc PackageCacheStore::PackageCacheStore */
    PackageCacheStore::PackageCacheStore(DurableFileSystem &files, std::filesystem::path root,
                                         const PackageValidationLimits &limits) noexcept
        : files_(files), root_(std::move(root)), limits_(limits) {}

    /** @copydoc PackageCacheStore::Publish(const ValidatedPackageArchive&) */
    Result<PackageCacheEntry> PackageCacheStore::Publish(const ValidatedPackageArchive &archive) {
        auto lock = Acquire(files_, root_, archive.Digest());  // NOSONAR
        if (lock.HasError())
            return Result<PackageCacheEntry>::Failure(lock.ErrorValue());
        return PublishLocked(archive);
    }

    /** @copydoc PackageCacheStore::PublishLocked */
    Result<PackageCacheEntry> PackageCacheStore::PublishLocked(const ValidatedPackageArchive &archive) {
        const Sha256Digest digest = archive.Digest();
        if (auto compatible = VerifyExpected(archive.Bytes(), digest, limits_); compatible.HasError())
            return Result<PackageCacheEntry>::Failure(compatible.ErrorValue());
        const std::filesystem::path destination = ArchivePath(root_, digest);
        if (std::error_code existsError; std::filesystem::exists(destination, existsError)) {
            auto bytes = ReadBounded(destination, limits_.archiveBytes);
            if (bytes.HasError())
                return Result<PackageCacheEntry>::Failure(bytes.ErrorValue());
            if (auto existing = VerifyExpected(bytes.Value(), digest, limits_); existing.HasValue())
                return Result<PackageCacheEntry>::Success({digest, bytes.Value().size(), true});
            auto quarantined =
                QuarantineLocked(files_, root_, bytes.Value(), PackageQuarantineReason::CorruptCacheEntry, digest, destination);
            if (quarantined.HasError())
                return Result<PackageCacheEntry>::Failure(WrapError(Corrupt, quarantined.ErrorValue()));
        } else if (existsError) {
            return Result<PackageCacheEntry>::Failure(MakeError(IoFailure, "Package cache destination could not be inspected."));
        }

        CacheStage staging{files_, root_, DigestHex(digest) + ".archive"};
        if (auto write = staging.Write(archive.Bytes()); write.HasError())
            return Result<PackageCacheEntry>::Failure(StorageError(write.ErrorValue(), "write a staged archive"));
        if (auto publish = staging.Publish(destination); publish.HasError())
            return Result<PackageCacheEntry>::Failure(StorageError(publish.ErrorValue(), "publish an archive atomically"));
        if (auto permissions = MakeReadOnly(destination); permissions.HasError())
            return Result<PackageCacheEntry>::Failure(permissions.ErrorValue());
        auto publishedBytes = ReadBounded(destination, limits_.archiveBytes);
        if (publishedBytes.HasError())
            return Result<PackageCacheEntry>::Failure(publishedBytes.ErrorValue());
        if (auto verified = VerifyExpected(publishedBytes.Value(), digest, limits_); verified.HasError()) {
            auto quarantined =
                QuarantineLocked(files_, root_, publishedBytes.Value(), PackageQuarantineReason::CorruptCacheEntry, digest, destination);
            return Result<PackageCacheEntry>::Failure(quarantined.HasError() ? WrapError(Corrupt, quarantined.ErrorValue())
                                                                             : MakeError(Corrupt));
        }
        return Result<PackageCacheEntry>::Success({digest, publishedBytes.Value().size(), false});
    }

    /** @copydoc PackageCacheStore::Load */
    Result<std::optional<ValidatedPackageArchive>> PackageCacheStore::Load(const Sha256Digest &digest) {
        // The lock must span the complete load and quarantine transaction.
        auto lock = Acquire(files_, root_, digest);  // NOSONAR
        if (lock.HasError())
            return Result<std::optional<ValidatedPackageArchive>>::Failure(lock.ErrorValue());
        return LoadLocked(digest);
    }

    /** @copydoc PackageCacheStore::LoadLocked */
    Result<std::optional<ValidatedPackageArchive>> PackageCacheStore::LoadLocked(const Sha256Digest &digest) {
        const std::filesystem::path path = ArchivePath(root_, digest);
        if (std::error_code existsError; !std::filesystem::exists(path, existsError)) {
            if (existsError)
                return Result<std::optional<ValidatedPackageArchive>>::Failure(MakeError(IoFailure));
            return Result<std::optional<ValidatedPackageArchive>>::Success(std::nullopt);
        }
        auto bytes = ReadBounded(path, limits_.archiveBytes);
        if (bytes.HasError())
            return Result<std::optional<ValidatedPackageArchive>>::Failure(bytes.ErrorValue());
        if (auto archive = VerifyExpected(bytes.Value(), digest, limits_); archive.HasValue())
            return Result<std::optional<ValidatedPackageArchive>>::Success(
                std::optional<ValidatedPackageArchive>{std::move(archive).Value()});
        auto quarantined = QuarantineLocked(files_, root_, bytes.Value(), PackageQuarantineReason::CorruptCacheEntry, digest, path);
        return Result<std::optional<ValidatedPackageArchive>>::Failure(quarantined.HasError() ? WrapError(Corrupt, quarantined.ErrorValue())
                                                                                              : MakeError(Corrupt));
    }

    /** @copydoc PackageCacheStore::Publish(const ValidatedPackageArchive&, const Security::DetachedSignatureEnvelope&) */
    Result<PackageCacheEntry> PackageCacheStore::Publish(const ValidatedPackageArchive &archive,
                                                         const Security::DetachedSignatureEnvelope &signature) {
        auto encoded = CacheDetail::EncodePublisherEnvelope(signature, archive.Digest());
        if (encoded.HasError())
            return Result<PackageCacheEntry>::Failure(encoded.ErrorValue());
        if (auto lock = Acquire(files_, root_, archive.Digest()); lock.HasError()) {
            return Result<PackageCacheEntry>::Failure(lock.ErrorValue());
        } else {
            // The if-init lease spans this complete archive/evidence publication transaction.
            if (auto admitted = AdmitEnvelopePath(root_, EnvelopePath(root_, archive.Digest())); admitted.HasError())
                return Result<PackageCacheEntry>::Failure(admitted.ErrorValue());
            auto published = PublishLocked(archive);
            if (published.HasError())
                return published;
            if (auto evidence = PublishEnvelope(files_, root_, archive.Digest(), encoded.Value()); evidence.HasError())
                return Result<PackageCacheEntry>::Failure(evidence.ErrorValue());
            return published;
        }
    }

    /** @copydoc PackageCacheStore::LoadWithPublisherEvidence */
    Result<std::optional<PackageCachedArtifact>> PackageCacheStore::LoadWithPublisherEvidence(const Sha256Digest &digest) {
        using Outcome = Result<std::optional<PackageCachedArtifact>>;
        if (auto lock = Acquire(files_, root_, digest); lock.HasError()) {
            return Outcome::Failure(lock.ErrorValue());
        } else {
            // The exact digest lease protects both bytes and detached evidence through the final owning result.
            if (auto admitted = AdmitEnvelopePath(root_, EnvelopePath(root_, digest)); admitted.HasError())
                return Outcome::Failure(admitted.ErrorValue());
            auto loaded = LoadLocked(digest);
            if (loaded.HasError())
                return Outcome::Failure(loaded.ErrorValue());
            auto archive = std::move(loaded).Value();
            if (!archive)
                return Outcome::Success(std::nullopt);
            PackageCachedArtifact artifact{std::move(*archive), std::nullopt};
            const auto path = EnvelopePath(root_, digest);
            if (auto admitted = AdmitEnvelopePath(root_, path); admitted.HasError())
                return Outcome::Failure(admitted.ErrorValue());
            if (std::error_code error; !std::filesystem::exists(path, error))
                return error ? Outcome::Failure(MakeError(IoFailure)) : Outcome::Success(std::move(artifact));
            auto bytes = ReadBounded(path, CacheDetail::MaximumPublisherEnvelopeBytes);
            if (bytes.HasError())
                return Outcome::Failure(bytes.ErrorValue());
            auto decoded = CacheDetail::DecodePublisherEnvelope(bytes.Value(), digest);
            if (decoded.HasError())
                return Outcome::Failure(WrapError(Corrupt, decoded.ErrorValue()));
            artifact.signature = std::move(decoded).Value();
            return Outcome::Success(std::move(artifact));
        }
    }

    /** @copydoc PackageCacheStore::Quarantine */
    Result<PackageQuarantineRecord> PackageCacheStore::Quarantine(const std::span<const std::byte> bytes,
                                                                  const PackageQuarantineReason reason,
                                                                  const std::optional<Sha256Digest> &expectedDigest) {
        if (bytes.size() > limits_.archiveBytes)
            return Result<PackageQuarantineRecord>::Failure(MakeError(ResourceLimit));
        if (ReasonName(reason) == nullptr)
            return Result<PackageQuarantineRecord>::Failure(MakeError(InvalidReason));
        const Sha256Digest identity = expectedDigest.value_or(ComputeSha256(bytes));
        // The lock must span the complete quarantine transaction.
        auto lock = Acquire(files_, root_, identity);  // NOSONAR
        if (lock.HasError())
            return Result<PackageQuarantineRecord>::Failure(lock.ErrorValue());
        return QuarantineLocked(files_, root_, bytes, reason, expectedDigest);
    }

    /** @copydoc PackageCacheStore::Remove */
    Result<bool> PackageCacheStore::Remove(const Sha256Digest &digest) {
        // The lock must span the complete removal transaction.
        auto lock = Acquire(files_, root_, digest);  // NOSONAR
        if (lock.HasError())
            return Result<bool>::Failure(lock.ErrorValue());
        if (auto removed = RemoveEnvelope(files_, root_, digest); removed.HasError())
            return Result<bool>::Failure(removed.ErrorValue());
        const std::filesystem::path path = ArchivePath(root_, digest);
        std::error_code statusError;
        const auto status = std::filesystem::symlink_status(path, statusError);
        if (statusError == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found)
            return Result<bool>::Success(false);
        if (statusError)
            return Result<bool>::Failure(MakeError(IoFailure));
        if (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status))
            return Result<bool>::Failure(MakeError(IoFailure, "Package cache cleanup rejected a non-regular entry."));
        if (auto permissions = MakeOwnerWritable(path); permissions.HasError())
            return Result<bool>::Failure(permissions.ErrorValue());
        if (auto remove = files_.RemoveDurable(path); remove.HasError())
            return Result<bool>::Failure(StorageError(remove.ErrorValue(), "remove an archive"));
        return Result<bool>::Success(true);
    }
}  // namespace Horo::Packages
