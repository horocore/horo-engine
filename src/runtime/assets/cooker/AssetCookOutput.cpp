/**
 * @file
 * @brief Publishes verified immutable cook generations and tracks the sole current selector commit and live adoption.
 */
#include "../AssetErrors.h"
#include "AssetCookStorageInternal.h"
#include "Horo/Foundation/Sha256.h"

#include <algorithm>
#include <format>
#include <limits>
#include <optional>
#include <utility>

namespace Horo::Assets::CookStorageDetail {
    namespace {
        /** @brief Writes the exact staged files before publishing the mutable current pointer. */
        [[nodiscard]] Result<void> WriteGenerationFiles(const std::filesystem::path &root,
                                                        const std::span<const AssetCookManifestEntry> entries,
                                                        const std::span<const std::vector<std::uint8_t>> payloads,
                                                        const std::span<const std::uint8_t> manifestBytes, DurableFileSystem *files) {
            for (std::size_t i = 0; i < entries.size(); ++i) {
                if (!IsSafeArtifactFile(entries[i].artifactFile))
                    return Result<void>::Failure(Error{CookErrors::MalformedArtifact.code});
                const auto artifactPath = root / entries[i].artifactFile;
                if (!IsSafePathWithin(root, artifactPath))
                    return Result<void>::Failure(Error{CookErrors::MalformedArtifact.code});
                auto writeResult = WritePrivate(artifactPath, payloads[i], files);
                if (writeResult.HasError())
                    return writeResult;
            }
            return WritePrivate(root / "manifest.json", manifestBytes, files);
        }

        /** @brief Verifies the complete inventory ordering and encoded payload bounds before staging. */
        Result<void> ValidateGenerationEntries(const std::span<const AssetCookManifestEntry> entries,
                                               const std::span<const std::vector<std::uint8_t>> artifactPayloads,
                                               const AssetCookTargetId &target, const AssetCookLimits &limits) {
            if (!AdmittedLimits(limits))
                return Result<void>::Failure(MakeError(CookErrors::TooLarge));
            if (!target.IsValid())
                return Result<void>::Failure(MakeError(AssetCookTargetErrors::Invalid));
            if (entries.size() != artifactPayloads.size()) {
                return Result<void>::Failure(Error{CookErrors::MalformedArtifact.code});
            }
            if (entries.size() > limits.maximumAssets)
                return Result<void>::Failure(MakeError(CookErrors::TooLarge));

            // Verify entries are sorted and have no duplicate IDs
            for (std::size_t i = 1; i < entries.size(); ++i) {
                if (entries[i].assetId <= entries[i - 1].assetId) {
                    return Result<void>::Failure(Error{CookErrors::DuplicateCooker.code});
                }
            }

            // Verify artifact payloads are within bounds
            std::size_t totalBytes{};
            for (std::size_t index = 0; index < entries.size(); ++index) {
                const auto &entry = entries[index];
                const auto &payload = artifactPayloads[index];
                if (entry.artifactFile != entry.assetId.ToString() + ".cooked")
                    return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
                if (payload.size() > limits.maximumArtifactBytes || payload.size() > MaximumGenerationBytes - totalBytes) {
                    return Result<void>::Failure(Error{CookErrors::TooLarge.code});
                }
                totalBytes += payload.size();
                if (auto verified = VerifyArtifactEnvelope(payload, entry, target, limits); verified.HasError())
                    return verified;
            }

            return Result<void>::Success();
        }

        /** @brief Verifies every immutable existing byte before reusing a deterministic generation. */
        Result<void> VerifyExistingGeneration(const AssetCookGeneration &generation,
                                              const std::span<const std::vector<std::uint8_t>> payloads, const AssetCookLimits &limits) {
            std::size_t total{};
            for (const auto &payload : payloads) {
                if (payload.size() > std::numeric_limits<std::size_t>::max() - total)
                    return Result<void>::Failure(MakeError(CookErrors::TooLarge));
                total += payload.size();
            }
            auto existing = ReadCookGenerationContents(generation, total, limits);
            if (existing.HasError())
                return Result<void>::Failure(existing.ErrorValue());
            if (!std::ranges::equal(existing.Value().artifacts, payloads))
                return Result<void>::Failure(MakeError(CookErrors::HashMismatch));
            return Result<void>::Success();
        }

