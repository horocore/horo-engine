#include "Horo/Assets/AssetSourcePublication.h"

#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/PathUtils.h"

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <new>
#include <type_traits>

namespace Horo::Assets {
    using Foundation::Paths::NativeApiPath;

    namespace AssetSourceErrors {
        const ErrorDomainId Domain{"horo.assets.source"};
        const ErrorCodeDescriptor InvalidPath{Domain, ErrorCode{"assets.source.path.invalid"}, ErrorSeverity::Error,
                                              "Source path is not a canonical contained regular file.", "Select a safe project source."};
        const ErrorCodeDescriptor ReadFailed{Domain, ErrorCode{"assets.source.read.failed"}, ErrorSeverity::Error,
                                             "Source could not be captured consistently.", "Retry after the writer finishes."};
        const ErrorCodeDescriptor Limit{Domain, ErrorCode{"assets.source.limit"}, ErrorSeverity::Error,
                                        "Source exceeds its finite byte budget.", "Reduce source size."};
        const ErrorCodeDescriptor Conflict{Domain, ErrorCode{"assets.source.conflict"}, ErrorSeverity::Error,
                                           "Source changed since the accepted capture.", "Compare or explicitly reload the new source."};
        const ErrorCodeDescriptor Authority{Domain, ErrorCode{"assets.source.authority"}, ErrorSeverity::Error,
                                            "Source publication requires an exact native exclusive lease.",
                                            "Acquire the source publication lease."};
        const ErrorCodeDescriptor Cancelled{Domain, ErrorCode{"assets.source.cancelled"}, ErrorSeverity::Error,
                                            "Source operation was cancelled.", "Submit a new operation."};
        const ErrorCodeDescriptor DurabilityUnconfirmed{Domain, ErrorCode{"assets.source.durability.unconfirmed"}, ErrorSeverity::Error,
                                                        "Source replacement is visible but its durability is unconfirmed.",
                                                        "Inspect and explicitly reconcile the published source."};
    }  // namespace AssetSourceErrors

    namespace {
        constexpr std::size_t MaximumBytes = 64 * 1024 * 1024;
        constexpr std::size_t MaximumReimportBytes = 1024ULL * 1024 * 1024;

        /** @brief Rejects lexical/physical escape and a retargeted canonical
         * destination before I/O. */
        bool SafePath(const std::filesystem::path &root, const std::filesystem::path &path) {
            std::error_code error;
            if (!root.is_absolute() || !path.is_absolute() || root != root.lexically_normal() || path != path.lexically_normal() ||
                root == path || !Foundation::Paths::HasPathPrefix(root, path))
                return false;
            const auto canonicalRoot = std::filesystem::canonical(NativeApiPath(root), error);
            if (error)
                return false;
            const auto canonicalPath = std::filesystem::weakly_canonical(NativeApiPath(path), error);
            return !error && canonicalPath == canonicalRoot / path.lexically_relative(root) &&
                   Foundation::Paths::HasPathPrefix(canonicalRoot, canonicalPath);
        }

        /** @brief Owns only an exclusively created stage; cleanup cannot mask a
         * publication outcome. */
        struct PreparedSource final {
            DurableFileSystem &files;
            const std::filesystem::path &path;
            bool created{};

            PreparedSource(DurableFileSystem &owner, const std::filesystem::path &temporary) noexcept : files(owner), path(temporary) {}

            PreparedSource(const PreparedSource &) = delete;
            PreparedSource &operator=(const PreparedSource &) = delete;

            ~PreparedSource() noexcept {
                if (!created)
                    return;
                try {
                    static_cast<void>(files.RemoveDurable(path));
                } catch (...) {  // NOSONAR(cpp:S1181, cpp:S2738): ADR-008 forbids provider
                                 // exceptions escaping rollback destruction.
                }
            }
        };

