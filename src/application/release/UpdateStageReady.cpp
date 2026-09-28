#include "Horo/Release/UpdateStageReady.h"

#include "Horo/Release/UpdateTransferErrors.h"

#include <algorithm>
#include <format>
#include <span>
#include <string>
#include <vector>

namespace Horo::Release {
    namespace {
        /** @brief Rejects lexical escape and aliases in version-private stage paths. */
        [[nodiscard]] bool ValidStagePath(const std::filesystem::path &stageRoot, const std::filesystem::path &packageFile) {
            if (!stageRoot.is_absolute() || !packageFile.is_absolute() || stageRoot.filename().empty() || stageRoot == packageFile ||
                packageFile == stageRoot.parent_path())
                return false;
            for (const auto &path : {stageRoot, packageFile}) {
                for (const auto &part : path) {
                    if (part == "." || part == "..")
                        return false;
                }
            }
            return true;
        }

        /** @brief Hashes canonical ordered inventory rows without buffering file content. */
        [[nodiscard]] Result<Sha256Digest> InventoryDigest(const std::span<const UpdateStagedFile> inventory) {
            std::vector<const UpdateStagedFile *> ordered;
            ordered.reserve(inventory.size());
            for (const auto &file : inventory)
                ordered.emplace_back(&file);
            std::ranges::sort(ordered, {}, [](const UpdateStagedFile *file) -> const std::string & {
                return file->path;
            });
            Sha256Builder hash;
            for (const auto *file : ordered) {
                const std::string row = std::format("{}\t{}\t{}\n", file->path, file->size, FormatSha256(file->digest));
                if (!hash.Update(std::as_bytes(std::span{row})))
                    return Result<Sha256Digest>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
            }
            return Result<Sha256Digest>::Success(hash.Finalize());
        }

        /** @brief Encodes only verified identities into one bounded version-private marker. */
        [[nodiscard]] Result<std::string> ReadyRecord(const UpdatePackageRecord &package,
                                                      const std::span<const UpdateStagedFile> inventory) {
            auto inventoryHash = InventoryDigest(inventory);
            if (inventoryHash.HasError())
                return Result<std::string>::Failure(inventoryHash.ErrorValue());
            return Result<std::string>::Success(std::format("horo-update-stage-ready-v1\n{}\n{}\n{}\n", FormatSha256(package.digest),
                                                            package.size, FormatSha256(inventoryHash.Value())));
        }

        /** @brief Makes absence of a stale marker durable before any new validation. */
        [[nodiscard]] Result<void> ClearMarkers(NativeDurableFileSystem &files, const std::filesystem::path &ready,
                                                const std::filesystem::path &prepared) {
            if (auto removed = files.RemoveDurable(ready); removed.HasError())
                return removed;
            return files.RemoveDurable(prepared);
        }
    }  // namespace

    /** @copydoc PublishVerifiedUpdateStage */
    Result<std::filesystem::path> PublishVerifiedUpdateStage(const VerifiedUpdateStageRequest &request, NativeDurableFileSystem &files,
                                                             const Security::ArtifactVerifier &verifier, CancellationToken cancellation) {
        const auto &[package, checkpoint, packageFile, stageRoot, inventory, limits] = request;
        const auto failed = [](const ErrorCodeDescriptor &code) {
            return Result<std::filesystem::path>::Failure(MakeError(code));
        };
        if (!ValidStagePath(stageRoot, packageFile))
            return failed(UpdateTransferErrors::StageMismatch);
        auto ready = stageRoot;
        ready += ".ready";
        auto prepared = ready;
        prepared += ".prepared";
        if (packageFile == ready || packageFile == prepared)
            return failed(UpdateTransferErrors::StageMismatch);
        if (auto cleared = ClearMarkers(files, ready, prepared); cleared.HasError())
            return Result<std::filesystem::path>::Failure(cleared.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return failed(UpdateTransferErrors::Cancelled);
        if (auto verified = VerifyCompletedUpdateTransfer(package, checkpoint, packageFile, verifier); verified.HasError())
            return Result<std::filesystem::path>::Failure(verified.ErrorValue());
        if (auto verified = VerifyUpdateStagedTree(stageRoot, inventory, limits); verified.HasError())
            return Result<std::filesystem::path>::Failure(verified.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return failed(UpdateTransferErrors::Cancelled);
        auto record = ReadyRecord(package, inventory);
        if (record.HasError())
            return Result<std::filesystem::path>::Failure(record.ErrorValue());
        if (auto written = files.AppendPrivateDurable(prepared, 0U, std::as_bytes(std::span{record.Value()})); written.HasError())
            return Result<std::filesystem::path>::Failure(written.ErrorValue());
        if (cancellation.IsCancellationRequested()) {
            static_cast<void>(files.RemoveDurable(prepared));
            return failed(UpdateTransferErrors::Cancelled);
        }
        if (auto published = files.AtomicReplace(prepared, ready); published.HasError())
            return Result<std::filesystem::path>::Failure(published.ErrorValue());
        return Result<std::filesystem::path>::Success(std::move(ready));
    }
}  // namespace Horo::Release
