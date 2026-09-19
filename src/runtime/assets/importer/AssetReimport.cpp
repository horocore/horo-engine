/**
 * @copydoc AssetReimport.h
 */

#include "Horo/Assets/AssetReimport.h"

#include "../AssetErrors.h"
#include "AssetImportFileFacts.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/PathUtils.h"

#include <atomic>
#include <chrono>
#include <span>

namespace Horo::Assets {
    namespace {
        std::atomic_uint64_t g_reimportTemporarySequence{0};  // NOSONAR(cpp:S5421)

        [[nodiscard]] bool HasPathPrefix(const std::filesystem::path &root, const std::filesystem::path &candidate) {
            return Horo::Foundation::Paths::HasPathPrefix(root, candidate);
        }

        [[nodiscard]] std::filesystem::path TemporarySibling(const std::filesystem::path &destination, const std::string_view role) {
            const std::uint64_t sequence = ++g_reimportTemporarySequence;
            return destination.parent_path() / std::format(".{}.horo-reimport-{}-{}", destination.filename().string(), sequence, role);
        }

        [[nodiscard]] std::span<const std::byte> AsBytes(const std::vector<std::uint8_t> &bytes) {
            return {
                reinterpret_cast<const std::byte *>(bytes.data()),
                bytes.size(),
            };
        }

        [[nodiscard]] std::span<const std::byte> AsBytes(const std::string_view text) {
            return {
                reinterpret_cast<const std::byte *>(text.data()),
                text.size(),
            };
        }

        void BestEffortRemove(DurableFileSystem &files, const std::filesystem::path &path) {
            std::error_code error;
            if (std::filesystem::exists(path, error) && !error)
                static_cast<void>(files.RemoveDurable(path));
        }

        [[nodiscard]] Result<void> RestorePair(DurableFileSystem &files, const std::filesystem::path &payloadBackup,
                                               const std::filesystem::path &payload, const std::filesystem::path &metadataBackup,
                                               const std::filesystem::path &metadata) {
            if (auto restored = files.AtomicReplace(payloadBackup, payload); restored.HasError())
                return restored;
            return files.AtomicReplace(metadataBackup, metadata);
        }

        [[nodiscard]] std::vector<AssetImportReason> ComputeReimportReasons(const AssetImportMetadata &metadata,
                                                                            const AssetImporterContribution &contribution,
                                                                            const std::string_view sourceHash) {
            using enum AssetImportReason;
            std::vector<AssetImportReason> reasons;
            if (!metadata.sourceHash.empty() && metadata.sourceHash != sourceHash)
                reasons.push_back(SourceChanged);
            if (metadata.importerVersion != contribution.version)
                reasons.push_back(ImporterChanged);
            if (metadata.importerModuleId != contribution.moduleId || metadata.importerModuleVersion != contribution.moduleVersion)
                reasons.push_back(ModuleChanged);
            if (reasons.empty())
                reasons.push_back(ManualReimport);
            return reasons;
        }

        struct ReimportPaths final {
            std::filesystem::path projectRoot;
            std::filesystem::path assetPath;
            std::filesystem::path metadataPath;
        };

        struct PreparedReimport final {
            std::vector<std::uint8_t> source;
            std::string sourceHash;
            std::vector<AssetImportReason> reasons;
            PreparedAssetImport asset;
        };

        /** @brief Validates and normalizes the project-owned reimport paths. */
        [[nodiscard]] Result<ReimportPaths> ResolveReimportPaths(const AssetReimportRequest &request) {
            ReimportPaths paths{
                .projectRoot = Detail::NormalizeAssetImportPath(request.absoluteProjectRoot),
                .assetPath = Detail::NormalizeAssetImportPath(request.absoluteAssetPath),
            };
            const std::filesystem::path assetRoot = Detail::NormalizeAssetImportPath(paths.projectRoot / "assets");
            if (paths.projectRoot.empty() || assetRoot.empty() || paths.assetPath.empty() || !request.absoluteAssetPath.is_absolute() ||
                !HasPathPrefix(assetRoot, paths.assetPath)) {
                return Result<ReimportPaths>::Failure(
                    MakeError(AssetErrors::SourceMissing, "Reimport target is outside the project asset root."));
            }

            std::error_code error;
            const auto targetStatus = std::filesystem::symlink_status(paths.assetPath, error);
            if (error || std::filesystem::is_symlink(targetStatus) || !std::filesystem::is_regular_file(targetStatus))
                return Result<ReimportPaths>::Failure(MakeError(AssetErrors::SourceMissing, "Reimport target is missing or unsafe."));

            paths.metadataPath = paths.assetPath;
            paths.metadataPath += ".horo";
            return Result<ReimportPaths>::Success(std::move(paths));
        }