        /** @brief Reconciles the irreversible native receipt using error storage
         * reserved before replacement. */
        Result<AssetSourcePublication> ReplacePreparedSource(const std::filesystem::path &path, const AssetSourceIdentity &identity,
                                                             PreparedSource &cleanup) {
            AssetSourcePublication publication{AssetSourcePublicationStatus::Durable, identity, {}};
            auto fallback = MakeError(AssetSourceErrors::DurabilityUnconfirmed);
            static_assert(std::is_nothrow_move_constructible_v<AssetSourcePublication>);
            static_assert(std::is_nothrow_move_constructible_v<Error>);
            AtomicFileReplacementReceipt receipt;
            try {
                auto replaced = cleanup.files.AtomicReplaceTracked(cleanup.path, path, receipt);
                if (!receipt.WasCommitted())
                    return Result<AssetSourcePublication>::Failure(replaced.HasError() ? std::move(replaced).ErrorValue()
                                                                                       : MakeError(AssetSourceErrors::ReadFailed));
                cleanup.created = false;
                if (replaced.HasError()) {
                    publication.status = AssetSourcePublicationStatus::ReconciliationRequired;
                    publication.durabilityError.emplace(std::move(replaced).ErrorValue());
                }
            } catch (...) {  // NOSONAR(cpp:S1181, cpp:S2738): the Platform commit receipt
                             // preserves every postcommit exception.
                if (!receipt.WasCommitted())
                    throw;
                cleanup.created = false;
                publication.status = AssetSourcePublicationStatus::ReconciliationRequired;
                publication.durabilityError.emplace(std::move(fallback));
            }
            return Result<AssetSourcePublication>::Success(std::move(publication));
        }
    }  // namespace

    namespace {
        struct SourceFacts final {
            bool exists{};
            std::uintmax_t size{};
            std::filesystem::file_time_type modified{};
        };

        bool Missing(const std::error_code &error, std::filesystem::file_status status) noexcept {
            return error == std::errc::no_such_file_or_directory || (!error && !std::filesystem::exists(status));
        }

        Result<SourceFacts> ReadSourceFacts(const std::filesystem::path &root, const std::filesystem::path &path, std::size_t maximumBytes,
                                            CancellationToken cancellation) {
            using Return = Result<SourceFacts>;
            if (!SafePath(root, path))
                return Return::Failure(MakeError(AssetSourceErrors::InvalidPath));
            if (cancellation.IsCancellationRequested())
                return Return::Failure(MakeError(AssetSourceErrors::Cancelled));
            std::error_code error;
            const auto nativePath = NativeApiPath(path);
            const auto status = std::filesystem::symlink_status(nativePath, error);
            if (Missing(error, status))
                return Return::Success({});
            if (error || !std::filesystem::is_regular_file(status))
                return Return::Failure(MakeError(AssetSourceErrors::InvalidPath));
            const auto size = std::filesystem::file_size(nativePath, error);
            if (error)
                return Return::Failure(MakeError(AssetSourceErrors::ReadFailed));
            if (size > maximumBytes)
                return Return::Failure(MakeError(AssetSourceErrors::Limit));
            const auto modified = std::filesystem::last_write_time(nativePath, error);
            if (error)
                return Return::Failure(MakeError(AssetSourceErrors::ReadFailed));
            return Return::Success({true, size, modified});
        }

        bool SourceStillMatches(const std::filesystem::path &root, const std::filesystem::path &path, const SourceFacts &facts) {
            if (!SafePath(root, path))
                return false;
            std::error_code error;
            const auto nativePath = NativeApiPath(path);
            if (std::filesystem::file_size(nativePath, error) != facts.size || error)
                return false;
            return std::filesystem::last_write_time(nativePath, error) == facts.modified && !error;
        }

