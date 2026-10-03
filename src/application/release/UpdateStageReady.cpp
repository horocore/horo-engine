#include "Horo/Release/UpdateStageReady.h"

#include "Horo/Release/UpdateTransferErrors.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <system_error>
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

        /** @brief Encodes only verified identities into one bounded version-private marker. */
        [[nodiscard]] Result<std::string> ReadyRecord(const UpdatePackageRecord &package, const std::span<const UpdateStagedFile> inventory,
                                                      const UpdateArchiveLimits &limits) {
            auto encoded = BuildCanonicalUpdateFileInventory(inventory, limits);
            if (encoded.HasError())
                return Result<std::string>::Failure(encoded.ErrorValue());
            const auto digest = ComputeSha256(std::as_bytes(std::span{encoded.Value()}));
            return Result<std::string>::Success(
                std::format("horo-update-stage-ready-v2\n{}\n{}\n{}\n", FormatSha256(package.digest), package.size, FormatSha256(digest)));
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
        auto record = ReadyRecord(package, inventory, limits);
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

    /** @copydoc VerifyReadyUpdateStage */
    Result<void> VerifyReadyUpdateStage(const UpdatePackageRecord &package, const UpdateTransferCheckpoint &checkpoint,
                                        const std::filesystem::path &packageFile, const std::filesystem::path &stageRoot,
                                        const std::span<const UpdateStagedFile> inventory, const UpdateArchiveLimits &limits,
                                        const Security::ArtifactVerifier &verifier) {
        const auto invalid = [] {
            return Result<void>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
        };
        if (!ValidStagePath(stageRoot, packageFile))
            return invalid();
        auto marker = stageRoot;
        marker += ".ready";
        if (packageFile == marker)
            return invalid();
        auto expected = ReadyRecord(package, inventory, limits);
        if (expected.HasError())
            return Result<void>::Failure(expected.ErrorValue());
        std::error_code error;
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(marker, error)) || error)
            return invalid();
        if (std::filesystem::hard_link_count(marker, error) != 1U || error)
            return invalid();
        if (std::filesystem::file_size(marker, error) != expected.Value().size() || error)
            return invalid();
        std::ifstream input(marker, std::ios::binary);
        std::string actual(expected.Value().size(), '\0');
        input.read(actual.data(), static_cast<std::streamsize>(actual.size()));
        if (!input || actual != expected.Value())
            return invalid();
        if (auto verified = VerifyCompletedUpdateTransfer(package, checkpoint, packageFile, verifier); verified.HasError())
            return verified;
        return VerifyUpdateStagedTree(stageRoot, inventory, limits);
    }
}  // namespace Horo::Release
