/**
 * @file
 * @brief Initializes the sole durable bootstrap selector and recovers only bounded operation-owned staging under the writer lease.
 */
#include "../AssetErrors.h"
#include "AssetCookStorageInternal.h"
#include "Horo/Foundation/Sha256.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace Horo::Assets::CookStorageDetail {
    namespace {
        /** @brief Durably persists a virgin selector before any immutable promotion, then releases its exact private namespace. */
        Result<void> PersistUnpublishedSelector(const std::filesystem::path &root, const std::string_view baseline,
                                                const AssetCookPublicationPolicy &policy) {
            auto operation = CreateOperationRoot(root, policy);
            if (operation.HasError())
                return Result<void>::Failure(operation.ErrorValue());
            const auto prepared = operation.Value() / "unpublished.current.json";
            const auto bytes = std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t *>(baseline.data()), baseline.size()};
            if (auto written = WritePrivate(prepared, bytes, policy.files); written.HasError())
                return written;
            auto verified = ReadFile(*policy.files, prepared, bytes.size());
            if (verified.HasError())
                return Result<void>::Failure(verified.ErrorValue());
            if (!std::ranges::equal(verified.Value(), bytes))
                return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
            std::optional<Error> durabilityError;
            if (auto replaced = ReplaceDurably(policy.files, prepared, root / "current.json", &durabilityError); replaced.HasError())
                return replaced;
            if (durabilityError)
                return Result<void>::Failure(std::move(*durabilityError));
            return policy.files->RemoveDurable(operation.Value());
        }

        /** @brief Accepts only the exact filenames produced by private generation assembly. */
        bool IsPrivateGenerationFile(const std::filesystem::path &path) {
            const auto name = path.filename().string();
            if (name == "manifest.json")
                return true;
            if (!name.ends_with(".cooked"))
                return false;
            const auto id = AssetId::Parse(std::string_view{name}.substr(0, name.size() - 7U));
            return id.HasValue() && name == id.Value().ToString() + ".cooked";
        }

        /** @brief Collects only exact private files through the host's complete bounded inventory capability. */
        Result<void> CollectPrivateFiles(const DurableFileSystem &storage, const std::filesystem::path &directory,
                                         const AssetCookLimits &limits, std::size_t &budget, std::vector<std::filesystem::path> &paths) {
            auto entries = StorageInventory(storage, directory, std::max(std::size_t{1}, budget));
            if (entries.HasError())
                return Result<void>::Failure(entries.ErrorValue());
            for (const auto &entry : entries.Value()) {
                if (budget == 0U || entry.info.sizeBytes > limits.maximumArtifactBytes)
                    return Result<void>::Failure(MakeError(CookErrors::TooLarge));
                --budget;
                if (entry.info.kind != FileEntryKind::RegularFile || entry.info.hardLinks != 1U || !IsPrivateGenerationFile(entry.name))
                    return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
                paths.push_back(directory / entry.name);
            }
            return Result<void>::Success();
        }

        /** @brief Collects only plain selector temporaries beside the known private generation directory. */
        Result<void> CollectOperationSelectors(const DurableFileSystem &storage, const std::filesystem::path &operation,
                                               std::size_t &budget, std::vector<std::filesystem::path> &paths) {
            auto entries = StorageInventory(storage, operation, std::max(std::size_t{1}, budget + 1U));
            if (entries.HasError())
                return Result<void>::Failure(entries.ErrorValue());
            for (const auto &entry : entries.Value()) {
                if (entry.name == "generation")
                    continue;
                if (budget == 0U || (entry.name != "current.json" && entry.name != "unpublished.current.json") ||
                    entry.info.kind != FileEntryKind::RegularFile || entry.info.hardLinks != 1U ||
                    entry.info.sizeBytes > MaximumSelectorBytes)
                    return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
                --budget;
                paths.push_back(operation / entry.name);
            }
            return Result<void>::Success();
        }

        /** @brief Prevalidates one canonical operation and collects children before their containing directories. */
        Result<void> CollectAbandonedOperation(const DurableFileSystem &storage, const std::filesystem::path &operation,
                                               const AssetCookLimits &limits, std::size_t &budget,
                                               std::vector<std::filesystem::path> &files, std::vector<std::filesystem::path> &directories) {
            const auto invalid = [] {
                return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
            };
            if (const auto id = AssetId::Parse(operation.filename().string());
                id.HasError() || id.Value().ToString() != operation.filename().string())
                return invalid();
            auto metadata = InspectStorageEntry(storage, operation);
            if (metadata.HasError())
                return Result<void>::Failure(metadata.ErrorValue());
            if (!metadata.Value() || metadata.Value()->kind != FileEntryKind::Directory)
                return invalid();
            const auto generation = operation / "generation";
            auto content = InspectStorageEntry(storage, generation);
            if (content.HasError())
                return Result<void>::Failure(content.ErrorValue());
            if (content.Value()) {
                if (content.Value()->kind != FileEntryKind::Directory)
                    return invalid();
                if (auto collected = CollectPrivateFiles(storage, generation, limits, budget, files); collected.HasError())
                    return collected;
                directories.push_back(generation);
            }
            if (auto collected = CollectOperationSelectors(storage, operation, budget, files); collected.HasError())
                return collected;
            directories.push_back(operation);
            return Result<void>::Success();
        }

        /** @brief Collects only private staging that cannot have a live writer while the common lease is held. */
        Result<std::vector<std::filesystem::path>> CollectAbandonedStaging(const DurableFileSystem &storage,
                                                                           const std::filesystem::path &root,
                                                                           const AssetCookLimits &limits) {
            using Paths = std::vector<std::filesystem::path>;
            const auto staging = root / ".cook-staging";
            auto metadata = InspectStorageEntry(storage, staging);
            if (metadata.HasError())
                return Result<Paths>::Failure(metadata.ErrorValue());
            if (!metadata.Value())
                return Result<Paths>::Success({});
            if (metadata.Value()->kind != FileEntryKind::Directory || limits.maximumAssets > std::numeric_limits<std::size_t>::max() - 4U)
                return Result<Paths>::Failure(MakeError(CookErrors::MalformedArtifact));
            std::size_t budget = limits.maximumAssets + 4U;
            auto entries = StorageInventory(storage, staging, budget);
            if (entries.HasError())
                return Result<Paths>::Failure(entries.ErrorValue());
            Paths files;
            Paths directories;
            for (const auto &entry : entries.Value()) {
                if (budget == 0U || entry.info.kind != FileEntryKind::Directory)
                    return Result<Paths>::Failure(MakeError(CookErrors::MalformedArtifact));
                --budget;
                if (auto collected = CollectAbandonedOperation(storage, staging / entry.name, limits, budget, files, directories);
                    collected.HasError())
                    return Result<Paths>::Failure(collected.ErrorValue());
            }
            files.insert(files.end(), directories.begin(), directories.end());
            return Result<Paths>::Success(std::move(files));
        }

        /** @brief Resolves exactly the current active selector or verified unpublished baseline with the caller's aggregate bound. */
        Result<std::optional<AssetCookGeneration>> ResolveRecoveredPublication(const DurableFileSystem &storage,
                                                                               const std::filesystem::path &root,
                                                                               const AssetCookTargetId &target,
                                                                               const std::size_t maximumTotalBytes,
                                                                               const AssetCookLimits &limits) {
            auto pointer = ReadFile(storage, root / "current.json", std::min(limits.maximumArtifactBytes, MaximumSelectorBytes));
            if (pointer.HasError())
                return Result<std::optional<AssetCookGeneration>>::Failure(pointer.ErrorValue());
            if (const std::string_view text(reinterpret_cast<const char *>(pointer.Value().data()), pointer.Value().size());
                text == UnpublishedSelector(target))
                return Result<std::optional<AssetCookGeneration>>::Success({});
            auto current = ResolveCurrentCookGeneration(storage, root, limits);
            if (current.HasError())
                return Result<std::optional<AssetCookGeneration>>::Failure(current.ErrorValue());
            if (current.Value().target != target)
                return Result<std::optional<AssetCookGeneration>>::Failure(MakeError(CookErrors::MalformedArtifact));
            if (auto contents = ReadCookGenerationContents(storage, current.Value(), maximumTotalBytes, limits); contents.HasError())
                return Result<std::optional<AssetCookGeneration>>::Failure(contents.ErrorValue());
            return Result<std::optional<AssetCookGeneration>>::Success(std::move(current).Value());
        }
    }  // namespace

    /** @copydoc CreateOperationRoot */
    Result<std::filesystem::path> CreateOperationRoot(const std::filesystem::path &root, const AssetCookPublicationPolicy &policy) {
        std::string token = policy.operationId;
        if (token.empty() && policy.newOperationId) {
            auto generated = policy.newOperationId();
            if (generated.HasError())
                return Result<std::filesystem::path>::Failure(generated.ErrorValue());
            if (!generated.Value().IsValid())
                return Result<std::filesystem::path>::Failure(MakeError(CookErrors::MalformedArtifact));
            token = generated.Value().ToString();
        }
        if (token.empty()) {
            return Result<std::filesystem::path>::Failure(
                MakeError(CookErrors::MalformedArtifact, "Durable publication requires a host attempt identity."));
        }
        if (const auto id = AssetId::Parse(token); id.HasError() || id.Value().ToString() != token)
            return Result<std::filesystem::path>::Failure(MakeError(CookErrors::MalformedArtifact));
        const auto privateRoot = root / ".cook-staging";
        const auto operationRoot = privateRoot / token;
        if (auto inspected = InspectStorageEntry(*policy.files, operationRoot); inspected.HasError())
            return Result<std::filesystem::path>::Failure(inspected.ErrorValue());
        if (auto created = policy.files->CreateDirectoryPath(privateRoot, DirectoryCreationMode::Parents); created.HasError())
            return Result<std::filesystem::path>::Failure(created.ErrorValue());
        const auto created = policy.files->CreateDirectoryPath(operationRoot, DirectoryCreationMode::Single);
        if (created.HasError())
            return Result<std::filesystem::path>::Failure(created.ErrorValue());
        if (!created.Value())
            return Result<std::filesystem::path>::Failure(MakeError(CookErrors::MalformedArtifact));
        return Result<std::filesystem::path>::Success(operationRoot);
    }

    /** @copydoc EnsurePublicationBaseline */
    Result<void> EnsurePublicationBaseline(const std::filesystem::path &root, const AssetCookTargetId &target,
                                           const AssetCookLimits &limits, const AssetCookPublicationPolicy &policy) {
        const auto invalid = [] {
            return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
        };
        const auto current = root / "current.json";
        auto metadata = InspectStorageEntry(*policy.files, current);
        if (metadata.HasError())
            return Result<void>::Failure(metadata.ErrorValue());
        const auto baseline = UnpublishedSelector(target);
        if (baseline.size() > std::min(limits.maximumArtifactBytes, MaximumSelectorBytes))
            return Result<void>::Failure(MakeError(CookErrors::TooLarge));
        if (metadata.Value()) {
            auto bytes = ReadFile(*policy.files, current, std::min(limits.maximumArtifactBytes, MaximumSelectorBytes));
            if (bytes.HasError())
                return Result<void>::Failure(bytes.ErrorValue());
            if (const std::string_view text(reinterpret_cast<const char *>(bytes.Value().data()), bytes.Value().size()); text != baseline) {
                auto active = ResolveCurrentCookGeneration(*policy.files, root, limits);
                if (active.HasError())
                    return Result<void>::Failure(active.ErrorValue());
                if (active.Value().target != target)
                    return invalid();
            }
            return policy.files->SyncDirectory(root);
        }
        const auto generations = root / "generations";
        auto generation = InspectStorageEntry(*policy.files, generations);
        if (generation.HasError())
            return Result<void>::Failure(generation.ErrorValue());
        if (generation.Value()) {
            if (generation.Value()->kind != FileEntryKind::Directory)
                return invalid();
            auto inventory = StorageInventory(*policy.files, generations, 1U);
            if (inventory.HasError())
                return Result<void>::Failure(inventory.ErrorValue());
            if (!inventory.Value().empty())
                return invalid();
        }
        return PersistUnpublishedSelector(root, baseline, policy);
    }
}  // namespace Horo::Assets::CookStorageDetail