        /** @brief Hash every transferred chunk; optionally retain bytes without
         * allocating a duplicate read buffer. */
        Result<void> TransferSource(std::ifstream &stream, std::size_t size, AssetSourceCapture &capture, Sha256Builder &hash,
                                    bool retainBytes, CancellationToken cancellation) {
            std::array<std::byte, 4096> scratch{};
            for (std::size_t offset = 0; offset < size;) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(AssetSourceErrors::Cancelled));
                const auto count = std::min(scratch.size(), size - offset);
                auto *destination = retainBytes ? capture.bytes.data() + offset : scratch.data();
                if (!stream.read(std::bit_cast<char *>(destination), static_cast<std::streamsize>(count)))
                    return Result<void>::Failure(MakeError(AssetSourceErrors::ReadFailed));
                if (!hash.Update(std::span<const std::byte>(destination, count)))
                    return Result<void>::Failure(MakeError(AssetSourceErrors::ReadFailed));
                offset += count;
            }
            return Result<void>::Success();
        }

        Result<AssetSourceCapture> CaptureWithLimit(const std::filesystem::path &root, const std::filesystem::path &path,
                                                    std::size_t maximumBytes, bool retainBytes, CancellationToken cancellation) {
            using Return = Result<AssetSourceCapture>;
            auto facts = ReadSourceFacts(root, path, maximumBytes, cancellation);
            if (facts.HasError())
                return Return::Failure(std::move(facts).ErrorValue());
            if (!facts.Value().exists)
                return Return::Success({});
            AssetSourceCapture capture;
            const auto size = static_cast<std::size_t>(facts.Value().size);
            if (retainBytes)
                capture.bytes.resize(size);
            std::ifstream stream(NativeApiPath(path), std::ios::binary);
            Sha256Builder hash;
            if (auto transferred = TransferSource(stream, size, capture, hash, retainBytes, cancellation); transferred.HasError())
                return Return::Failure(std::move(transferred).ErrorValue());
            if (!stream || stream.peek() != std::char_traits<char>::eof() || !SourceStillMatches(root, path, facts.Value()))
                return Return::Failure(MakeError(AssetSourceErrors::ReadFailed));
            capture.identity = {true, size, hash.Finalize()};
            return Return::Success(std::move(capture));
        }

        Result<void> CheckSourceIdentity(const std::filesystem::path &root, const std::filesystem::path &path,
                                         const AssetSourceIdentity &expected, std::size_t maximumBytes, CancellationToken cancellation) {
            auto current = CaptureWithLimit(root, path, maximumBytes, false, cancellation);
            if (current.HasError())
                return Result<void>::Failure(std::move(current).ErrorValue());
            if (current.Value().identity != expected)
                return Result<void>::Failure(MakeError(AssetSourceErrors::Conflict));
            return Result<void>::Success();
        }

        Result<AssetSourceIdentity> HashPublication(std::span<const std::byte> bytes, CancellationToken cancellation) {
            Sha256Builder hash;
            for (std::size_t offset = 0; offset < bytes.size();) {
                if (cancellation.IsCancellationRequested())
                    return Result<AssetSourceIdentity>::Failure(MakeError(AssetSourceErrors::Cancelled));
                const auto count = std::min<std::size_t>(4096, bytes.size() - offset);
                if (!hash.Update(bytes.subspan(offset, count)))
                    return Result<AssetSourceIdentity>::Failure(MakeError(AssetSourceErrors::ReadFailed));
                offset += count;
            }
            return Result<AssetSourceIdentity>::Success({true, bytes.size(), hash.Finalize()});
        }
    }  // namespace

    /** @copydoc CaptureAssetSource */
    Result<AssetSourceCapture> CaptureAssetSource(const std::filesystem::path &root, const std::filesystem::path &path,
                                                  const std::size_t maximumBytes, CancellationToken cancellation) {
        if (!SafePath(root, path))
            return Result<AssetSourceCapture>::Failure(MakeError(AssetSourceErrors::InvalidPath));
        if (!maximumBytes || maximumBytes > MaximumBytes)
            return Result<AssetSourceCapture>::Failure(MakeError(AssetSourceErrors::Limit));
        return CaptureWithLimit(root, path, maximumBytes, true, cancellation);
    }

    /** @copydoc CaptureReimportAssetSource */
    Result<AssetSourceCapture> CaptureReimportAssetSource(const std::filesystem::path &root, const std::filesystem::path &path,
                                                          CancellationToken cancellation) {
        return CaptureWithLimit(root, path, MaximumReimportBytes, true, cancellation);
    }

    /** @copydoc InspectReimportAssetSource */
    Result<AssetSourceIdentity> InspectReimportAssetSource(const std::filesystem::path &root, const std::filesystem::path &path,
                                                           CancellationToken cancellation) {
        auto captured = CaptureWithLimit(root, path, MaximumReimportBytes, false, cancellation);
        if (captured.HasError())
            return Result<AssetSourceIdentity>::Failure(std::move(captured).ErrorValue());
        return Result<AssetSourceIdentity>::Success(captured.Value().identity);
    }

    /** @copydoc AssetSourcePublicationLockPath */
    Result<std::filesystem::path> AssetSourcePublicationLockPath(const std::filesystem::path &root, const std::filesystem::path &path) {
        if (!SafePath(root, path))
            return Result<std::filesystem::path>::Failure(MakeError(AssetSourceErrors::InvalidPath));
        const auto relative = path.lexically_relative(root).generic_u8string();
        const auto digest = ComputeSha256(std::as_bytes(std::span(relative.data(), relative.size())));
        return Result<std::filesystem::path>::Success(root / ".horo/local/source-locks" / (FormatSha256(digest).substr(7) + ".lock"));
    }

    namespace {
        Result<void> AdmitPublication(const std::filesystem::path &root, const std::filesystem::path &path, const ExclusiveFileLock &lease,
                                      std::size_t size, std::size_t maximumBytes) {
            const auto lockPath = AssetSourcePublicationLockPath(root, path);
            if (lockPath.HasError())
                return Result<void>::Failure(lockPath.ErrorValue());
            if (!lease.ProtectsPath(lockPath.Value()))
                return Result<void>::Failure(MakeError(AssetSourceErrors::Authority));
            if (size > maximumBytes)
                return Result<void>::Failure(MakeError(AssetSourceErrors::Limit));
            return Result<void>::Success();
        }

        Result<std::filesystem::path> PrepareSourcePath(const std::filesystem::path &root, const std::filesystem::path &path) {
            using Return = Result<std::filesystem::path>;
            auto prepared = path;
            prepared += ".source-publication.tmp";
            if (!SafePath(root, prepared))
                return Return::Failure(MakeError(AssetSourceErrors::InvalidPath));
            std::error_code stageError;
            if (const auto stageStatus = std::filesystem::symlink_status(NativeApiPath(prepared), stageError);
                (!stageError && std::filesystem::exists(stageStatus)) || (stageError && stageError != std::errc::no_such_file_or_directory))
                return Return::Failure(MakeError(AssetSourceErrors::Conflict));
            return Return::Success(std::move(prepared));
        }

        constexpr std::string_view AttemptSealName = "ownership.seal";
        constexpr std::string_view AttemptPayloadName = "payload.tmp";
        constexpr std::size_t MaximumPrivateAttempts = 64;

        /** @brief Destination-local single-owner stage with a stable, bounded seal. */
        struct PrivateAttempt final {
            std::filesystem::path directory;
            std::filesystem::path anchor;
            std::string seal;
        };

        /** @brief Derives stage authority from the exact destination lock key and never accepts a caller path. */
        Result<PrivateAttempt> DescribePrivateAttempt(const std::filesystem::path &root, const std::filesystem::path &path,
                                                      const AssetSourcePublicationAttempt &attempt) {
            using Return = Result<PrivateAttempt>;
            if (!attempt.id.IsValid() || !attempt.payload.exists || attempt.payload.bytes > MaximumBytes ||
                (attempt.placement != AssetSourceAttemptPlacement::DestinationLocal &&
                 attempt.placement != AssetSourceAttemptPlacement::ProjectLocal))
                return Return::Failure(MakeError(AssetSourceErrors::Authority));
            auto lock = AssetSourcePublicationLockPath(root, path);
            if (lock.HasError())
                return Return::Failure(lock.ErrorValue());
            const auto key = lock.Value().stem().string();
            const auto name = attempt.id.ToString();
            const auto anchor = attempt.placement == AssetSourceAttemptPlacement::ProjectLocal ? root : path.parent_path();
            const auto base = attempt.placement == AssetSourceAttemptPlacement::ProjectLocal ? root / ".horo/local/source-attempts"
                                                                                             : anchor / ".horo-source-attempts";
            const auto directory = base / key / name;
            if (!SafePath(root, directory))
                return Return::Failure(MakeError(AssetSourceErrors::InvalidPath));
            return Return::Success({directory, anchor,
                                    "horo.asset-source-attempt.v1\n" + key + "\n" + name + "\n" + std::to_string(attempt.payload.bytes) +
                                        "\n" + FormatSha256(attempt.payload.digest)});
        }

        /** @brief Claims a fresh attempt and anchors its sealed directory before any candidate payload can be written. */
        Result<std::filesystem::path> PreparePrivateAttempt(const std::filesystem::path &root, const std::filesystem::path &path,
                                                            const AssetSourcePublicationAttempt &attempt, DurableFileSystem &files) {
            using Return = Result<std::filesystem::path>;
            const auto described = DescribePrivateAttempt(root, path, attempt);
            if (described.HasError())
                return Return::Failure(described.ErrorValue());
            const auto &description = described.Value();
            const auto bucket = description.directory.parent_path();
            if (auto created = files.CreateDirectoryPath(bucket, DirectoryCreationMode::Parents); created.HasError())
                return Return::Failure(created.ErrorValue());
            const auto entries = files.ListDirectory(bucket, MaximumPrivateAttempts);
            if (entries.HasError())
                return Return::Failure(entries.ErrorValue());
            if (entries.Value().size() >= MaximumPrivateAttempts)
                return Return::Failure(MakeError(AssetSourceErrors::Limit));
            const auto reserved = files.CreateDirectoryPath(description.directory, DirectoryCreationMode::Single);
            if (reserved.HasError())
                return Return::Failure(reserved.ErrorValue());
            if (!reserved.Value())
                return Return::Failure(MakeError(AssetSourceErrors::Conflict));
            bool created{};
            if (auto sealed =
                    files.WritePrivateDurable(description.directory / AttemptSealName, std::as_bytes(std::span{description.seal}), created);
                sealed.HasError())
                return Return::Failure(sealed.ErrorValue());
            for (auto parent = description.directory.parent_path();; parent = parent.parent_path()) {
                if (auto synced = files.SyncDirectory(parent); synced.HasError())
                    return Return::Failure(synced.ErrorValue());
                if (parent == description.anchor)
                    break;
            }
            return Return::Success(description.directory / AttemptPayloadName);
        }

        /** @brief Retires only the exact live no-follow object reviewed under the destination's writer lease. */
        Result<void> RemovePrivateAttemptEntry(const std::filesystem::path &directory, const FileDirectoryEntry &entry,
                                               DurableFileSystem &files) {
            const auto path = directory / entry.name;
            const auto observed = files.InspectEntry(path);
            if (observed.HasError())
                return Result<void>::Failure(observed.ErrorValue());
            if (!observed.Value() || observed.Value()->kind != FileEntryKind::RegularFile || observed.Value()->hardLinks != 1 ||
                observed.Value()->identity != entry.info.identity)
                return Result<void>::Failure(MakeError(AssetSourceErrors::Conflict));
            return files.RemoveDurable(path);
        }

        /** @brief Settles an exclusively claimed attempt after the private payload guard unwinds. */
        struct AttemptCleanup final {
            const std::filesystem::path &root;
            const std::filesystem::path &path;
            const AssetSourcePublicationAttempt *attempt;
            const ExclusiveFileLock &lease;
            DurableFileSystem &files;

            ~AttemptCleanup() noexcept {
                if (attempt == nullptr)
                    return;
                try {
                    if (auto retired = RetireAssetSourcePublicationAttempt(root, path, *attempt, lease, files); retired.HasError())
                        LOG_WARN("assets.source", "Private source publication attempt retained: %s", retired.ErrorValue().message.c_str());
                } catch (...) {
                    LOG_WARN("assets.source", "Private source publication attempt retirement failed.");
                }
            }
        };

        Result<AssetSourcePublication> PublishChecked(const std::filesystem::path &root, const std::filesystem::path &path,
                                                      const AssetSourceIdentity &expected, std::span<const std::byte> bytes,
                                                      const ExclusiveFileLock &lease, DurableFileSystem &files, std::size_t maximumBytes,
                                                      CancellationToken cancellation, const AssetSourcePublicationAttempt *attempt) {
            using Return = Result<AssetSourcePublication>;
            if (auto admitted = AdmitPublication(root, path, lease, bytes.size(), maximumBytes); admitted.HasError())
                return Return::Failure(std::move(admitted).ErrorValue());
            if (auto current = CheckSourceIdentity(root, path, expected, maximumBytes, cancellation); current.HasError())
                return Return::Failure(std::move(current).ErrorValue());
            auto identity = HashPublication(bytes, cancellation);
            if (identity.HasError())
                return Return::Failure(std::move(identity).ErrorValue());
            if (attempt && identity.Value() != attempt->payload)
                return Return::Failure(MakeError(AssetSourceErrors::Conflict));
            auto prepared = attempt ? PreparePrivateAttempt(root, path, *attempt, files) : PrepareSourcePath(root, path);
            if (prepared.HasError())
                return Return::Failure(std::move(prepared).ErrorValue());
            const AttemptCleanup attemptCleanup{root, path, attempt, lease, files};
            PreparedSource cleanup(files, prepared.Value());
            if (auto written = files.WritePrivateDurable(prepared.Value(), bytes, cleanup.created); written.HasError())
                return Return::Failure(std::move(written).ErrorValue());
            if (auto current = CheckSourceIdentity(root, path, expected, maximumBytes, cancellation); current.HasError())
                return Return::Failure(std::move(current).ErrorValue());
            if (auto staged = CheckSourceIdentity(root, prepared.Value(), identity.Value(), maximumBytes, cancellation); staged.HasError())
                return Return::Failure(std::move(staged).ErrorValue());
            if (cancellation.IsCancellationRequested())
                return Return::Failure(MakeError(AssetSourceErrors::Cancelled));
            return ReplacePreparedSource(path, identity.Value(), cleanup);
        }

        Result<AssetSourcePublication> PublishWithLimit(const std::filesystem::path &root, const std::filesystem::path &path,
                                                        const AssetSourceIdentity &expected, std::span<const std::byte> bytes,
                                                        const ExclusiveFileLock &lease, DurableFileSystem &files, std::size_t maximumBytes,
                                                        CancellationToken cancellation,
                                                        const AssetSourcePublicationAttempt *attempt = nullptr) {
            try {
                return PublishChecked(root, path, expected, bytes, lease, files, maximumBytes, cancellation, attempt);
            } catch (const std::bad_alloc &) {
                return Result<AssetSourcePublication>::Failure(MakeError(AssetSourceErrors::Limit));
            } catch (...) {  // ADR-008: map precommit provider failures;
                             // ReplacePreparedSource retains native commit receipts.
                return Result<AssetSourcePublication>::Failure(MakeError(AssetSourceErrors::ReadFailed));
            }
        }
    }  // namespace

    /** @copydoc PublishAssetSource */
    Result<AssetSourcePublication> PublishAssetSource(const std::filesystem::path &root, const std::filesystem::path &path,
                                                      const AssetSourceIdentity &expected, std::span<const std::byte> bytes,
                                                      const ExclusiveFileLock &lease, DurableFileSystem &files,
                                                      CancellationToken cancellation) {
        return PublishWithLimit(root, path, expected, bytes, lease, files, MaximumBytes, cancellation);
    }

    /** @copydoc PublishAssetSourceAttempt */
    Result<AssetSourcePublication> PublishAssetSourceAttempt(const std::filesystem::path &root, const std::filesystem::path &path,
                                                             const AssetSourceIdentity &expected, const std::span<const std::byte> bytes,
                                                             const AssetSourcePublicationAttempt &attempt, const ExclusiveFileLock &lease,
                                                             DurableFileSystem &files, CancellationToken cancellation) {
        return PublishWithLimit(root, path, expected, bytes, lease, files, MaximumBytes, cancellation, &attempt);
    }

    /** @copydoc RetireAssetSourcePublicationAttempt */
    Result<void> RetireAssetSourcePublicationAttempt(const std::filesystem::path &root, const std::filesystem::path &path,
                                                     const AssetSourcePublicationAttempt &attempt, const ExclusiveFileLock &lease,
                                                     DurableFileSystem &files) {
        using Return = Result<void>;
        if (auto admitted = AdmitPublication(root, path, lease, 0, MaximumBytes); admitted.HasError())
            return admitted;
        const auto described = DescribePrivateAttempt(root, path, attempt);
        if (described.HasError())
            return Return::Failure(described.ErrorValue());
        const auto &stage = described.Value();
        const auto metadata = files.InspectEntry(stage.directory);
        if (metadata.HasError())
            return Return::Failure(metadata.ErrorValue());
        if (!metadata.Value())
            return Return::Success();
        const auto conflict = [] {
            return Return::Failure(MakeError(AssetSourceErrors::Conflict));
        };
        if (metadata.Value()->kind != FileEntryKind::Directory)
            return conflict();
        const auto entries = files.ListDirectory(stage.directory, 2);
        if (entries.HasError())
            return Return::Failure(entries.ErrorValue());
        const FileDirectoryEntry *sealEntry{};
        const FileDirectoryEntry *payloadEntry{};
        for (const auto &entry : entries.Value()) {
            if (entry.info.kind != FileEntryKind::RegularFile || entry.info.hardLinks != 1 || !entry.info.identity)
                return conflict();
            if (entry.name == AttemptPayloadName)
                payloadEntry = &entry;
            else if (entry.name == AttemptSealName)
                sealEntry = &entry;
            else
                return conflict();
        }
        if (sealEntry == nullptr)
            return conflict();
        const auto seal = files.ReadFileBounded(stage.directory / AttemptSealName, stage.seal.size());
        if (seal.HasError())
            return Return::Failure(seal.ErrorValue());
        if (!std::ranges::equal(std::as_bytes(std::span{seal.Value()}), std::as_bytes(std::span{stage.seal})))
            return conflict();

        if (payloadEntry != nullptr) {
            const auto payload = stage.directory / AttemptPayloadName;
            if (auto verified = CheckSourceIdentity(root, payload, attempt.payload, MaximumBytes, {}); verified.HasError())
                return verified;
            if (auto retired = RemovePrivateAttemptEntry(stage.directory, *payloadEntry, files); retired.HasError())
                return retired;
        }
        if (auto retired = RemovePrivateAttemptEntry(stage.directory, *sealEntry, files); retired.HasError())
            return retired;
        return files.RemoveDurable(stage.directory);
    }

    /** @copydoc PublishReimportAssetSource */
    Result<AssetSourcePublication> PublishReimportAssetSource(const std::filesystem::path &root, const std::filesystem::path &path,
                                                              const AssetSourceIdentity &expected, std::span<const std::byte> bytes,
                                                              const ExclusiveFileLock &lease, DurableFileSystem &files,
                                                              CancellationToken cancellation) {
        return PublishWithLimit(root, path, expected, bytes, lease, files, MaximumReimportBytes, cancellation);
    }
}  // namespace Horo::Assets