        /** @brief Verifies reusable immutable content or durably assembles and promotes a fresh private generation. */
        Result<void> AssembleImmutableGeneration(const AssetCookGeneration &generation, const std::filesystem::path &operationRoot,
                                                 const std::span<const AssetCookManifestEntry> entries,
                                                 const std::span<const std::vector<std::uint8_t>> payloads,
                                                 const std::span<const std::uint8_t> manifestBytes, const AssetCookLimits &limits,
                                                 DurableFileSystem *files) {
            std::error_code directoryError;
            const auto privateGeneration = operationRoot / "generation";
            if (!std::filesystem::create_directory(privateGeneration, directoryError) || directoryError)
                return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
            if (const auto existingStatus = std::filesystem::symlink_status(generation.generationRoot, directoryError);
                std::filesystem::exists(existingStatus)) {
                if (directoryError || !std::filesystem::is_directory(existingStatus))
                    return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
                if (auto verified = VerifyExistingGeneration(generation, payloads, limits); verified.HasError())
                    return Result<void>::Failure(verified.ErrorValue());
            } else {
                if (directoryError && directoryError != std::errc::no_such_file_or_directory)
                    return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
                if (auto written = WriteGenerationFiles(privateGeneration, entries, payloads, manifestBytes, files); written.HasError())
                    return Result<void>::Failure(written.ErrorValue());
                auto stagedGeneration = generation;
                stagedGeneration.generationRoot = privateGeneration;
                if (auto verified = VerifyExistingGeneration(stagedGeneration, payloads, limits); verified.HasError())
                    return Result<void>::Failure(verified.ErrorValue());
                if (auto synced = files->SyncDirectory(privateGeneration); synced.HasError())
                    return Result<void>::Failure(synced.ErrorValue());
                if (auto promoted = ReplaceDurably(files, privateGeneration, generation.generationRoot, nullptr); promoted.HasError())
                    return Result<void>::Failure(promoted.ErrorValue());
                if (auto synced = files->SyncDirectory(operationRoot); synced.HasError())
                    return Result<void>::Failure(synced.ErrorValue());
            }

            return Result<void>::Success();
        }

        /** @brief Prepares the verified selector and fences freshness immediately before tracked replacement and live adoption. */
        Result<void> CommitGenerationSelector(const std::filesystem::path &targetRoot, const std::filesystem::path &operationRoot,
                                              AssetCookGeneration &generation, const std::string &genRelPath,
                                              const AssetCookPublicationPolicy &policy) {
            // Build and write current.json atomically
            if (auto synced = policy.files->SyncDirectory(targetRoot); synced.HasError())
                return Result<void>::Failure(synced.ErrorValue());
            const std::string currentStr =
                std::format(R"({{"schemaVersion":1,"target":"{}","manifestDigest":"{}","generationPath":"{}","artifactCount":"{}"}})",
                            generation.target.Value(), HexEncodeSha256(generation.manifestDigest), genRelPath, generation.artifactCount);
            auto currentBytes = std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t *>(currentStr.data()),
                                                          reinterpret_cast<const std::uint8_t *>(currentStr.data()) + currentStr.size());

            const auto preparedCurrent = operationRoot / "current.json";
            if (auto written = WritePrivate(preparedCurrent, currentBytes, policy.files); written.HasError())
                return Result<void>::Failure(written.ErrorValue());
            if (auto verified = ReadFile(preparedCurrent, currentBytes.size()); verified.HasError() || verified.Value() != currentBytes)
                return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
            if (policy.prepareCommit) {
                if (auto prepared = policy.prepareCommit(generation); prepared.HasError())
                    return Result<void>::Failure(prepared.ErrorValue());
            }
            if (policy.beforeCommit) {
                if (auto accepted = policy.beforeCommit(); accepted.HasError())
                    return Result<void>::Failure(accepted.ErrorValue());
            }
            if (auto replaced = ReplaceDurably(policy.files, preparedCurrent, targetRoot / "current.json", &generation.durabilityError);
                replaced.HasError())
                return Result<void>::Failure(replaced.ErrorValue());
            if (policy.afterCommit)
                policy.afterCommit(generation);

            return Result<void>::Success();
        }

