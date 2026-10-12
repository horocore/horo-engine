#include "Horo/Assets/AssetSourcePublication.h"

#include "Horo/Foundation/PathUtils.h"

#include <algorithm>
#include <bit>
#include <fstream>
#include <new>
#include <type_traits>

namespace Horo::Assets {
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

        /** @brief Rejects lexical/physical escape and a retargeted canonical destination before I/O. */
        bool SafePath(const std::filesystem::path &root, const std::filesystem::path &path) {
            std::error_code error;
            if (!root.is_absolute() || !path.is_absolute() || root != root.lexically_normal() || path != path.lexically_normal() ||
                root == path || !Foundation::Paths::HasPathPrefix(root, path))
                return false;
            const auto canonicalRoot = std::filesystem::canonical(root, error);
            if (error)
                return false;
            const auto canonicalPath = std::filesystem::weakly_canonical(path, error);
            return !error && canonicalPath == canonicalRoot / path.lexically_relative(root) &&
                   Foundation::Paths::HasPathPrefix(canonicalRoot, canonicalPath);
        }

        /** @brief Computes the byte-exact identity of one complete owned capture. */
        AssetSourceIdentity Identity(std::span<const std::byte> bytes) {
            return {true, bytes.size(), ComputeSha256(bytes)};
        }