        /** @brief Loads metadata that contains complete reproducible import provenance. */
        [[nodiscard]] Result<AssetImportMetadata> LoadReimportMetadata(const std::filesystem::path &metadataPath) {
            auto metadata = ReadAssetImportMetadata(metadataPath);
            if (metadata.HasError())
                return metadata;
            if (metadata.Value().absoluteSourcePath.empty() || !metadata.Value().absoluteSourcePath.is_absolute() ||
                metadata.Value().importerContributionId.empty()) {
                return Result<AssetImportMetadata>::Failure(
                    MakeError(AssetErrors::SourceMissing, "This asset predates reproducible import provenance."));
            }
            return metadata;
        }

        /** @brief Resolves the exact importer contribution recorded in metadata. */
        [[nodiscard]] Result<const AssetImporterContribution *> ResolveReimportContribution(const AssetImporterCatalogSnapshot &catalog,
                                                                                            const AssetImportMetadata &metadata) {
            const AssetImporterContribution *contribution = catalog.FindById(metadata.importerContributionId);
            if (contribution == nullptr || contribution->strategy == nullptr || !contribution->HandlesExtension(metadata.sourceExtension)) {
                return Result<const AssetImporterContribution *>::Failure(
                    MakeError(ImportErrors::NoImporter, "The exact importer contribution recorded by this asset is unavailable."));
            }
            return Result<const AssetImporterContribution *>::Success(contribution);
        }

        /** @brief Reads the source and executes the resolved importer without publishing partial output. */
        [[nodiscard]] Result<PreparedReimport> PrepareReimport(const AssetImporterContribution &contribution,
                                                               const AssetImportMetadata &metadata, const CancellationToken &cancellation) {
            auto source = ReadAssetImportSource(metadata.absoluteSourcePath);
            if (source.HasError())
                return Result<PreparedReimport>::Failure(source.ErrorValue());
            auto settings = ResolveImportSettings(contribution, metadata.importSettings);
            if (settings.HasError())
                return Result<PreparedReimport>::Failure(settings.ErrorValue());

            PreparedReimport prepared{
                .source = std::move(source).Value(),
            };
            prepared.sourceHash = HashAssetImportSource(prepared.source);
            AssetImportInput input{
                .sourceBytes = prepared.source,
                .sourceExtension = metadata.sourceExtension,
                .settings = std::move(settings).Value(),
            };
            auto imported = contribution.strategy->Import(input, cancellation);
            if (imported.HasError())
                return Result<PreparedReimport>::Failure(imported.ErrorValue());
            if (cancellation.IsCancellationRequested())
                return Result<PreparedReimport>::Failure(MakeError(ImportErrors::ImportCancelled));

            prepared.reasons = ComputeReimportReasons(metadata, contribution, prepared.sourceHash);
            prepared.asset = std::move(imported).Value();
            return Result<PreparedReimport>::Success(std::move(prepared));
        }

        /** @brief Projects successful import facts into the durable metadata snapshot. */
        void UpdateReimportMetadata(AssetImportMetadata &metadata, const AssetImporterContribution &contribution,
                                    const PreparedReimport &prepared) {
            metadata.assetType = prepared.asset.type;
            metadata.importerVersion = contribution.version;
            metadata.importerPackageId = contribution.packageId;
            metadata.importerModuleId = contribution.moduleId;
            metadata.importerModuleVersion = contribution.moduleVersion;
            metadata.sourceHash = prepared.sourceHash;
            metadata.sourceByteSize = prepared.source.size();
            metadata.sourceLastWriteTime = Detail::AssetImportSourceLastWriteTime(metadata.absoluteSourcePath);
            metadata.dependencies = prepared.asset.dependencies;
            metadata.lastImportReasons = prepared.reasons;
            metadata.importedAtUtc = CurrentImportTimestampUtc();
        }

