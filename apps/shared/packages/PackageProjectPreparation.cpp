#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Packages/PackageLifecycle.h"
#include "Horo/Packages/PackageRequest.h"
#include "Horo/Packages/PackageRestoreErrors.h"
#include "PackageBehaviorReferences.h"
#include "PackageProjectSession.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>

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

        std::filesystem::path RollbackPath(const std::filesystem::path &metadata, const bool pending) {
            return metadata / (pending ? "local/package-update.rollback.pending" : "local/package-update.rollback");
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

        struct MetadataDocuments final {
            std::string request;
            std::string lock;

            bool Matches(const PreparedPackageProject &expected) const {
                return Digest(request) == expected.requestDocumentDigest && Digest(lock) == expected.lockDigest;
            }

            bool operator==(const MetadataDocuments &) const = default;

            bool Fits(const Packages::PackageLockfileLimits &limits) const {
                return request.size() <= RequestLimit && lock.size() <= limits.documentBytes;
            }
        };

        /** @brief Captures both documents under their independent byte limits. */
        Result<MetadataDocuments> ReadDocuments(const std::filesystem::path &metadata, const Packages::PackageLockfileLimits &limits,
                                                const CancellationToken &cancellation) {
            auto request = ReadMetadata(metadata / "packages.json", RequestLimit, cancellation);
            if (request.HasError())
                return Result<MetadataDocuments>::Failure(request.ErrorValue());
            auto lock = ReadMetadata(metadata / "packages.lock", limits.documentBytes, cancellation);
            if (lock.HasError())
                return Result<MetadataDocuments>::Failure(lock.ErrorValue());
            return Result<MetadataDocuments>::Success({std::move(request).Value(), std::move(lock).Value()});
        }

        struct ValidatedMetadata final {
            Packages::ValidatedPackageRequest request;
            Packages::ValidatedPackageLockfileV1 lock;
        };

        /** @brief Verifies that the lock describes the exact portable request. */
        Result<ValidatedMetadata> ValidateDocuments(const std::string_view requestBytes, const std::string_view lockBytes,
                                                    const Packages::PackageLockfileLimits &limits) {
            auto request = Packages::ValidatedPackageRequest::Parse(requestBytes);
            if (request.HasError())
                return Result<ValidatedMetadata>::Failure(request.ErrorValue());
            auto lock = Packages::ValidatedPackageLockfileV1::Parse(lockBytes, limits);
            if (lock.HasError())
                return Result<ValidatedMetadata>::Failure(lock.ErrorValue());
            if (auto matched = request.Value().ValidateLock(lock.Value()); matched.HasError())
                return Result<ValidatedMetadata>::Failure(matched.ErrorValue());
            return Result<ValidatedMetadata>::Success({std::move(request).Value(), std::move(lock).Value()});
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
        Result<bool> HasJournal(const std::filesystem::path &path, const DurableFileSystem &files) {
            const auto entry = files.InspectEntry(path);
            if (entry.HasError())
                return Result<bool>::Failure(entry.ErrorValue());
            if (entry.Value() && (entry.Value()->kind != FileEntryKind::RegularFile || entry.Value()->hardLinks != 1))
                return Result<bool>::Failure(MetadataConflict());
            return Result<bool>::Success(entry.Value().has_value());
        }

        constexpr std::string_view RollbackMagic = "HOROPUR1";
        constexpr auto RollbackHeader = RollbackMagic.size() + 2 * DigestSize;
        constexpr auto RollbackOverhead = RollbackHeader + DigestSize + 24U;

        struct RollbackPoint final {
            Sha256Digest publishedRequest;
            Sha256Digest publishedLock;
            std::string_view oldRequest;
            std::string_view oldLock;

            /** @brief Tests the exact committed side of the reviewed update. */
            bool MatchesPublished(const MetadataDocuments &documents) const {
                return Digest(documents.request) == publishedRequest && Digest(documents.lock) == publishedLock;
            }
        };

        /** @brief Stores one exact document pair, bounded by the same limits as project metadata. */
        std::string EncodeRollbackPoint(const MetadataDocuments &previous, const MetadataDocuments &next) {
            std::string bytes{RollbackMagic};
            AppendDigest(bytes, Digest(next.request));
            AppendDigest(bytes, Digest(next.lock));
            bytes.append(std::to_string(previous.request.size()));
            bytes.push_back('\n');
            bytes.append(previous.request);
            bytes.append(previous.lock);
            AppendDigest(bytes, Digest(bytes));
            return bytes;
        }

        /** @brief Validates framing and checksum before returning views into the owned file bytes. */
        std::optional<RollbackPoint> DecodeRollbackPoint(const std::string_view bytes, const Packages::PackageLockfileLimits &limits) {
            if (bytes.size() <= RollbackHeader + DigestSize || !bytes.starts_with(RollbackMagic))
                return std::nullopt;
            const auto body = bytes.substr(0, bytes.size() - DigestSize);
            if (DecodeDigest(bytes.substr(body.size())) != Digest(body))
                return std::nullopt;
            const auto separator = body.find('\n', RollbackHeader);
            if (separator == std::string_view::npos || separator - RollbackHeader > 20)
                return std::nullopt;
            std::size_t requestSize{};
            const auto end = body.data() + separator;
            const auto converted = std::from_chars(body.data() + RollbackHeader, end, requestSize);
            const auto documents = body.substr(separator + 1);
            if (converted.ec != std::errc{} || converted.ptr != end || requestSize == 0 || requestSize > RequestLimit ||
                requestSize >= documents.size() || documents.size() - requestSize > limits.documentBytes)
                return std::nullopt;
            return RollbackPoint{DecodeDigest(body.substr(RollbackMagic.size(), DigestSize)),
                                 DecodeDigest(body.substr(RollbackMagic.size() + DigestSize, DigestSize)), documents.substr(0, requestSize),
                                 documents.substr(requestSize)};
        }

        /** @brief Reconciles a staged checkpoint only after metadata recovery establishes a complete pair. */
        Result<void> ReconcileRollbackPoint(const std::filesystem::path &metadata, const MetadataDocuments &current,
                                            const Packages::PackageLockfileLimits &limits, DurableFileSystem &files) {
            const auto pending = RollbackPath(metadata, true);
            const auto exists = HasJournal(pending, files);
            if (exists.HasError())
                return Result<void>::Failure(exists.ErrorValue());
            if (!exists.Value())
                return Result<void>::Success();
            const auto bytes = ReadMetadata(pending, RequestLimit + limits.documentBytes + RollbackOverhead, {});
            if (bytes.HasError())
                return Result<void>::Failure(bytes.ErrorValue());
            const auto point = DecodeRollbackPoint(bytes.Value(), limits);
            if (!point)
                return Result<void>::Failure(MetadataConflict());
            if (point->MatchesPublished(current)) {
                const auto target = RollbackPath(metadata, false);
                if (const auto prior = HasJournal(target, files); prior.HasError())
                    return Result<void>::Failure(prior.ErrorValue());
                AtomicFileReplacementReceipt receipt;
                return files.AtomicReplaceTracked(pending, target, receipt);
            }
            return files.RemoveDurable(pending);
        }

        /** @brief Serializes repair and removal with authored Scene, asset and migration writers across processes. */
        Result<Editor::ProjectMutationLease> AcquireProjectMutation(const std::filesystem::path &metadata, DurableFileSystem &files,
                                                                    std::string operation) {
            const Editor::ProjectMutationCoordinator mutations{files};
            return mutations.TryAcquire({metadata.parent_path(), Editor::ProjectMutationOwner::Package, std::move(operation)});
        }

        enum class RecoveryAction {
            Keep,
            RestoreRequest,
            Conflict
        };

        /** @brief Derives the only safe action from the exact observed identities. */
        RecoveryAction RecoveryFor(const MetadataDocuments &documents, const MetadataJournal &journal) {
            using enum RecoveryAction;
            const auto lock = Digest(documents.lock);
            const bool publishedRequest = Digest(documents.request) == journal.newRequest;
            if (lock == journal.newLock)
                return publishedRequest ? Keep : Conflict;
            if (lock != journal.oldLock)
                return Conflict;
            if (documents.request == journal.oldRequest)
                return Keep;
            return publishedRequest ? RestoreRequest : Conflict;
        }

        /** @brief Restores a proven half-commit without overwriting an independent revision. */
        Result<void> ReconcileDocuments(const std::filesystem::path &metadata, const Packages::PackageLockfileLimits &limits,
                                        const MetadataJournal &journal, DurableFileSystem &files, const CancellationToken &cancellation) {
            using enum RecoveryAction;
            const auto documents = ReadDocuments(metadata, limits, cancellation);
            if (documents.HasError())
                return Result<void>::Failure(documents.ErrorValue());
            const auto action = RecoveryFor(documents.Value(), journal);
            if (action == Conflict)
                return Result<void>::Failure(MetadataConflict());
            const std::string_view request = action == RestoreRequest ? journal.oldRequest : documents.Value().request;
            if (auto validated = ValidateDocuments(request, documents.Value().lock, limits); validated.HasError())
                return Result<void>::Failure(MetadataConflict());
            if (action == Keep)
                return Result<void>::Success();
            AtomicFileReplacementReceipt receipt;
            return ReplaceMetadata(metadata / "packages.json", request, files, receipt);
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
            if (auto reconciled = ReconcileDocuments(metadata, limits, *journal, files, cancellation); reconciled.HasError())
                return reconciled;
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
            if (const auto lease = AcquireProjectMutation(metadata, files, "package-metadata-recovery"); lease.HasValue())
                return RecoverMetadata(metadata, limits, files, cancellation);
            else
                return Result<void>::Failure(lease.ErrorValue());
        }

        /** @brief Detects changes to the approved pair by a writer outside the project lease. */
        Result<void> RecheckMetadataBase(const std::filesystem::path &metadata, const Packages::PackageLockfileLimits &limits,
                                         const MetadataDocuments &previous, const CancellationToken &cancellation) {
            const auto current = ReadDocuments(metadata, limits, cancellation);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            if (current.Value() != previous)
                return Result<void>::Failure(MetadataConflict());
            return Result<void>::Success();
        }

        /** @brief Checks the exact approved documents against immutable staging evidence and host restore policy. */
        Result<void> CheckUpdateCandidate(const PackageProjectPolicy &policy, const ApprovedPackageProjectUpdate &update) {
            if (update.candidateRequest.size() > RequestLimit || update.candidateLock.size() > policy.limits.lockfile.documentBytes)
                return Result<void>::Failure(MakeError(Packages::PackageRestoreErrors::ResourceLimit));
            const auto graph = update.staged.Graph();
            const auto &approval = update.approval;
            if (!graph || graph->platform != policy.platform || update.staged.ProjectRoot() != policy.projectRoot ||
                update.base.application.restore.projectRoot != policy.projectRoot ||
                approval.baseRequest != update.base.requestDocumentDigest || approval.baseLock != update.base.lockDigest ||
                approval.candidateRequest != Digest(update.candidateRequest) || approval.candidateLock != Digest(update.candidateLock) ||
                approval.candidateLock != update.staged.LockDigest())
                return Result<void>::Failure(MetadataConflict());
            const auto candidate = ValidateDocuments(update.candidateRequest, update.candidateLock, policy.limits.lockfile);
            if (candidate.HasError())
                return Result<void>::Failure(candidate.ErrorValue());
            const auto &request = candidate.Value().request;
            if (graph->requestHash != request.Digest())
                return Result<void>::Failure(MetadataConflict());
            if (policy.requirePublisherVerification && std::ranges::any_of(graph->packages, [](const auto &package) {
                return !package.publisher.has_value();
            }))
                return Result<void>::Failure(MakeError(Packages::PackageRestoreErrors::PublisherEvidenceUnavailable));
            return candidate.Value().lock.ValidateForRestore(request.Digest(), policy.platform,
                                                             policy.limits.supportedPackageFormatVersion);
        }

        /** @brief Rechecks the request after the lock read and detects an in-flight paired publication. */
        Result<void> ConfirmReadablePair(const std::filesystem::path &metadata, const std::string_view request,
                                         const DurableFileSystem &files, const CancellationToken &cancellation) {
            const auto recheck = ReadMetadata(metadata / "packages.json", RequestLimit, cancellation);
            if (recheck.HasError())
                return Result<void>::Failure(recheck.ErrorValue());
            if (recheck.Value() != request)
                return Result<void>::Failure(MakeError(Packages::PackageRestoreErrors::InvalidInput));
            const auto pending = HasJournal(JournalPath(metadata), files);
            if (pending.HasError())
                return Result<void>::Failure(pending.ErrorValue());
            if (pending.Value())
                return Result<void>::Failure(MakeError(Packages::PackageRestoreErrors::Busy));
            return Result<void>::Success();
        }

        /** @brief The durable request precedes the lock, whose native replacement is the commit point. */
        Result<PackageMetadataPublication> CommitDocuments(const std::filesystem::path &metadata, const MetadataDocuments &previous,
                                                           const MetadataDocuments &next, const Packages::PackageLockfileLimits &limits,
                                                           DurableFileSystem &files,
                                                           const Platform::ProjectReadContext &context) {
            using Publication = Result<PackageMetadataPublication>;
            const auto journalPath = JournalPath(metadata);
            AtomicFileReplacementReceipt journalReceipt;
            const auto journal = EncodeJournal({Digest(previous.lock), Digest(next.lock), Digest(next.request), previous.request});
            if (auto stable = RecheckMetadataBase(metadata, limits, previous, context.cancellation); stable.HasError())
                return Publication::Failure(stable.ErrorValue());
            if (auto authority = Platform::CheckProjectReadContext(context); authority.HasError())
                return Publication::Failure(authority.ErrorValue());
            // Once the journal is admitted, finish it under the mutation lease even if the caller closes.
            if (auto recorded = ReplaceMetadata(journalPath, journal, files, journalReceipt); recorded.HasError())
                return Publication::Failure(recorded.ErrorValue());
            AtomicFileReplacementReceipt requestReceipt;
            if (auto written = ReplaceMetadata(metadata / "packages.json", next.request, files, requestReceipt); written.HasError())
                return Publication::Failure(written.ErrorValue());
            AtomicFileReplacementReceipt lockReceipt;
            if (auto written = ReplaceMetadata(metadata / "packages.lock", next.lock, files, lockReceipt); written.HasError())
                return lockReceipt.WasCommitted() ? Publication::Success({written.ErrorValue()})
                                                  : Publication::Failure(written.ErrorValue());
            if (auto cleared = files.RemoveDurable(journalPath); cleared.HasError())
                return Publication::Success({cleared.ErrorValue()});
            return Publication::Success({});
        }

        /** @brief Recovers interrupted work and reopens the exact approved pair under the caller's mutation lease. */
        Result<MetadataDocuments> ReadApprovedDocuments(const std::filesystem::path &metadata,
                                                        const Packages::PackageLockfileLimits &limits,
                                                        const PreparedPackageProject &expected, DurableFileSystem &files,
                                                        const CancellationToken &cancellation) {
            if (auto recovered = RecoverMetadata(metadata, limits, files, cancellation); recovered.HasError())
                return Result<MetadataDocuments>::Failure(recovered.ErrorValue());
            auto previous = ReadDocuments(metadata, limits, cancellation);
            if (previous.HasValue() && !previous.Value().Matches(expected))
                return Result<MetadataDocuments>::Failure(MetadataConflict());
            return previous;
        }

        /** @brief Keeps the prior pair through a recoverable metadata publication under the caller's mutation lease. */
        Result<PackageMetadataPublication> CommitWithRollbackPoint(const std::filesystem::path &metadata, const MetadataDocuments &previous,
                                                                   const MetadataDocuments &next,
                                                                   const Packages::PackageLockfileLimits &limits, DurableFileSystem &files,
                                                                   const Platform::ProjectReadContext &context) {
            using Publication = Result<PackageMetadataPublication>;
            if (auto reconciled = ReconcileRollbackPoint(metadata, previous, limits, files); reconciled.HasError())
                return Publication::Failure(reconciled.ErrorValue());
            if (previous.lock == next.lock)
                return Publication::Success({});
            if (auto authority = Platform::CheckProjectReadContext(context); authority.HasError())
                return Publication::Failure(authority.ErrorValue());
            AtomicFileReplacementReceipt receipt;
            if (auto checkpoint = ReplaceMetadata(RollbackPath(metadata, true), EncodeRollbackPoint(previous, next), files, receipt);
                checkpoint.HasError())
                return Publication::Failure(checkpoint.ErrorValue());

            auto result = CommitDocuments(metadata, previous, next, limits, files, context);
            if (result.HasValue()) {
                if (result.Value().recoveryDiagnostic)
                    return result;
                auto reconciled = ReconcileRollbackPoint(metadata, next, limits, files);
                return reconciled.HasError() ? Publication::Success({std::move(reconciled).ErrorValue()}) : std::move(result);
            }
            if (auto recovered = RecoverMetadata(metadata, limits, files, {}); recovered.HasError())
                return Publication::Failure(WithCause(std::move(result).ErrorValue(), std::move(recovered).ErrorValue()));
            if (auto reconciled = ReconcileRollbackPoint(metadata, previous, limits, files); reconciled.HasError())
                return Publication::Failure(WithCause(std::move(result).ErrorValue(), std::move(reconciled).ErrorValue()));
            return result;
        }

        /** @brief Keeps the most relevant scan failure while preserving diagnostics from independent scans. */
        void RecordRemovalFailure(std::optional<Error> &blocking, Error failure) {
            if (!blocking) {
                blocking = std::move(failure);
                return;
            }
            if (failure.diagnostics.empty()) {
                blocking->diagnostics.emplace_back(DiagnosticCode{failure.code.Value()}, DiagnosticSeverity::Error,
                                                   std::move(failure.message), SourceLocation{failure.domain.Value()}, "");
                return;
            }
            std::ranges::move(failure.diagnostics, std::back_inserter(blocking->diagnostics));
        }

        /** @brief Reports confirmed references even when another reference category cannot be fully inspected. */
        Result<void> CheckRemovalReferences(const PackageRemovalSources &sources, const Packages::ValidatedPackageLockfileV1 &lock,
                                            const Packages::PackageRemovalGraphPreview &impact) {
            std::optional<Error> blocking;
            if (auto coverage = CheckRemovalRegistrationCoverage(sources, lock, impact); coverage.HasError())
                RecordRemovalFailure(blocking, std::move(coverage).ErrorValue());

            std::vector<Diagnostic> references;
            auto importers = InspectPackageRemovalImporters(sources.assets, impact);
            if (importers.HasError())
                RecordRemovalFailure(blocking, std::move(importers).ErrorValue());
            else
                AppendPackageImporterDiagnostics(references, importers.Value());

            auto sceneSources = InspectPackageScenePrefabReferences(sources, lock, impact);
            if (sceneSources.HasError())
                RecordRemovalFailure(blocking, std::move(sceneSources).ErrorValue());
            else
                std::ranges::move(sceneSources.Value(), std::back_inserter(references));

            auto nativeReferences = InspectPackageNativeReferences(sources, lock, impact);
            if (nativeReferences.HasError())
                RecordRemovalFailure(blocking, std::move(nativeReferences).ErrorValue());
            else
                std::ranges::move(nativeReferences.Value(), std::back_inserter(references));

            auto decisions = InspectPackageDecisionReferences(sources, lock, impact);
            if (decisions.HasError())
                RecordRemovalFailure(blocking, std::move(decisions).ErrorValue());
            else
                std::ranges::move(decisions.Value(), std::back_inserter(references));

            if (!blocking && references.empty())
                return Result<void>::Success();
            if (!blocking)
                return Result<void>::Failure(Error{ErrorCode{references.front().code.Value()}, ErrorDomainId{"packages.host"},
                                                   ErrorSeverity::Error, "", std::move(references)});
            std::ranges::move(references, std::back_inserter(blocking->diagnostics));
            return Result<void>::Failure(std::move(*blocking));
        }

        /** @brief Constructs replacement documents after checking native authority and saved references. */
        Result<MetadataDocuments> PrepareRemovalDocuments(const MetadataDocuments &previous, const Packages::PackageLockfileLimits &limits,
                                                          const std::filesystem::path &root, const Packages::HoroPackageId &package,
                                                          const PackageRemovalSources &sources) {
            using Prepared = Result<MetadataDocuments>;
            const auto validated = ValidateDocuments(previous.request, previous.lock, limits);
            if (validated.HasError())
                return Prepared::Failure(validated.ErrorValue());
            const auto plan = Packages::PackageRemovalPlan::Prepare(validated.Value().request, validated.Value().lock, package);
            if (plan.HasError())
                return Prepared::Failure(plan.ErrorValue());
            if (!sources.offline && (!sources.live || !sources.live->Matches(root, plan.Value().graph.removed)))
                return Prepared::Failure(Error{ErrorCode{"packages.host.removal_reservation_mismatch"}, ErrorDomainId{"packages.host"}});
            if (auto references = CheckRemovalReferences(sources, validated.Value().lock, plan.Value().graph); references.HasError())
                return Prepared::Failure(references.ErrorValue());
            MetadataDocuments next{plan.Value().request.SerializeCanonical(), plan.Value().lock.SerializeCanonical()};
            if (!next.Fits(limits))
                return Prepared::Failure(MakeError(Packages::PackageRestoreErrors::ResourceLimit));
            return Prepared::Success(std::move(next));
        }

        /** @brief Keeps offline staging and live reservations exclusive and bound to the approved project. */
        bool MatchesRemovalProject(const std::filesystem::path &root, const PreparedPackageProject &expected,
                                   const PackageRemovalSources &sources) {
            if (root != expected.application.restore.projectRoot)
                return false;
            return !sources.offline ||
                   (!sources.live && sources.offline->ProjectRoot() == root && sources.offline->LockDigest() == expected.lockDigest);
        }
    }  // namespace

    /** @copydoc PreparePackageProject */
    Result<PreparedPackageProject> PreparePackageProject(const PackageProjectPolicy &policy, DurableFileSystem &files,
                                                         const CancellationToken &cancellation, const PackageProjectReadMode mode) {
        using Prepared = Result<PreparedPackageProject>;
        const auto directory = MetadataPath(policy.projectRoot);
        if (directory.HasError())
            return Prepared::Failure(directory.ErrorValue());
        const auto &metadata = directory.Value();
        if (mode == PackageProjectReadMode::Recover) {
            if (auto recovered = RecoverInterruptedPublication(metadata, policy.limits.lockfile, files, cancellation); recovered.HasError())
                return Prepared::Failure(recovered.ErrorValue());
        } else {
            const auto pending = HasJournal(JournalPath(metadata), files);
            if (pending.HasError())
                return Prepared::Failure(pending.ErrorValue());
            if (pending.Value())
                return Prepared::Failure(MakeError(Packages::PackageRestoreErrors::Busy));
        }
        auto documents = ReadDocuments(metadata, policy.limits.lockfile, cancellation);
        if (documents.HasError())
            return Result<PreparedPackageProject>::Failure(documents.ErrorValue());
        auto validated = ValidateDocuments(documents.Value().request, documents.Value().lock, policy.limits.lockfile);
        if (validated.HasError())
            return Result<PreparedPackageProject>::Failure(validated.ErrorValue());
        auto [request, lock] = std::move(validated).Value();
        if (const auto valid = lock.ValidateForRestore(request.Digest(), policy.platform, policy.limits.supportedPackageFormatVersion);
            valid.HasError())
            return Result<PreparedPackageProject>::Failure(valid.ErrorValue());
        if (auto stable = ConfirmReadablePair(metadata, documents.Value().request, files, cancellation); stable.HasError())
            return Result<PreparedPackageProject>::Failure(stable.ErrorValue());
        auto bytes = std::move(documents).Value();
        PreparedPackageProject prepared;
        prepared.application.restore = {.projectRoot = metadata.parent_path(),
                                        .lockfileJson = std::move(bytes.lock),
                                        .expectedRequestHash = request.Digest(),
                                        .platform = policy.platform,
                                        .mode = policy.mode,
                                        .requirePublisherVerification = policy.requirePublisherVerification};
        prepared.application.lockedPlan = std::move(lock);
        prepared.application.portableRequest = std::move(request);
        prepared.lockDigest = Digest(prepared.application.restore.lockfileJson);
        prepared.requestDocumentDigest = Digest(bytes.request);
        return Result<PreparedPackageProject>::Success(std::move(prepared));
    }

    /** @copydoc PublishPackageProjectUpdate */
    Result<PackageMetadataPublication> PublishPackageProjectUpdate(const PackageProjectPolicy &policy,
                                                                   const ApprovedPackageProjectUpdate &update, DurableFileSystem &files,
                                                                   const Platform::ProjectReadContext &context) {
        using Publication = Result<PackageMetadataPublication>;
        if (auto authority = Platform::CheckProjectReadContext(context); authority.HasError())
            return Publication::Failure(authority.ErrorValue());
        const auto directory = MetadataPath(policy.projectRoot);
        if (directory.HasError())
            return Publication::Failure(directory.ErrorValue());
        if (auto candidate = CheckUpdateCandidate(policy, update); candidate.HasError())
            return Publication::Failure(candidate.ErrorValue());
        const auto &metadata = directory.Value();
        auto lease = AcquireProjectMutation(metadata, files, "package-update");
        if (lease.HasError())
            return Publication::Failure(lease.ErrorValue());
        const auto &limits = policy.limits.lockfile;
        const auto previous = ReadApprovedDocuments(metadata, limits, update.base, files, context.cancellation);
        if (previous.HasError())
            return Publication::Failure(previous.ErrorValue());
        const MetadataDocuments next{std::string{update.candidateRequest}, std::string{update.candidateLock}};
        return CommitWithRollbackPoint(metadata, previous.Value(), next, limits, files, context);
    }

    /** @copydoc ReadPackageProjectRollback */
    Result<PackageRollbackDocuments> ReadPackageProjectRollback(const PackageProjectPolicy &policy, DurableFileSystem &files,
                                                                const CancellationToken &cancellation) {
        using Output = Result<PackageRollbackDocuments>;
        const auto directory = MetadataPath(policy.projectRoot);
        if (directory.HasError())
            return Output::Failure(directory.ErrorValue());
        const auto &metadata = directory.Value();
        const auto &limits = policy.limits.lockfile;
        const auto current = ReadDocuments(metadata, limits, cancellation);
        if (current.HasError())
            return Output::Failure(current.ErrorValue());
        if (const auto stable = ConfirmReadablePair(metadata, current.Value().request, files, cancellation); stable.HasError())
            return Output::Failure(stable.ErrorValue());
        for (const bool pending : {true, false}) {
            const auto path = RollbackPath(metadata, pending);
            const auto exists = HasJournal(path, files);
            if (exists.HasError())
                return Output::Failure(exists.ErrorValue());
            if (!exists.Value())
                continue;
            const auto bytes = ReadMetadata(path, RequestLimit + limits.documentBytes + RollbackOverhead, cancellation);
            if (bytes.HasError())
                return Output::Failure(bytes.ErrorValue());
            const auto point = DecodeRollbackPoint(bytes.Value(), limits);
            if (!point)
                return Output::Failure(MetadataConflict());
            if (!point->MatchesPublished(current.Value())) {
                if (pending)
                    continue;
                return Output::Failure(MetadataConflict());
            }
            const auto valid = ValidateDocuments(point->oldRequest, point->oldLock, limits);
            if (valid.HasError())
                return Output::Failure(valid.ErrorValue());
            if (const auto stable = RecheckMetadataBase(metadata, limits, current.Value(), cancellation); stable.HasError())
                return Output::Failure(stable.ErrorValue());
            return Output::Success(
                {std::string{point->oldRequest}, std::string{point->oldLock}, point->publishedRequest, point->publishedLock});
        }
        return Output::Failure(Error{ErrorCode{"packages.host.rollback_unavailable"}, ErrorDomainId{"packages.host"}});
    }

    /** @copydoc PublishPackageProjectRemoval */
    Result<PackageMetadataPublication> PublishPackageProjectRemoval(const PackageProjectPolicy &policy,
                                                                    const PreparedPackageProject &expected,
                                                                    const Packages::HoroPackageId &package, DurableFileSystem &files,
                                                                    const PackageRemovalSources &sources) {
        using Publication = Result<PackageMetadataPublication>;
        if (auto authority = Platform::CheckProjectReadContext(sources.context); authority.HasError())
            return Publication::Failure(authority.ErrorValue());
        const auto directory = MetadataPath(policy.projectRoot);
        if (directory.HasError())
            return Publication::Failure(directory.ErrorValue());
        const auto &metadata = directory.Value();
        const auto root = metadata.parent_path();
        if (!MatchesRemovalProject(root, expected, sources))
            return Publication::Failure(MetadataConflict());
        auto acquired = AcquireProjectMutation(metadata, files, "package-removal");
        if (acquired.HasError())
            return Publication::Failure(acquired.ErrorValue());
        const auto lease = std::move(acquired).Value();
        const auto &cancellation = sources.context.cancellation;
        const auto previous = ReadApprovedDocuments(metadata, policy.limits.lockfile, expected, files, cancellation);
        if (previous.HasError())
            return Publication::Failure(previous.ErrorValue());
        const auto next = PrepareRemovalDocuments(previous.Value(), policy.limits.lockfile, root, package, sources);
        if (next.HasError())
            return Publication::Failure(next.ErrorValue());
        if (auto stable = RecheckMetadataBase(metadata, policy.limits.lockfile, previous.Value(), cancellation); stable.HasError())
            return Publication::Failure(stable.ErrorValue());
        auto published = CommitWithRollbackPoint(metadata, previous.Value(), next.Value(), policy.limits.lockfile, files, sources.context);
        if (published.HasValue() && sources.live)
            sources.live->MarkPublished();
        return published;
    }
}  // namespace Horo::Application::Internal