        /** @brief Gives the native receipt precedence over an adapter error or exception after replacement. */
        Result<void> ResolveReplacementOutcome(Result<void> replaced, const AtomicFileReplacementReceipt &receipt,
                                               std::optional<Error> *postCommitError) {
            if (postCommitError != nullptr) {
                if (receipt.WasCommitted()) {
                    if (replaced.HasError())
                        *postCommitError = std::move(replaced).ErrorValue();
                    return Result<void>::Success();
                }
                if (replaced.HasValue())
                    return Result<void>::Failure(
                        MakeError(CookErrors::MalformedArtifact, "Replacement returned no native commit receipt."));
            }
            return replaced;
        }
    }  // namespace

    /** @copydoc ReplaceDurably */
    Result<void> ReplaceDurably(DurableFileSystem *files, const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                std::optional<Error> *postCommitError) {
        AtomicFileReplacementReceipt receipt;
        auto replaced = Result<void>::Failure(MakeError(CookErrors::MalformedArtifact, "Filesystem replacement raised an exception."));
        try {
            replaced = postCommitError == nullptr ? files->AtomicReplace(prepared, destination)
                                                  : files->AtomicReplaceTracked(prepared, destination, receipt);
        } catch (...) {
            // External filesystem adapters cannot erase the true commit point by throwing after replacement.
            return ResolveReplacementOutcome(std::move(replaced), receipt, postCommitError);
        }
        return ResolveReplacementOutcome(std::move(replaced), receipt, postCommitError);
    }

    /** @copydoc WritePrivate */
    Result<void> WritePrivate(const std::filesystem::path &path, const std::span<const std::uint8_t> bytes, DurableFileSystem *files) {
        std::error_code error;
        if (const auto status = std::filesystem::symlink_status(path, error); !HasPlainPath(path) ||
                                                                              status.type() != std::filesystem::file_type::not_found ||
                                                                              (error && error != std::errc::no_such_file_or_directory))
            return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
        return files->WriteDurable(path, std::as_bytes(bytes));
    }
}  // namespace Horo::Assets::CookStorageDetail

namespace Horo::Assets {
    using namespace CookStorageDetail;

    /** @copydoc PublishCookGeneration */
    Result<AssetCookGeneration> PublishCookGeneration(const std::filesystem::path &targetRoot, const AssetCookTargetId &target,
                                                      std::span<const AssetCookManifestEntry> entries,
                                                      std::span<const std::vector<std::uint8_t>> artifactPayloads,
                                                      const AssetCookLimits &limits, const AssetCookPublicationPolicy &policy) {
        if (policy.files == nullptr || policy.writerLease == nullptr || !policy.writerLease->ProtectsPath(targetRoot / ".cook-writer.lock"))
            return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::MalformedArtifact));
        if (const auto valid = ValidateGenerationEntries(entries, artifactPayloads, target, limits); valid.HasError())
            return Result<AssetCookGeneration>::Failure(valid.ErrorValue());
        if (!HasPlainPath(targetRoot))
            return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::MalformedArtifact));
        if (auto initialized = EnsurePublicationBaseline(targetRoot, target, limits, policy); initialized.HasError())
            return Result<AssetCookGeneration>::Failure(initialized.ErrorValue());

        // Build manifest JSON
        auto manifestJson = BuildManifestJson(target.Value(), entries);
        auto manifestBytes =
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(manifestJson.data()), manifestJson.size());
        auto manifestDigest = ComputeSha256(std::as_bytes(manifestBytes));
        if (manifestBytes.size() > limits.maximumArtifactBytes)
            return Result<AssetCookGeneration>::Failure(MakeError(CookErrors::TooLarge));

        auto genRelPath = std::string("generations/") + HexEncodeSha256(manifestDigest);
        auto genRoot = targetRoot / genRelPath;

        if (!HasPlainPath(genRoot) || !IsSafePathWithin(targetRoot, genRoot))
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});
        std::error_code directoryError;
        std::filesystem::create_directories(genRoot.parent_path(), directoryError);
        if (directoryError)
            return Result<AssetCookGeneration>::Failure(Error{CookErrors::MalformedArtifact.code});
        auto operation = CreateOperationRoot(targetRoot, policy);
        if (operation.HasError())
            return Result<AssetCookGeneration>::Failure(operation.ErrorValue());
        AssetCookGeneration generation{.target = target,
                                       .manifestDigest = manifestDigest,
                                       .generationRoot = genRoot,
                                       .artifactCount = entries.size()};
        if (auto assembled =
                AssembleImmutableGeneration(generation, operation.Value(), entries, artifactPayloads, manifestBytes, limits, policy.files);
            assembled.HasError())
            return Result<AssetCookGeneration>::Failure(assembled.ErrorValue());
        if (auto committed = CommitGenerationSelector(targetRoot, operation.Value(), generation, genRelPath, policy); committed.HasError())
            return Result<AssetCookGeneration>::Failure(committed.ErrorValue());
        // The next locked recovery removes empty owned staging. No fallible filesystem work follows live adoption.
        return Result<AssetCookGeneration>::Success(std::move(generation));
    }
}  // namespace Horo::Assets