namespace Horo::Assets {
    using namespace CookStorageDetail;

    /** @copydoc RecoverCookPublication */
    Result<std::optional<AssetCookGeneration>> RecoverCookPublication(const std::filesystem::path &targetRoot,
                                                                      const AssetCookTargetId &target, const std::size_t maximumTotalBytes,
                                                                      const AssetCookLimits &limits,
                                                                      const AssetCookPublicationPolicy &policy) {
        const auto invalid = [] {
            return Result<std::optional<AssetCookGeneration>>::Failure(MakeError(CookErrors::MalformedArtifact));
        };
        if (!AdmittedLimits(limits) || maximumTotalBytes > MaximumGenerationBytes)
            return Result<std::optional<AssetCookGeneration>>::Failure(MakeError(CookErrors::TooLarge));
        if (!target.IsValid())
            return Result<std::optional<AssetCookGeneration>>::Failure(MakeError(AssetCookTargetErrors::Invalid));
        if (policy.files == nullptr || policy.writerLease == nullptr ||
            !policy.writerLease->ProtectsPath(targetRoot / ".cook-writer.lock") || maximumTotalBytes == 0U)
            return invalid();
        auto root = InspectStorageEntry(*policy.files, targetRoot);
        if (root.HasError())
            return Result<std::optional<AssetCookGeneration>>::Failure(root.ErrorValue());
        if (!root.Value() || root.Value()->kind != FileEntryKind::Directory)
            return invalid();
        auto initialized = EnsurePublicationBaseline(targetRoot, target, limits, policy);
        auto current = initialized.HasError() ? Result<std::optional<AssetCookGeneration>>::Failure(initialized.ErrorValue())
                                              : ResolveRecoveredPublication(*policy.files, targetRoot, target, maximumTotalBytes, limits);
        auto abandoned = CollectAbandonedStaging(*policy.files, targetRoot, limits);
        if (abandoned.HasError())
            return Result<std::optional<AssetCookGeneration>>::Failure(abandoned.ErrorValue());
        for (const auto &path : abandoned.Value()) {
            if (auto removed = policy.files->RemoveDurable(path); removed.HasError())
                return Result<std::optional<AssetCookGeneration>>::Failure(removed.ErrorValue());
        }
        // Exact private cleanup remains independent of, and precedes reporting, current authority failure.
        return current;
    }
}  // namespace Horo::Assets
