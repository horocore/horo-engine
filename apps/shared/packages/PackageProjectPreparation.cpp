#include "Horo/Packages/PackageRequest.h"
#include "Horo/Packages/PackageRestoreErrors.h"
#include "PackageProjectSession.h"

#include <algorithm>
#include <fstream>

namespace Horo::Application::Internal {
    namespace {
        constexpr auto RequestLimit = 1024U * 1024U;
        constexpr std::string_view JournalMagic = "HOROPMJ1";
        constexpr auto DigestSize = Sha256Digest{}.bytes.size();
        constexpr auto JournalFields = JournalMagic.size() + 3 * DigestSize;
        constexpr auto JournalOverhead = JournalFields + DigestSize;

        Error MetadataConflict() {
            return Error{ErrorCode{"packages.host.metadata_conflict"}, ErrorDomainId{"packages.host"}};
        }

        std::filesystem::path JournalPath(const std::filesystem::path &metadata) {
            return metadata / "local/package-metadata.pending";
        }

        std::span<const std::byte> Bytes(const std::string_view bytes) {
            return std::as_bytes(std::span{bytes});
        }

        Sha256Digest Digest(const std::string_view bytes) {
            return ComputeSha256(Bytes(bytes));
        }

        /** @brief Checks that project metadata belongs to the exact host-selected directory. */
        Result<std::filesystem::path> MetadataPath(const std::filesystem::path &path) {
            std::error_code error;
            const auto root = std::filesystem::canonical(path, error);
            if (error || !path.is_absolute() || root != path.lexically_normal())
                return Result<std::filesystem::path>::Failure(MakeError(Packages::PackageRestoreErrors::InvalidInput));
            const auto metadata = root / ".horo";
            if (const auto status = std::filesystem::symlink_status(metadata, error); error || !std::filesystem::is_directory(status))
                return Result<std::filesystem::path>::Failure(MakeError(Packages::PackageRestoreErrors::InvalidInput));
            return Result<std::filesystem::path>::Success(metadata);
        }

        /** @brief Reads one regular bounded metadata document on an explicit host I/O lane. */
        Result<std::string> ReadMetadata(const std::filesystem::path &path, const std::size_t limit,
                                         const CancellationToken &cancellation) {
            std::error_code error;
            if (const auto status = std::filesystem::symlink_status(path, error); error || !std::filesystem::is_regular_file(status))
                return Result<std::string>::Failure(MakeError(Packages::PackageRestoreErrors::LockfileReadFailed));
            const auto size = std::filesystem::file_size(path, error);
            if (error || size > limit)
                return Result<std::string>::Failure(MakeError(Packages::PackageRestoreErrors::ResourceLimit));
            std::ifstream input{path, std::ios::binary};
            if (!input)
                return Result<std::string>::Failure(MakeError(Packages::PackageRestoreErrors::LockfileReadFailed));
            std::string bytes(static_cast<std::size_t>(size), '\0');
            for (std::size_t offset = 0; offset < bytes.size();) {
                if (cancellation.IsCancellationRequested())
                    return Result<std::string>::Failure(MakeError(Packages::PackageRestoreErrors::Cancelled));
                const auto count = std::min<std::size_t>(64 * 1024, bytes.size() - offset);
                input.read(bytes.data() + offset, static_cast<std::streamsize>(count));
                if (input.gcount() != static_cast<std::streamsize>(count))
                    return Result<std::string>::Failure(MakeError(Packages::PackageRestoreErrors::LockfileReadFailed));
                offset += count;
            }
            if (input.peek() != std::char_traits<char>::eof())
                return Result<std::string>::Failure(MakeError(Packages::PackageRestoreErrors::ResourceLimit));
            return Result<std::string>::Success(std::move(bytes));
        }

        /** @brief The request's old bytes and the three identities needed to recognize every publication phase. */
        struct MetadataJournal final {
            Sha256Digest oldLock;
            Sha256Digest newLock;
            Sha256Digest newRequest;
            std::string_view oldRequest;
        };

        void AppendDigest(std::string &output, const Sha256Digest &digest) {
            output.append(reinterpret_cast<const char *>(digest.bytes.data()), DigestSize);
        }

        Sha256Digest DecodeDigest(const std::string_view bytes) {
            Sha256Digest digest;
            std::ranges::copy(Bytes(bytes), std::as_writable_bytes(std::span{digest.bytes}).begin());
            return digest;
        }