        [[nodiscard]] Result<void> CommitReimportedFiles(DurableFileSystem &files, AssetRegistry &registry,
                                                         const std::filesystem::path &projectRoot, const std::filesystem::path &assetPath,
                                                         const std::filesystem::path &metadataPath, const PreparedAssetImport &prepared,
                                                         const std::string_view serializedMeta) {
            const std::filesystem::path payloadStaging = TemporarySibling(assetPath, "payload.new");
            const std::filesystem::path metadataStaging = TemporarySibling(metadataPath, "metadata.new");
            const std::filesystem::path payloadBackup = TemporarySibling(assetPath, "payload.backup");
            const std::filesystem::path metadataBackup = TemporarySibling(metadataPath, "metadata.backup");
            const auto cleanup = [&] {
                BestEffortRemove(files, payloadStaging);
                BestEffortRemove(files, metadataStaging);
                BestEffortRemove(files, payloadBackup);
                BestEffortRemove(files, metadataBackup);
            };

            if (auto result = files.WriteDurable(payloadStaging, AsBytes(prepared.editorPayload)); result.HasError()) {
                cleanup();
                return result;
            }
            if (auto result = files.WriteDurable(metadataStaging, AsBytes(serializedMeta)); result.HasError()) {
                cleanup();
                return result;
            }
            if (auto result = files.CopyDurable(assetPath, payloadBackup); result.HasError()) {
                cleanup();
                return result;
            }
            if (auto result = files.CopyDurable(metadataPath, metadataBackup); result.HasError()) {
                cleanup();
                return result;
            }

            if (auto result = files.AtomicReplace(payloadStaging, assetPath); result.HasError()) {
                cleanup();
                return result;
            }
            if (auto result = files.AtomicReplace(metadataStaging, metadataPath); result.HasError()) {
                static_cast<void>(RestorePair(files, payloadBackup, assetPath, metadataBackup, metadataPath));
                cleanup();
                return result;
            }

            if (const auto rebuilt = RebuildAssetRegistry(registry, projectRoot, AssetRegistryOpenMode::Edit);
                rebuilt.HasError() || rebuilt.Value().status == AssetRegistryBuildStatus::Failed) {
                static_cast<void>(RestorePair(files, payloadBackup, assetPath, metadataBackup, metadataPath));
                static_cast<void>(RebuildAssetRegistry(registry, projectRoot, AssetRegistryOpenMode::Edit));
                cleanup();
                return Result<void>::Failure(rebuilt.HasError() ? rebuilt.ErrorValue() : MakeError(AssetErrors::IndexMalformed));
            }

            cleanup();
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ReimportProjectAsset */
    Result<AssetReimportReport> ReimportProjectAsset(const AssetReimportRequest &request, const CancellationToken &cancellation) {
        if (request.importerCatalog == nullptr || request.registry == nullptr || request.files == nullptr)
            return Result<AssetReimportReport>::Failure(MakeError(AssetErrors::IndexIo, "Reimport services are unavailable."));

        auto pathsResult = ResolveReimportPaths(request);
        if (pathsResult.HasError())
            return Result<AssetReimportReport>::Failure(pathsResult.ErrorValue());
        ReimportPaths paths = std::move(pathsResult).Value();

        auto metadataResult = LoadReimportMetadata(paths.metadataPath);
        if (metadataResult.HasError())
            return Result<AssetReimportReport>::Failure(metadataResult.ErrorValue());
        AssetImportMetadata metadata = std::move(metadataResult).Value();

        auto contributionResult = ResolveReimportContribution(*request.importerCatalog, metadata);
        if (contributionResult.HasError())
            return Result<AssetReimportReport>::Failure(contributionResult.ErrorValue());
        const AssetImporterContribution &contribution = *contributionResult.Value();

        auto preparedResult = PrepareReimport(contribution, metadata, cancellation);
        if (preparedResult.HasError())
            return Result<AssetReimportReport>::Failure(preparedResult.ErrorValue());
        PreparedReimport prepared = std::move(preparedResult).Value();
        UpdateReimportMetadata(metadata, contribution, prepared);

        auto serialized = SerializeAssetImportMetadata(metadata);
        if (serialized.HasError())
            return Result<AssetReimportReport>::Failure(serialized.ErrorValue());

        if (auto commitResult = CommitReimportedFiles(*request.files, *request.registry, paths.projectRoot, paths.assetPath,
                                                      paths.metadataPath, prepared.asset, serialized.Value());
            commitResult.HasError()) {
            return Result<AssetReimportReport>::Failure(commitResult.ErrorValue());
        }

        LOG_INFO("editor.asset_reimport", "Reimported asset id=%s importer=%s@%s module=%s@%s source=%s",
                 metadata.assetId.ToString().c_str(), contribution.contributionId.c_str(), contribution.version.c_str(),
                 contribution.moduleId.c_str(), contribution.moduleVersion.c_str(), metadata.absoluteSourcePath.string().c_str());
        return Result<AssetReimportReport>::Success(AssetReimportReport{
            .assetId = metadata.assetId,
            .reasons = std::move(prepared.reasons),
            .sourceHash = std::move(prepared.sourceHash),
            .importerVersion = contribution.version,
            .moduleVersion = contribution.moduleVersion,
        });
    }
}  // namespace Horo::Assets
