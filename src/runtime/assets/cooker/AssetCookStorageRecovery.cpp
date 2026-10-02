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
        Result<void> PersistUnpublishedSelector(const std::filesystem::path &root, const std::string &baseline,
                                                const AssetCookPublicationPolicy &policy) {
            auto operation = CreateOperationRoot(root, policy);
            if (operation.HasError())
                return Result<void>::Failure(operation.ErrorValue());
            const auto prepared = operation.Value() / "unpublished.current.json";
            const auto bytes = std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t *>(baseline.data()), baseline.size()};
            if (auto written = WritePrivate(prepared, bytes, policy.files); written.HasError())
                return written;
            auto verified = ReadFile(prepared, bytes.size());
            if (verified.HasError() || !std::ranges::equal(verified.Value(), bytes))
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

        /** @brief Collects a bounded, fully prevalidated exact directory inventory without recursion or link traversal. */
        Result<void> CollectPrivateFiles(const std::filesystem::path &directory, const AssetCookLimits &limits, std::size_t &budget,
                                         std::vector<std::filesystem::path> &paths, const bool generation) {
            std::error_code error;
            std::filesystem::directory_iterator iterator(directory, error);
            const std::filesystem::directory_iterator end;
            if (error)
                return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
            for (; iterator != end; iterator.increment(error)) {
                if (error || budget == 0U)
                    return Result<void>::Failure(MakeError(CookErrors::TooLarge));
                --budget;
                const auto path = iterator->path();
                if (!IsPlainFile(path) || (generation ? !IsPrivateGenerationFile(path) : path.filename() != "current.json"))
                    return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
                const auto size = std::filesystem::file_size(path, error);
                if (error || size > limits.maximumArtifactBytes)
                    return Result<void>::Failure(MakeError(CookErrors::TooLarge));
                paths.push_back(path);
            }
            if (error)
                return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
            return Result<void>::Success();
        }

        /** @brief Collects only bounded, plain selector temporaries beside the known private generation directory. */
        Result<void> CollectOperationSelectors(const std::filesystem::path &operation, std::size_t &budget,
                                               std::vector<std::filesystem::path> &files) {
            const auto invalid = [] {
                return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
            };
            std::error_code error;
            std::filesystem::directory_iterator iterator(operation, error);
            const std::filesystem::directory_iterator end;
            if (error)
                return invalid();
            for (; iterator != end; iterator.increment(error)) {
                if (error)
                    return invalid();
                const auto path = iterator->path();
                if (path.filename() == "generation")
                    continue;
                if (budget == 0U || (path.filename() != "current.json" && path.filename() != "unpublished.current.json") ||
                    !IsPlainFile(path))
                    return invalid();
                --budget;
                if (std::filesystem::file_size(path, error) > MaximumSelectorBytes || error)
                    return invalid();
                files.push_back(path);
            }
            return error ? invalid() : Result<void>::Success();
        }

        /** @brief Validates one canonical operation namespace and collects children before their exact containing directories. */
        Result<void> CollectAbandonedOperation(const std::filesystem::path &operation, const AssetCookLimits &limits, std::size_t &budget,
                                               std::vector<std::filesystem::path> &files, std::vector<std::filesystem::path> &directories) {
            const auto invalid = [] {
                return Result<void>::Failure(MakeError(CookErrors::MalformedArtifact));
            };
            std::error_code error;
            const auto id = AssetId::Parse(operation.filename().string());
            if (id.HasError() || id.Value().ToString() != operation.filename().string() || !HasPlainPath(operation) ||
                !std::filesystem::is_directory(std::filesystem::symlink_status(operation, error)) || error)
                return invalid();
            // The only nested directory is exactly generation; examine and remove it explicitly.
            const auto generation = operation / "generation";
            const auto status = std::filesystem::symlink_status(generation, error);
            if (std::filesystem::exists(status)) {
                if (error || !HasPlainPath(generation) || !std::filesystem::is_directory(status))
                    return invalid();
                if (auto collected = CollectPrivateFiles(generation, limits, budget, files, true); collected.HasError())
                    return collected;
                directories.push_back(generation);
            } else if (error && error != std::errc::no_such_file_or_directory) {
                return invalid();
            }
            if (auto collected = CollectOperationSelectors(operation, budget, files); collected.HasError())
                return collected;
            directories.push_back(operation);
            return Result<void>::Success();
        }

        /** @brief Collects operation-owned staging whose writers cannot be live while the common native lock is held. */
        Result<std::vector<std::filesystem::path>> CollectAbandonedStaging(const std::filesystem::path &root,
                                                                           const AssetCookLimits &limits) {
            const auto staging = root / ".cook-staging";
            const auto invalid = [] {
                return Result<std::vector<std::filesystem::path>>::Failure(MakeError(CookErrors::MalformedArtifact));
            };
            if (!HasPlainPath(staging))
                return invalid();
            std::error_code error;
            const auto status = std::filesystem::symlink_status(staging, error);
            if (status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory))
                return Result<std::vector<std::filesystem::path>>::Success({});
            if (error || !std::filesystem::is_directory(status))
                return invalid();
            std::vector<std::filesystem::path> files;
            std::vector<std::filesystem::path> directories;
            if (limits.maximumAssets > std::numeric_limits<std::size_t>::max() - 4U)
                return invalid();
            std::size_t budget = limits.maximumAssets + 4U;
            std::filesystem::directory_iterator iterator(staging, error);
            const std::filesystem::directory_iterator end;
            if (error)
                return invalid();
            for (; iterator != end; iterator.increment(error)) {
                if (error || budget == 0U)
                    return invalid();
                --budget;
                if (auto collected = CollectAbandonedOperation(iterator->path(), limits, budget, files, directories); collected.HasError())
                    return Result<std::vector<std::filesystem::path>>::Failure(collected.ErrorValue());
            }
            if (error)
                return invalid();
            files.insert(files.end(), directories.begin(), directories.end());
            return Result<std::vector<std::filesystem::path>>::Success(std::move(files));
        }

        /** @brief Resolves exactly the current active selector or verified unpublished baseline with the caller's aggregate bound. */
        Result<std::optional<AssetCookGeneration>> ResolveRecoveredPublication(const std::filesystem::path &root,
                                                                               const AssetCookTargetId &target,
                                                                               const std::size_t maximumTotalBytes,
                                                                               const AssetCookLimits &limits) {
            auto pointer = ReadFile(root / "current.json", std::min(limits.maximumArtifactBytes, MaximumSelectorBytes));
            if (pointer.HasError())
                return Result<std::optional<AssetCookGeneration>>::Failure(pointer.ErrorValue());
            const std::string_view text(reinterpret_cast<const char *>(pointer.Value().data()), pointer.Value().size());
            if (text == UnpublishedSelector(target))
                return Result<std::optional<AssetCookGeneration>>::Success({});
            auto current = ResolveCurrentCookGeneration(root, limits);
            if (current.HasError())
                return Result<std::optional<AssetCookGeneration>>::Failure(current.ErrorValue());
            if (current.Value().target != target)
                return Result<std::optional<AssetCookGeneration>>::Failure(MakeError(CookErrors::MalformedArtifact));
            if (auto contents = ReadCookGenerationContents(current.Value(), maximumTotalBytes, limits); contents.HasError())
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
        if (!HasPlainPath(operationRoot))
            return Result<std::filesystem::path>::Failure(MakeError(CookErrors::MalformedArtifact));
        std::error_code error;
        std::filesystem::create_directories(privateRoot, error);
        if (error || !std::filesystem::create_directory(operationRoot, error) || error)
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
        if (!HasPlainPath(current))
            return invalid();
        std::error_code error;
        const auto status = std::filesystem::symlink_status(current, error);
        const auto baseline = UnpublishedSelector(target);
        if (baseline.size() > std::min(limits.maximumArtifactBytes, MaximumSelectorBytes))
            return Result<void>::Failure(MakeError(CookErrors::TooLarge));
        if (std::filesystem::exists(status)) {
            auto bytes = ReadFile(current, std::min(limits.maximumArtifactBytes, MaximumSelectorBytes));
            if (bytes.HasError())
                return Result<void>::Failure(bytes.ErrorValue());
            const std::string_view text(reinterpret_cast<const char *>(bytes.Value().data()), bytes.Value().size());
            if (text != baseline) {
                auto active = ResolveCurrentCookGeneration(root, limits);
                if (active.HasError())
                    return Result<void>::Failure(active.ErrorValue());
                if (active.Value().target != target)
                    return invalid();
            }
            return policy.files->SyncDirectory(root);
        }
        if (error && error != std::errc::no_such_file_or_directory)
            return invalid();
        const auto generations = root / "generations";
        if (!HasPlainPath(generations))
            return invalid();
        const auto generationStatus = std::filesystem::symlink_status(generations, error);
        if (std::filesystem::exists(generationStatus)) {
            if (error || !std::filesystem::is_directory(generationStatus) || !std::filesystem::is_empty(generations, error) || error)
                return invalid();
        } else if (error && error != std::errc::no_such_file_or_directory) {
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
            !policy.writerLease->ProtectsPath(targetRoot / ".cook-writer.lock") || maximumTotalBytes == 0U || !HasPlainPath(targetRoot))
            return invalid();
        auto initialized = EnsurePublicationBaseline(targetRoot, target, limits, policy);
        auto current = initialized.HasError() ? Result<std::optional<AssetCookGeneration>>::Failure(initialized.ErrorValue())
                                              : ResolveRecoveredPublication(targetRoot, target, maximumTotalBytes, limits);
        auto abandoned = CollectAbandonedStaging(targetRoot, limits);
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