        /** @brief Encodes a versioned journal with bounded undo bytes and a corruption check. */
        std::string EncodeJournal(const MetadataJournal &journal) {
            std::string bytes{JournalMagic};
            AppendDigest(bytes, journal.oldLock);
            AppendDigest(bytes, journal.newLock);
            AppendDigest(bytes, journal.newRequest);
            bytes.append(journal.oldRequest);
            AppendDigest(bytes, Digest(bytes));
            return bytes;
        }

        std::optional<MetadataJournal> DecodeJournal(const std::string_view bytes) {
            if (bytes.size() <= JournalOverhead || bytes.size() > RequestLimit + JournalOverhead || !bytes.starts_with(JournalMagic) ||
                DecodeDigest(bytes.substr(bytes.size() - DigestSize)) != Digest(bytes.substr(0, bytes.size() - DigestSize)))
                return std::nullopt;
            const auto fields = bytes.substr(JournalMagic.size(), 3 * DigestSize);
            return MetadataJournal{DecodeDigest(fields.substr(0, DigestSize)), DecodeDigest(fields.substr(DigestSize, DigestSize)),
                                   DecodeDigest(fields.substr(2 * DigestSize)),
                                   bytes.substr(JournalFields, bytes.size() - JournalOverhead)};
        }

        /** @brief Uses a private sibling under the held metadata lease; leftover staging is never accepted as published data. */
        Result<void> ReplaceMetadata(const std::filesystem::path &path, const std::string_view bytes, DurableFileSystem &files,
                                     AtomicFileReplacementReceipt &receipt) {
            auto prepared = path;
            prepared += ".metadata-tmp";
            const auto previous = files.InspectEntry(prepared);
            if (previous.HasError())
                return Result<void>::Failure(previous.ErrorValue());
            if (const auto &entry = previous.Value(); entry) {
                if (entry->kind != FileEntryKind::RegularFile || entry->hardLinks != 1)
                    return Result<void>::Failure(MetadataConflict());
                if (auto removed = files.RemoveDurable(prepared); removed.HasError())
                    return removed;
            }
            bool created{};
            if (auto written = files.WritePrivateDurable(prepared, Bytes(bytes), created); written.HasError()) {
                if (created)
                    static_cast<void>(files.RemoveDurable(prepared));
                return written;
            }
            auto published = files.AtomicReplaceTracked(prepared, path, receipt);
            if (published.HasError() && !receipt.WasCommitted())
                static_cast<void>(files.RemoveDurable(prepared));
            return published;
        }

        /** @brief Detects a journal without creating directories or locks during ordinary read-only project admission. */
        Result<bool> HasJournal(const std::filesystem::path &path, DurableFileSystem &files) {
            const auto entry = files.InspectEntry(path);
            if (entry.HasError())
                return Result<bool>::Failure(entry.ErrorValue());
            if (entry.Value() && (entry.Value()->kind != FileEntryKind::RegularFile || entry.Value()->hardLinks != 1))
                return Result<bool>::Failure(MetadataConflict());
            return Result<bool>::Success(entry.Value().has_value());
        }

        Result<ExclusiveFileLock> AcquireMetadata(const std::filesystem::path &journal, DurableFileSystem &files) {
            return files.TryAcquireExclusive(journal.parent_path() / "package-metadata.lock", "horo.package-metadata");
        }

