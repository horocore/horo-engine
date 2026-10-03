#include "Horo/Release/UpdateOfflineSource.h"

#include "Horo/Release/UpdateOfflineSourceErrors.h"
#include "Horo/Release/UpdateTransferCheckpointStore.h"
#include "Horo/Release/UpdateTransferErrors.h"
#include "Horo/Security/SecurityErrors.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <ranges>
#include <span>
#include <system_error>
#include <utility>

namespace Horo::Release {
    namespace {
        constexpr std::size_t MaximumManifestBytes = 128U * 1024U;
        constexpr std::size_t CopyChunkBytes = 64U * 1024U;

        /** @brief Rejects traversal even when the resulting lexical path would remain below the root. */
        [[nodiscard]] bool NoTraversal(const std::filesystem::path &path) {
            return std::ranges::none_of(path, [](const auto &part) {
                return part == "." || part == "..";
            });
        }

        /** @brief Distinguishes missing media from links and other unsafe file types. */
        [[nodiscard]] Result<void> CheckType(const std::filesystem::path &path, const std::filesystem::file_type expected) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (error == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found)
                return Result<void>::Failure(MakeError(UpdateOfflineSourceErrors::Unavailable));
            if (error || status.type() != expected)
                return Result<void>::Failure(MakeError(UpdateOfflineSourceErrors::UnsafePath));
            if (expected == std::filesystem::file_type::regular && (std::filesystem::hard_link_count(path, error) != 1U || error))
                return Result<void>::Failure(MakeError(UpdateOfflineSourceErrors::UnsafePath));
            return Result<void>::Success();
        }

        /** @brief Distinguishes an absent medium from a linked or malformed one. */
        [[nodiscard]] Result<void> CheckRoot(const std::filesystem::path &root) {
            if (!root.is_absolute() || !NoTraversal(root))
                return Result<void>::Failure(MakeError(UpdateOfflineSourceErrors::UnsafePath));
            std::error_code error;
            const auto status = std::filesystem::symlink_status(root, error);
            if (error == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found)
                return Result<void>::Failure(MakeError(UpdateOfflineSourceErrors::Unavailable));
            if (error || !std::filesystem::is_directory(status))
                return Result<void>::Failure(MakeError(UpdateOfflineSourceErrors::UnsafePath));
            return Result<void>::Success();
        }

