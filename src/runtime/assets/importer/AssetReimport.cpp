#include "Horo/Assets/AssetReimport.h"

#include "../AssetErrors.h"
#include "AssetImportFileFacts.h"
#include "AssetReimportPublication.h"
#include "Horo/Foundation/Logging/Logger.h"

#include <new>
#include <type_traits>

namespace Horo::Assets {
    namespace {
        struct PreparedReimport final {
            std::size_t sourceBytes{};
            std::string sourceHash;
            std::vector<AssetImportReason> reasons;
            PreparedAssetImport asset;
        };

        std::vector<AssetImportReason> Reasons(const AssetImportMetadata &metadata, const AssetImporterContribution &contribution,
                                               std::string_view hash) {
            using enum AssetImportReason;
            std::vector<AssetImportReason> reasons;
            if (!metadata.sourceHash.empty() && metadata.sourceHash != hash)
                reasons.push_back(SourceChanged);
            if (metadata.importerVersion != contribution.version)
                reasons.push_back(ImporterChanged);
            if (metadata.importerModuleId != contribution.moduleId || metadata.importerModuleVersion != contribution.moduleVersion)
                reasons.push_back(ModuleChanged);
            if (reasons.empty())
                reasons.push_back(ManualReimport);
            return reasons;
        }

        /** @brief Own upstream bytes only during importer invocation; backups already own the originally admitted target bytes. */
        Result<PreparedReimport> Prepare(const Detail::ReimportAdmission &admission, const CancellationToken &cancellation) {
            using Return = Result<PreparedReimport>;
            const auto &metadata = admission.metadata;
            const auto &contribution = *admission.contribution;
            auto source = ReadAssetImportSource(metadata.absoluteSourcePath);
            if (source.HasError())
                return Return::Failure(std::move(source).ErrorValue());
            auto settings = ResolveImportSettings(contribution, metadata.importSettings);
            if (settings.HasError())
                return Return::Failure(std::move(settings).ErrorValue());
            PreparedReimport prepared;
            prepared.sourceBytes = source.Value().size();
            prepared.sourceHash = HashAssetImportSource(source.Value());
            prepared.reasons = Reasons(metadata, contribution, prepared.sourceHash);
            AssetImportInput input{.sourceBytes = source.Value(),
                                   .sourceExtension = metadata.sourceExtension,
                                   .settings = std::move(settings).Value()};
            auto imported = contribution.strategy->Import(input, cancellation);
            if (imported.HasError())
                return Return::Failure(std::move(imported).ErrorValue());
            if (cancellation.IsCancellationRequested())
                return Return::Failure(MakeError(ImportErrors::ImportCancelled));
            prepared.asset = std::move(imported).Value();
            return Return::Success(std::move(prepared));
        }

        void Metadata(AssetImportMetadata &metadata, const AssetImporterContribution &contribution, const PreparedReimport &prepared) {
            metadata.assetType = prepared.asset.type;
            metadata.importerVersion = contribution.version;
            metadata.importerPackageId = contribution.packageId;
            metadata.importerModuleId = contribution.moduleId;
            metadata.importerModuleVersion = contribution.moduleVersion;
            metadata.sourceHash = prepared.sourceHash;
            metadata.sourceByteSize = prepared.sourceBytes;
            metadata.sourceLastWriteTime = Detail::AssetImportSourceLastWriteTime(metadata.absoluteSourcePath);
            metadata.dependencies = prepared.asset.dependencies;
            metadata.lastImportReasons = prepared.reasons;
            metadata.importedAtUtc = CurrentImportTimestampUtc();
        }

        /** @brief Diagnostics cannot turn a committed native transaction into a reported precommit failure. */
        void LogCommitted(const Detail::ReimportAdmission &admission) noexcept {
            try {
                const auto &metadata = admission.metadata;
                const auto &contribution = *admission.contribution;
                LOG_INFO("editor.asset_reimport", "Reimported asset id=%s importer=%s@%s module=%s@%s source=%s",
                         metadata.assetId.ToString().c_str(), contribution.contributionId.c_str(), contribution.version.c_str(),
                         contribution.moduleId.c_str(), contribution.moduleVersion.c_str(), metadata.absoluteSourcePath.string().c_str());
            } catch (...) { /* Logging is not source or registry authority. */
            }
        }

        Result<AssetReimportReport> Execute(Detail::ReimportAdmission &admission, AssetRegistry &registry,
                                            const CancellationToken &cancellation) {
            using Return = Result<AssetReimportReport>;
            auto result = Prepare(admission, cancellation);
            if (result.HasError())
                return Return::Failure(std::move(result).ErrorValue());
            auto prepared = std::move(result).Value();
            Metadata(admission.metadata, *admission.contribution, prepared);
            auto serialized = SerializeAssetImportMetadata(admission.metadata);
            if (serialized.HasError())
                return Return::Failure(std::move(serialized).ErrorValue());
            if (auto validated = DecodeAssetImportMetadata(serialized.Value()); validated.HasError())
                return Return::Failure(std::move(validated).ErrorValue());
            AssetReimportReport report{admission.metadata.assetId, std::move(prepared.reasons), std::move(prepared.sourceHash),
                                       admission.contribution->version, admission.contribution->moduleVersion};
            static_assert(std::is_nothrow_move_constructible_v<AssetReimportReport>);
            auto published = Detail::PublishReimport(admission, std::move(prepared.asset), serialized.Value(), registry, cancellation);
            if (published.HasError())
                return Return::Failure(std::move(published).ErrorValue());
            LogCommitted(admission);
            return Return::Success(std::move(report));
        }
    }  // namespace

    /** @copydoc ReimportProjectAsset */
    Result<AssetReimportReport> ReimportProjectAsset(const AssetReimportRequest &request, const CancellationToken &cancellation) {
        using Return = Result<AssetReimportReport>;
        auto capacity = MakeError(AssetSourceErrors::Limit);
        auto provider = MakeError(AssetSourceErrors::ReadFailed);
        try {
            auto admitted = Detail::AdmitReimport(request, cancellation);
            if (admitted.HasError())
                return Return::Failure(std::move(admitted).ErrorValue());
            auto admission = std::move(admitted).Value();
            return Execute(admission, *request.registry, cancellation);
        } catch (const std::bad_alloc &) {
            return Return::Failure(std::move(capacity));
        } catch (...) {
            return Return::Failure(std::move(provider));
        }
    }
}  // namespace Horo::Assets