        /** @brief Repairs only an exact interrupted publication. A changed or malformed pair is left intact for review. */
        Result<void> RecoverMetadata(const std::filesystem::path &metadata, const Packages::PackageLockfileLimits &limits,
                                     DurableFileSystem &files, const CancellationToken &cancellation) {
            const auto path = JournalPath(metadata);
            auto pending = HasJournal(path, files);
            if (pending.HasError())
                return Result<void>::Failure(pending.ErrorValue());
            if (!pending.Value())
                return Result<void>::Success();
            const auto bytes = ReadMetadata(path, RequestLimit + JournalOverhead, cancellation);
            if (bytes.HasError())
                return Result<void>::Failure(bytes.ErrorValue());
            const auto journal = DecodeJournal(bytes.Value());
            if (!journal)
                return Result<void>::Failure(MetadataConflict());
            const auto request = ReadMetadata(metadata / "packages.json", RequestLimit, cancellation);
            if (request.HasError())
                return Result<void>::Failure(request.ErrorValue());
            const auto lock = ReadMetadata(metadata / "packages.lock", limits.documentBytes, cancellation);
            if (lock.HasError())
                return Result<void>::Failure(lock.ErrorValue());

            const auto lockDigest = Digest(lock.Value());
            const bool committed = lockDigest == journal->newLock;
            if (committed ? Digest(request.Value()) != journal->newRequest
                          : lockDigest != journal->oldLock ||
                                (request.Value() != journal->oldRequest && Digest(request.Value()) != journal->newRequest))
                return Result<void>::Failure(MetadataConflict());

            const auto restoredRequest = Packages::ValidatedPackageRequest::Parse(committed ? request.Value() : journal->oldRequest);
            const auto restoredLock = Packages::ValidatedPackageLockfileV1::Parse(lock.Value(), limits);
            if (restoredRequest.HasError() || restoredLock.HasError() ||
                restoredRequest.Value().ValidateLock(restoredLock.Value()).HasError())
                return Result<void>::Failure(MetadataConflict());
            if (!committed && request.Value() != journal->oldRequest) {
                AtomicFileReplacementReceipt receipt;
                if (auto restored = ReplaceMetadata(metadata / "packages.json", journal->oldRequest, files, receipt); restored.HasError())
                    return restored;
            }
            // A failed durability confirmation preserves the journal for the next attempt.
            if (auto synced = files.SyncDirectory(metadata); synced.HasError())
                return synced;
            return files.RemoveDurable(path);
        }

        /** @brief Takes the write lease only when a journal is present. */
        Result<void> RecoverInterruptedPublication(const std::filesystem::path &metadata, const Packages::PackageLockfileLimits &limits,
                                                   DurableFileSystem &files, const CancellationToken &cancellation) {
            const auto path = JournalPath(metadata);
            const auto pending = HasJournal(path, files);
            if (pending.HasError())
                return Result<void>::Failure(pending.ErrorValue());
            if (!pending.Value())
                return Result<void>::Success();
            const auto lease = AcquireMetadata(path, files);
            if (lease.HasError())
                return Result<void>::Failure(lease.ErrorValue());
            return RecoverMetadata(metadata, limits, files, cancellation);
        }
    }  // namespace

    /** @copydoc PreparePackageProject */
    Result<PreparedPackageProject> PreparePackageProject(const PackageProjectPolicy &policy, DurableFileSystem &files,
                                                         const CancellationToken &cancellation) {
        const auto directory = MetadataPath(policy.projectRoot);
        if (directory.HasError())
            return Result<PreparedPackageProject>::Failure(directory.ErrorValue());
        const auto &metadata = directory.Value();
        if (auto recovered = RecoverInterruptedPublication(metadata, policy.limits.lockfile, files, cancellation); recovered.HasError())
            return Result<PreparedPackageProject>::Failure(recovered.ErrorValue());
        auto requestBytes = ReadMetadata(metadata / "packages.json", RequestLimit, cancellation);
        if (requestBytes.HasError())
            return Result<PreparedPackageProject>::Failure(requestBytes.ErrorValue());
        auto request = Packages::ValidatedPackageRequest::Parse(requestBytes.Value());
        if (request.HasError())
            return Result<PreparedPackageProject>::Failure(request.ErrorValue());
        auto lockBytes = ReadMetadata(metadata / "packages.lock", policy.limits.lockfile.documentBytes, cancellation);
        if (lockBytes.HasError())
            return Result<PreparedPackageProject>::Failure(lockBytes.ErrorValue());
        auto lock = Packages::ValidatedPackageLockfileV1::Parse(lockBytes.Value(), policy.limits.lockfile);
        if (lock.HasError())
            return Result<PreparedPackageProject>::Failure(lock.ErrorValue());
        if (const auto matched = request.Value().ValidateLock(lock.Value()); matched.HasError())
            return Result<PreparedPackageProject>::Failure(matched.ErrorValue());
        if (const auto valid =
                lock.Value().ValidateForRestore(request.Value().Digest(), policy.platform, policy.limits.supportedPackageFormatVersion);
            valid.HasError())
            return Result<PreparedPackageProject>::Failure(valid.ErrorValue());
        // Re-read bounded intent after lock capture. A concurrent metadata replacement cannot silently bind another current request.
        const auto recheck = ReadMetadata(metadata / "packages.json", RequestLimit, cancellation);
        if (recheck.HasError())
            return Result<PreparedPackageProject>::Failure(recheck.ErrorValue());
        if (recheck.Value() != requestBytes.Value())
            return Result<PreparedPackageProject>::Failure(MakeError(Packages::PackageRestoreErrors::InvalidInput));
        const auto pending = HasJournal(JournalPath(metadata), files);
        if (pending.HasError())
            return Result<PreparedPackageProject>::Failure(pending.ErrorValue());
        if (pending.Value())
            return Result<PreparedPackageProject>::Failure(MakeError(Packages::PackageRestoreErrors::Busy));
        PreparedPackageProject prepared;
        prepared.application.restore = {.projectRoot = metadata.parent_path(),
                                        .lockfileJson = std::move(lockBytes).Value(),
                                        .expectedRequestHash = request.Value().Digest(),
                                        .platform = policy.platform,
                                        .mode = policy.mode,
                                        .requirePublisherVerification = policy.requirePublisherVerification};
        prepared.application.lockedPlan = std::move(lock).Value();
        prepared.application.portableRequest = std::move(request).Value();
        prepared.lockDigest = Digest(prepared.application.restore.lockfileJson);
        prepared.requestDocumentDigest = Digest(requestBytes.Value());
        return Result<PreparedPackageProject>::Success(std::move(prepared));
    }