        /** @brief Reads at most the canonical manifest schema bound from a regular root child. */
        [[nodiscard]] Result<std::string> ReadManifest(const std::filesystem::path &root) {
            const auto path = root / "manifest.json";
            if (auto checked = CheckType(path, std::filesystem::file_type::regular); checked.HasError())
                return Result<std::string>::Failure(checked.ErrorValue());
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size == 0U || size > MaximumManifestBytes)
                return Result<std::string>::Failure(MakeError(UpdateOfflineSourceErrors::UnsafePath));
            std::string bytes(static_cast<std::size_t>(size), '\0');
            if (std::ifstream stream(path, std::ios::binary); !stream ||
                                                              !stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
                                                              stream.peek() != std::char_traits<char>::eof() || stream.bad())
                return Result<std::string>::Failure(MakeError(UpdateOfflineSourceErrors::UnsafePath));
            return Result<std::string>::Success(std::move(bytes));
        }

        /** @brief Requires package content to live at its signed digest filename under a literal packages child. */
        [[nodiscard]] Result<std::filesystem::path> PackagePath(const std::filesystem::path &root, const UpdatePackageRecord &package) {
            const auto directory = root / "packages";
            const auto suffix = package.selection.format == DistributionPackageFormat::TarGzip ? ".tar.gz" : ".zip";
            const auto path = directory / (FormatSha256(package.digest).substr(7U) + suffix);
            if (auto checked = CheckType(directory, std::filesystem::file_type::directory); checked.HasError())
                return Result<std::filesystem::path>::Failure(checked.ErrorValue());
            if (auto checked = CheckType(path, std::filesystem::file_type::regular); checked.HasError())
                return Result<std::filesystem::path>::Failure(checked.ErrorValue());
            if (std::error_code error; std::filesystem::file_size(path, error) != package.size || error)
                return Result<std::filesystem::path>::Failure(MakeError(UpdateOfflineSourceErrors::MirrorMismatch));
            return Result<std::filesystem::path>::Success(path);
        }

        /** @brief Requires absent distinct private files and stage under a single protected directory. */
        [[nodiscard]] bool PrivatePathsReady(const UpdateOfflineImportRequest &request) {
            const auto &paths = request.privatePaths;
            const auto &stage = request.stageRoot;
            if (!paths.partialFile.is_absolute() || !paths.checkpointFile.is_absolute() || !stage.is_absolute() ||
                !NoTraversal(paths.partialFile) || !NoTraversal(paths.checkpointFile) || !NoTraversal(stage) ||
                paths.partialFile.parent_path() != paths.checkpointFile.parent_path() ||
                paths.partialFile.parent_path() != stage.parent_path() || paths.partialFile == paths.checkpointFile ||
                paths.partialFile == stage || paths.checkpointFile == stage ||
                CheckType(stage.parent_path(), std::filesystem::file_type::directory).HasError())
                return false;
            for (const auto &path : {paths.partialFile, paths.checkpointFile, stage}) {
                if (path.filename().empty())
                    return false;
                std::error_code error;
                const auto status = std::filesystem::symlink_status(path, error);
                if (status.type() != std::filesystem::file_type::not_found || (error && error != std::errc::no_such_file_or_directory))
                    return false;
            }
            return true;
        }

        /** @brief Copies exact source bytes into a durable private file without trusting the source name. */
        [[nodiscard]] Result<void> CopyPackage(const std::filesystem::path &source, const std::filesystem::path &destination,
                                               const std::uint64_t expectedBytes, NativeDurableFileSystem &files,
                                               const CancellationToken &cancellation) {
            std::ifstream input(source, std::ios::binary);
            if (!input)
                return Result<void>::Failure(MakeError(UpdateOfflineSourceErrors::UnsafePath));
            std::array<char, CopyChunkBytes> buffer{};
            std::uint64_t offset = 0U;
            while (offset < expectedBytes) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(UpdateTransferErrors::Cancelled));
                const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), expectedBytes - offset));
                input.read(buffer.data(), count);
                if (input.gcount() != count)
                    return Result<void>::Failure(MakeError(UpdateOfflineSourceErrors::MirrorMismatch));
                if (auto written = files.AppendPrivateDurable(destination, offset,
                                                              std::as_bytes(std::span{buffer.data(), static_cast<std::size_t>(count)}));
                    written.HasError())
                    return written;
                offset += static_cast<std::uint64_t>(count);
            }
            if (input.peek() != std::char_traits<char>::eof() || input.bad())
                return Result<void>::Failure(MakeError(UpdateOfflineSourceErrors::MirrorMismatch));
            return Result<void>::Success();
        }

        /** @brief Managed identities and fields are bounded before any source access. */
        [[nodiscard]] bool ValidSource(const UpdateOfflineSource &source) {
            using enum UpdateOfflineSourceKind;
            const bool validKind = source.kind == LocalDirectory || source.kind == RemovableMedia || source.kind == EnterpriseMirror;
            return validKind && IsValidDistributionIdentity(source.sourceId) && IsValidDistributionIdentity(source.channel) &&
                   source.root.is_absolute() && NoTraversal(source.root) && source.maximumExpiredManifestSeconds <= 30U * 24U * 60U * 60U;
        }

        /** @brief Removes private import bytes that cannot be trusted or resumed after a failed fresh import. */
        void DiscardPrivateImport(const UpdateDownloadPaths &paths, NativeDurableFileSystem &files) {
            auto prepared = paths.checkpointFile;
            prepared += ".next";
            static_cast<void>(files.RemoveDurable(prepared));
            static_cast<void>(files.RemoveDurable(paths.checkpointFile));
            static_cast<void>(files.RemoveDurable(paths.partialFile));
        }
    }  // namespace

    /** @copydoc OrderUpdateOfflineSources */
    Result<std::vector<const UpdateOfflineSource *>> OrderUpdateOfflineSources(const std::vector<UpdateOfflineSource> &sources) {
        std::vector<const UpdateOfflineSource *> ordered;
        ordered.reserve(sources.size());
        for (const auto &source : sources) {
            if (!ValidSource(source) || std::ranges::any_of(ordered, [&](const auto *other) {
                return other->sourceId == source.sourceId;
            }))
                return Result<std::vector<const UpdateOfflineSource *>>::Failure(MakeError(UpdateOfflineSourceErrors::InvalidPolicy));
            ordered.push_back(&source);
        }
        std::ranges::stable_sort(ordered, [](const auto *left, const auto *right) {
            return left->precedence < right->precedence;
        });
        return Result<std::vector<const UpdateOfflineSource *>>::Success(std::move(ordered));
    }

    /** @copydoc MayTryNextOfflineSource */
    bool MayTryNextOfflineSource(const Error &failure) noexcept {
        return failure.code.Value() == UpdateOfflineSourceErrors::Unavailable.code.Value();
    }

    namespace {
        /** @brief Imports one explicitly selected archive format without changing the source trust policy. */
        Result<ImportedOfflineUpdate> ImportOfflinePackage(const UpdateOfflineImportRequest &request, NativeDurableFileSystem &files,
                                                           std::shared_ptr<const Security::SignatureProvider> provider,
                                                           CancellationToken cancellation, const DistributionPackageFormat format) {
            if (!ValidSource(request.source) || request.admission.channel != request.source.channel)
                return Result<ImportedOfflineUpdate>::Failure(MakeError(UpdateOfflineSourceErrors::InvalidPolicy));
            if (cancellation.IsCancellationRequested())
                return Result<ImportedOfflineUpdate>::Failure(MakeError(UpdateTransferErrors::Cancelled));
            if (auto root = CheckRoot(request.source.root); root.HasError())
                return Result<ImportedOfflineUpdate>::Failure(root.ErrorValue());
            if (!PrivatePathsReady(request))
                return Result<ImportedOfflineUpdate>::Failure(MakeError(UpdateOfflineSourceErrors::UnsafePath));
            auto bytes = ReadManifest(request.source.root);
            if (bytes.HasError())
                return Result<ImportedOfflineUpdate>::Failure(bytes.ErrorValue());
            auto manifest = SignedUpdateManifest::ParseCanonical(bytes.Value());
            if (manifest.HasError())
                return Result<ImportedOfflineUpdate>::Failure(manifest.ErrorValue());

            auto admission = request.admission;
            admission.authorizedDowngrade = admission.authorizedDowngrade && request.source.allowDowngrade;
            const UpdatePackagePreferences preferences{{format}};
            const auto discovery = AssessUpdate(manifest.Value(), admission, request.roots, provider, preferences,
                                                {request.source.maximumExpiredManifestSeconds});
            if (discovery.status != UpdateDiscoveryStatus::Available || !discovery.package)
                return Result<ImportedOfflineUpdate>::Failure(discovery.failure.value_or(MakeError(UpdateOfflineSourceErrors::NoUpdate)));
            const UpdatePackageRecord package = *discovery.package;
            auto sourcePath = PackagePath(request.source.root, package);
            if (sourcePath.HasError())
                return Result<ImportedOfflineUpdate>::Failure(sourcePath.ErrorValue());
            auto available = files.AvailableBytes(request.privatePaths.partialFile.parent_path());
            if (available.HasError())
                return Result<ImportedOfflineUpdate>::Failure(available.ErrorValue());
            if (auto space = CheckUpdateTransferSpace(package, available.Value(), request.downloadLimits.maximumPackageBytes,
                                                      request.downloadLimits.reserveBytes);
                space.HasError())
                return Result<ImportedOfflineUpdate>::Failure(space.ErrorValue());
            if (auto copied = CopyPackage(sourcePath.Value(), request.privatePaths.partialFile, package.size, files, cancellation);
                copied.HasError()) {
                DiscardPrivateImport(request.privatePaths, files);
                return Result<ImportedOfflineUpdate>::Failure(copied.ErrorValue());
            }
            const UpdateTransferCheckpoint checkpoint{package.digest, package.size, package.size, package.url, package.url, {}};
            Security::ArtifactVerifier verifier{std::move(provider), request.roots.Roots()};
            if (auto verified = VerifyCompletedUpdateTransfer(package, checkpoint, request.privatePaths.partialFile, verifier);
                verified.HasError()) {
                DiscardPrivateImport(request.privatePaths, files);
                if (verified.ErrorValue().code.Value() == SecurityErrors::IntegrityMismatch.code.Value())
                    return Result<ImportedOfflineUpdate>::Failure(MakeError(UpdateOfflineSourceErrors::MirrorMismatch));
                return Result<ImportedOfflineUpdate>::Failure(verified.ErrorValue());
            }
            if (auto saved =
                    SaveUpdateTransferCheckpoint(files, request.privatePaths.partialFile, request.privatePaths.checkpointFile, checkpoint);
                saved.HasError()) {
                DiscardPrivateImport(request.privatePaths, files);
                return Result<ImportedOfflineUpdate>::Failure(saved.ErrorValue());
            }
            auto ready = format == DistributionPackageFormat::TarGzip
                             ? StageVerifiedTarGzipUpdate({package, checkpoint, request.privatePaths.partialFile, request.stageRoot,
                                                           request.archiveLimits},
                                                          files, verifier, cancellation)
                             : StageVerifiedZipUpdate({package, checkpoint, request.privatePaths.partialFile, request.stageRoot,
                                                       request.archiveLimits},
                                                      files, verifier, cancellation);
            if (ready.HasError()) {
                DiscardPrivateImport(request.privatePaths, files);
                return Result<ImportedOfflineUpdate>::Failure(ready.ErrorValue());
            }
            return Result<ImportedOfflineUpdate>::Success({package, checkpoint, std::move(ready).Value()});
        }
    }  // namespace

    /** @copydoc ImportOfflineZipUpdate */
    Result<ImportedOfflineUpdate> ImportOfflineZipUpdate(const UpdateOfflineImportRequest &request, NativeDurableFileSystem &files,
                                                         std::shared_ptr<const Security::SignatureProvider> provider,
                                                         CancellationToken cancellation) {
        return ImportOfflinePackage(request, files, std::move(provider), cancellation, DistributionPackageFormat::ZipArchive);
    }

    /** @copydoc ImportOfflineTarGzipUpdate */
    Result<ImportedOfflineUpdate> ImportOfflineTarGzipUpdate(const UpdateOfflineImportRequest &request, NativeDurableFileSystem &files,
                                                             std::shared_ptr<const Security::SignatureProvider> provider,
                                                             CancellationToken cancellation) {
        if (request.admission.platform != DistributionPlatform::Linux)
            return Result<ImportedOfflineUpdate>::Failure(MakeError(UpdateOfflineSourceErrors::InvalidPolicy));
        return ImportOfflinePackage(request, files, std::move(provider), cancellation, DistributionPackageFormat::TarGzip);
    }
}  // namespace Horo::Release