        /** @brief Owns only an exclusively created stage; cleanup cannot mask a publication outcome. */
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
                } catch (...) {  // NOSONAR(cpp:S1181, cpp:S2738): ADR-008 forbids provider exceptions escaping rollback destruction.
                }
            }
        };

        /** @brief Reconciles the irreversible native receipt using error storage reserved before replacement. */
        Result<AssetSourcePublication> ReplacePreparedSource(const std::filesystem::path &path, std::span<const std::byte> bytes,
                                                             PreparedSource &cleanup) {
            AssetSourcePublication publication{AssetSourcePublicationStatus::Durable, Identity(bytes), {}};
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
            } catch (...) {  // NOSONAR(cpp:S1181, cpp:S2738): the Platform commit receipt preserves every postcommit exception.
                if (!receipt.WasCommitted())
                    throw;
                cleanup.created = false;
                publication.status = AssetSourcePublicationStatus::ReconciliationRequired;
                publication.durabilityError.emplace(std::move(fallback));
            }
            return Result<AssetSourcePublication>::Success(std::move(publication));
        }
    }  // namespace

    /** @copydoc CaptureAssetSource */
    Result<AssetSourceCapture> CaptureAssetSource(const std::filesystem::path &root, const std::filesystem::path &path,
                                                  const std::size_t maximumBytes, CancellationToken cancellation) {
        if (!SafePath(root, path))
            return Result<AssetSourceCapture>::Failure(MakeError(AssetSourceErrors::InvalidPath));
        if (!maximumBytes || maximumBytes > MaximumBytes)
            return Result<AssetSourceCapture>::Failure(MakeError(AssetSourceErrors::Limit));
        if (cancellation.IsCancellationRequested())
            return Result<AssetSourceCapture>::Failure(MakeError(AssetSourceErrors::Cancelled));
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error == std::errc::no_such_file_or_directory || (!error && !std::filesystem::exists(status)))
            return Result<AssetSourceCapture>::Success({});
        if (error || !std::filesystem::is_regular_file(status))
            return Result<AssetSourceCapture>::Failure(MakeError(AssetSourceErrors::InvalidPath));
        const auto size = std::filesystem::file_size(path, error);
        if (error)
            return Result<AssetSourceCapture>::Failure(MakeError(AssetSourceErrors::ReadFailed));
        if (size > maximumBytes)
            return Result<AssetSourceCapture>::Failure(MakeError(AssetSourceErrors::Limit));
        const auto modified = std::filesystem::last_write_time(path, error);
        if (error)
            return Result<AssetSourceCapture>::Failure(MakeError(AssetSourceErrors::ReadFailed));
        std::ifstream stream(path, std::ios::binary);
        AssetSourceCapture capture;
        capture.bytes.resize(static_cast<std::size_t>(size));
        for (std::size_t offset = 0; offset < capture.bytes.size();) {
            if (cancellation.IsCancellationRequested())
                return Result<AssetSourceCapture>::Failure(MakeError(AssetSourceErrors::Cancelled));
            const auto count = std::min<std::size_t>(4096, capture.bytes.size() - offset);
            if (!stream.read(std::bit_cast<char *>(capture.bytes.data() + offset), static_cast<std::streamsize>(count)))
                return Result<AssetSourceCapture>::Failure(MakeError(AssetSourceErrors::ReadFailed));
            offset += count;
        }
        if (!stream || stream.peek() != std::char_traits<char>::eof() || !SafePath(root, path) ||
            std::filesystem::file_size(path, error) != size || error)
            return Result<AssetSourceCapture>::Failure(MakeError(AssetSourceErrors::ReadFailed));
        if (std::filesystem::last_write_time(path, error) != modified || error)
            return Result<AssetSourceCapture>::Failure(MakeError(AssetSourceErrors::ReadFailed));
        capture.identity = Identity(capture.bytes);
        return Result<AssetSourceCapture>::Success(std::move(capture));
    }

    /** @copydoc AssetSourcePublicationLockPath */
    Result<std::filesystem::path> AssetSourcePublicationLockPath(const std::filesystem::path &root, const std::filesystem::path &path) {
        if (!SafePath(root, path))
            return Result<std::filesystem::path>::Failure(MakeError(AssetSourceErrors::InvalidPath));
        const auto relative = path.lexically_relative(root).generic_string();
        const auto digest = ComputeSha256(std::as_bytes(std::span(relative.data(), relative.size())));
        return Result<std::filesystem::path>::Success(root / ".horo/local/source-locks" / (FormatSha256(digest).substr(7) + ".lock"));
    }

    /** @copydoc PublishAssetSource */
    Result<AssetSourcePublication> PublishAssetSource(const std::filesystem::path &root, const std::filesystem::path &path,
                                                      const AssetSourceIdentity &expected, std::span<const std::byte> bytes,
                                                      const ExclusiveFileLock &lease, DurableFileSystem &files,
                                                      CancellationToken cancellation) {
        try {
            const auto lockPath = AssetSourcePublicationLockPath(root, path);
            if (lockPath.HasError())
                return Result<AssetSourcePublication>::Failure(lockPath.ErrorValue());
            if (!lease.ProtectsPath(lockPath.Value()))
                return Result<AssetSourcePublication>::Failure(MakeError(AssetSourceErrors::Authority));
            if (bytes.size() > MaximumBytes)
                return Result<AssetSourcePublication>::Failure(MakeError(AssetSourceErrors::Limit));
            auto current = CaptureAssetSource(root, path, MaximumBytes, cancellation);
            if (current.HasError())
                return Result<AssetSourcePublication>::Failure(current.ErrorValue());
            if (current.Value().identity != expected)
                return Result<AssetSourcePublication>::Failure(MakeError(AssetSourceErrors::Conflict));
            auto prepared = path;
            prepared += ".source-publication.tmp";
            if (!SafePath(root, prepared))
                return Result<AssetSourcePublication>::Failure(MakeError(AssetSourceErrors::InvalidPath));
            std::error_code stageError;
            if (const auto stageStatus = std::filesystem::symlink_status(prepared, stageError);
                (!stageError && std::filesystem::exists(stageStatus)) || (stageError && stageError != std::errc::no_such_file_or_directory))
                return Result<AssetSourcePublication>::Failure(MakeError(AssetSourceErrors::Conflict));
            PreparedSource cleanup(files, prepared);
            if (const auto written = files.WritePrivateDurable(prepared, bytes, cleanup.created); written.HasError())
                return Result<AssetSourcePublication>::Failure(written.ErrorValue());
            if (const auto observed = CaptureAssetSource(root, path, MaximumBytes, cancellation);
                observed.HasError() || observed.Value().identity != expected) {
                return Result<AssetSourcePublication>::Failure(observed.HasError() ? observed.ErrorValue()
                                                                                   : MakeError(AssetSourceErrors::Conflict));
            }
            if (const auto staged = CaptureAssetSource(root, prepared, MaximumBytes, cancellation);
                staged.HasError() || staged.Value().identity != Identity(bytes)) {
                return Result<AssetSourcePublication>::Failure(staged.HasError() ? staged.ErrorValue()
                                                                                 : MakeError(AssetSourceErrors::Conflict));
            }
            if (cancellation.IsCancellationRequested())
                return Result<AssetSourcePublication>::Failure(MakeError(AssetSourceErrors::Cancelled));
            return ReplacePreparedSource(path, bytes, cleanup);
        } catch (const std::bad_alloc &) {
            return Result<AssetSourcePublication>::Failure(MakeError(AssetSourceErrors::Limit));
        } catch (...) {  // NOSONAR(cpp:S1181, cpp:S2738): ADR-008 maps arbitrary precommit provider failures at the typed Assets boundary.
            return Result<AssetSourcePublication>::Failure(MakeError(AssetSourceErrors::ReadFailed));
        }
    }
}  // namespace Horo::Assets