    /** @copydoc PublishPackageProjectRemoval */
    Result<PackageMetadataPublication> PublishPackageProjectRemoval(const PackageProjectPolicy &policy,
                                                                    const PreparedPackageProject &expected,
                                                                    const Packages::HoroPackageId &package, DurableFileSystem &files) {
        using Publication = Result<PackageMetadataPublication>;
        const auto directory = MetadataPath(policy.projectRoot);
        if (directory.HasError())
            return Publication::Failure(directory.ErrorValue());
        const auto &metadata = directory.Value();
        if (metadata.parent_path() != expected.application.restore.projectRoot || !expected.application.portableRequest ||
            !expected.application.lockedPlan)
            return Publication::Failure(MetadataConflict());
        const auto journalPath = JournalPath(metadata);
        const auto lease = AcquireMetadata(journalPath, files);
        if (lease.HasError())
            return Publication::Failure(lease.ErrorValue());
        if (auto recovered = RecoverMetadata(metadata, policy.limits.lockfile, files, {}); recovered.HasError())
            return Publication::Failure(recovered.ErrorValue());
        const auto requestPath = metadata / "packages.json";
        const auto lockPath = metadata / "packages.lock";
        const auto beforeRequest = ReadMetadata(requestPath, RequestLimit, {});
        if (beforeRequest.HasError())
            return Publication::Failure(beforeRequest.ErrorValue());
        const auto beforeLock = ReadMetadata(lockPath, policy.limits.lockfile.documentBytes, {});
        if (beforeLock.HasError())
            return Publication::Failure(beforeLock.ErrorValue());
        if (Digest(beforeRequest.Value()) != expected.requestDocumentDigest || Digest(beforeLock.Value()) != expected.lockDigest)
            return Publication::Failure(MetadataConflict());
        const auto plan =
            Packages::PackageRemovalPlan::Prepare(*expected.application.portableRequest, *expected.application.lockedPlan, package);
        if (plan.HasError())
            return Publication::Failure(plan.ErrorValue());
        const auto &nextRequest = plan.Value().request.SerializeCanonical();
        const auto nextLock = plan.Value().lock.SerializeCanonical();
        if (nextRequest.size() > RequestLimit || nextLock.size() > policy.limits.lockfile.documentBytes)
            return Publication::Failure(MakeError(Packages::PackageRestoreErrors::ResourceLimit));

        AtomicFileReplacementReceipt journalReceipt;
        const auto journal = EncodeJournal({expected.lockDigest, Digest(nextLock), Digest(nextRequest), beforeRequest.Value()});
        if (auto recorded = ReplaceMetadata(journalPath, journal, files, journalReceipt); recorded.HasError())
            return Publication::Failure(recorded.ErrorValue());
        AtomicFileReplacementReceipt requestReceipt;
        if (auto written = ReplaceMetadata(requestPath, nextRequest, files, requestReceipt); written.HasError())
            return Publication::Failure(written.ErrorValue());
        AtomicFileReplacementReceipt lockReceipt;
        if (auto written = ReplaceMetadata(lockPath, nextLock, files, lockReceipt); written.HasError())
            return lockReceipt.WasCommitted() ? Publication::Success({written.ErrorValue()}) : Publication::Failure(written.ErrorValue());
        if (auto cleared = files.RemoveDurable(journalPath); cleared.HasError())
            return Publication::Success({cleared.ErrorValue()});
        return Publication::Success({});
    }
}  // namespace Horo::Application::Internal
