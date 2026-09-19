/**
 * @copydoc AssetImportOperation.h
 */

#include "Horo/Assets/AssetImportOperation.h"

#include "../AssetErrors.h"
#include "Horo/Assets/AssetImportMetadata.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/TransparentString.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Horo::Assets {
    namespace {
        std::string LowerExtension(const std::filesystem::path &path) {
            auto ext = path.extension().string();
            if (!ext.empty() && ext.front() == '.')
                ext.erase(0, 1);
            std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return ext;
        }

        std::filesystem::path NormalizeAbsolute(const std::filesystem::path &path) {
            std::error_code error;
            std::filesystem::path absolute = std::filesystem::absolute(path, error);
            if (error)
                return {};
            absolute = absolute.lexically_normal();
            const std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
            return error ? absolute : canonical;
        }

        std::int64_t SourceLastWriteTime(const std::filesystem::path &path) {
            std::error_code error;
            const auto value = std::filesystem::last_write_time(path, error);
            return error ? 0 : static_cast<std::int64_t>(value.time_since_epoch().count());
        }

        std::string SerializeDefaultSetting(const ImportSettingDescriptor &descriptor) {
            using enum ImportSettingKind;
            switch (descriptor.kind) {
                case Boolean:
                    return std::holds_alternative<bool>(descriptor.defaultValue) && std::get<bool>(descriptor.defaultValue) ? "true"
                                                                                                                            : "false";
                case Integer:
                    return std::holds_alternative<std::int64_t>(descriptor.defaultValue)
                               ? std::to_string(std::get<std::int64_t>(descriptor.defaultValue))
                               : "0";
                case Float:
                    return std::holds_alternative<double>(descriptor.defaultValue)
                               ? std::to_string(std::get<double>(descriptor.defaultValue))
                               : "0.000000";
                case Text:
                    return std::holds_alternative<std::string>(descriptor.defaultValue) ? std::get<std::string>(descriptor.defaultValue)
                                                                                        : std::string{};
                case Choice:
                    if (std::holds_alternative<std::size_t>(descriptor.defaultValue))
                        return std::to_string(std::get<std::size_t>(descriptor.defaultValue));
                    for (std::size_t index = 0; index < descriptor.choices.size(); ++index) {
                        if (descriptor.choices[index].value == descriptor.defaultValue)
                            return std::to_string(index);
                    }
                    return "0";
            }
            return {};
        }

        void MaterializeDefaultSettings(AssetImportItem &item, const AssetImporterContribution *contribution) {
            if (contribution == nullptr)
                return;
            for (const auto &descriptor : contribution->settings) {
                item.settings.try_emplace("settings." + descriptor.id, SerializeDefaultSetting(descriptor));
            }
        }

    }  // namespace

    AssetImportOperation::AssetImportOperation(JobSystem &jobs, std::shared_ptr<const AssetImporterCatalogSnapshot> catalog)
        : jobs_(jobs), catalog_(std::move(catalog)) {}

    Result<AssetImportSnapshot> AssetImportOperation::Start(const AssetImportRequest &request, const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Result<AssetImportSnapshot>::Failure(Error{CookErrors::Cancelled.code});

        if (!catalog_)
            return Result<AssetImportSnapshot>::Failure(Error{CookErrors::MalformedArtifact.code});

        const std::scoped_lock lock{mutex_};
        cancelled_ = false;
        snapshot_ = AssetImportSnapshot{
            .operationId = std::format("import-{}", ++revision_),
            .revision = revision_,
            .phase = AssetImportPhase::Selecting,
            .canCancel = true,
        };

        for (const auto &sourceFile : request.sourceFiles) {
            const std::filesystem::path absoluteSource = NormalizeAbsolute(sourceFile);
            auto ext = LowerExtension(absoluteSource);
            const auto *strategy = catalog_->FindByExtension(ext);

            // Find contribution ID from the strategy — workaround until
            // FindContributionByExtension is added to the snapshot.
            std::string contribId;
            const auto *contrib = catalog_->FindContributionByExtension(ext);
            std::string targetExt = ".horoasset";
            bool supportsMeta = true;
            if (contrib) {
                contribId = contrib->contributionId;
                targetExt = contrib->targetExtension;
                supportsMeta = contrib->supportsMetaSidecar;
            }

            auto parsed = ProjectPath::Parse(std::filesystem::relative(absoluteSource, request.projectRoot).string());
            if (!parsed.HasValue()) {
                // File is outside the project root — use just the filename
                // as the project-relative path.
                parsed = ProjectPath::Parse(absoluteSource.filename().string());
                if (!parsed.HasValue())
                    continue;
            }
            AssetImportItem item{
                .sourceFile = std::move(parsed).Value(),
                .absoluteSourcePath = absoluteSource,
                .importerContributionId = contribId,
                .importerVersion = contrib != nullptr ? contrib->version : std::string{},
                .importerPackageId = contrib != nullptr ? contrib->packageId : std::string{},
                .importerModuleId = contrib != nullptr ? contrib->moduleId : std::string{},
                .importerModuleVersion = contrib != nullptr ? contrib->moduleVersion : std::string{},
                .sourceExtension = ext,
                .displayName = absoluteSource.stem().string(),
                .destinationFolder = request.destinationFolder,
                .targetExtension = targetExt,
                .supportsMetaSidecar = supportsMeta,
                .importReasons = {AssetImportReason::InitialImport},
            };
            MaterializeDefaultSettings(item, contrib);

            if (!strategy) {
                item.diagnostics.push_back(ImportDiagnostic{
                    .severity = ImportDiagnostic::Severity::Error,
                    .code = ImportErrors::NoImporter.code.Value(),
                    .message = "No importer registered for extension ." + ext,
                });
                LOG_ERROR("editor.asset_import", "No importer for .%s — %s", ext.c_str(), absoluteSource.filename().string().c_str());
            }

            snapshot_.items.push_back(std::move(item));
        }

        return Result<AssetImportSnapshot>::Success(snapshot_);
    }

    Result<AssetImportSnapshot> AssetImportOperation::AddFiles(const std::vector<std::filesystem::path> &sourceFiles,
                                                               const std::filesystem::path &projectRoot,
                                                               const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Result<AssetImportSnapshot>::Failure(Error{CookErrors::Cancelled.code});

        if (!catalog_)
            return Result<AssetImportSnapshot>::Failure(Error{CookErrors::MalformedArtifact.code});

        const std::scoped_lock lock{mutex_};
        for (const auto &sourceFile : sourceFiles) {
            const std::filesystem::path absoluteSource = NormalizeAbsolute(sourceFile);
            auto ext = LowerExtension(absoluteSource);
            const auto *strategy = catalog_->FindByExtension(ext);

            std::string contribId;
            const auto *contrib = catalog_->FindContributionByExtension(ext);
            std::string targetExt = ".horoasset";
            bool supportsMeta = true;
            if (contrib) {
                contribId = contrib->contributionId;
                targetExt = contrib->targetExtension;
                supportsMeta = contrib->supportsMetaSidecar;
            }

            auto parsed = ProjectPath::Parse(std::filesystem::relative(absoluteSource, projectRoot).string());
            if (!parsed.HasValue()) {
                // File is outside the project root — use just the filename
                // as the project-relative path. The importer reads from the
                // original absolute path; sourceFile is for display only.
                parsed = ProjectPath::Parse(absoluteSource.filename().string());
                if (!parsed.HasValue())
                    continue;  // Can't even parse the filename — skip
            }

            AssetImportItem item{
                .sourceFile = std::move(parsed).Value(),
                .absoluteSourcePath = absoluteSource,
                .importerContributionId = contribId,
                .importerVersion = contrib != nullptr ? contrib->version : std::string{},
                .importerPackageId = contrib != nullptr ? contrib->packageId : std::string{},
                .importerModuleId = contrib != nullptr ? contrib->moduleId : std::string{},
                .importerModuleVersion = contrib != nullptr ? contrib->moduleVersion : std::string{},
                .sourceExtension = ext,
                .displayName = absoluteSource.stem().string(),
                .destinationFolder = "assets",
                .targetExtension = targetExt,
                .supportsMetaSidecar = supportsMeta,
                .importReasons = {AssetImportReason::InitialImport},
            };
            MaterializeDefaultSettings(item, contrib);

            if (!strategy) {
                item.diagnostics.push_back(ImportDiagnostic{
                    .severity = ImportDiagnostic::Severity::Error,
                    .code = ImportErrors::NoImporter.code.Value(),
                    .message = "No importer registered for extension ." + ext,
                });
                LOG_ERROR("editor.asset_import", "No importer for .%s — %s", ext.c_str(), absoluteSource.filename().string().c_str());
            }

            snapshot_.items.push_back(std::move(item));
        }

        snapshot_.revision = ++revision_;
        return Result<AssetImportSnapshot>::Success(snapshot_);
    }

    Result<AssetImportSnapshot> AssetImportOperation::ImportSingleItem(std::size_t index, const CancellationToken &cancellation) {
        const AssetImporterContribution *contribution{};
        std::filesystem::path absoluteSourcePath;
        std::string sourceExtension;
        std::string displayName;
        std::string operationId;
        TransparentStringMap<std::string> settings;
        {
            const std::scoped_lock lock{mutex_};
            if (cancelled_ || cancellation.IsCancellationRequested()) {
                snapshot_.phase = AssetImportPhase::Cancelled;
                snapshot_.canCancel = false;
                snapshot_.canCommit = false;
                snapshot_.revision = ++revision_;
                return Result<AssetImportSnapshot>::Failure(Error{CookErrors::Cancelled.code});
            }
            if (index >= snapshot_.items.size())
                return Result<AssetImportSnapshot>::Failure(Error{CookErrors::MalformedArtifact.code});

            auto &item = snapshot_.items[index];
            if (item.result.has_value())
                return Result<AssetImportSnapshot>::Success(snapshot_);

            contribution = catalog_->FindById(item.importerContributionId);
            if (contribution == nullptr || contribution->strategy == nullptr || !contribution->HandlesExtension(item.sourceExtension)) {
                item.diagnostics.push_back(ImportDiagnostic{
                    .severity = ImportDiagnostic::Severity::Error,
                    .code = ImportErrors::NoImporter.code.Value(),
                    .message = "No importer for ." + item.sourceExtension,
                });
                LOG_ERROR("editor.asset_import", "No importer for .%s — %s", item.sourceExtension.c_str(), item.displayName.c_str());
                snapshot_.phase = AssetImportPhase::Failed;
                snapshot_.canCancel = false;
                snapshot_.revision = ++revision_;
                return Result<AssetImportSnapshot>::Success(snapshot_);
            }

            absoluteSourcePath = item.absoluteSourcePath;
            sourceExtension = item.sourceExtension;
            displayName = item.displayName;
            operationId = snapshot_.operationId;
            settings = item.settings;
            snapshot_.phase = AssetImportPhase::Preparing;
            snapshot_.canCommit = false;
            snapshot_.revision = ++revision_;
        }

        auto source = ReadAssetImportSource(absoluteSourcePath);
        if (source.HasError()) {
            const std::scoped_lock lock{mutex_};
            if (snapshot_.operationId != operationId || index >= snapshot_.items.size())
                return Result<AssetImportSnapshot>::Failure(Error{CookErrors::MalformedArtifact.code});
            auto &item = snapshot_.items[index];
            item.diagnostics.push_back(ImportDiagnostic{
                .severity = ImportDiagnostic::Severity::Error,
                .code = source.ErrorValue().code.Value(),
                .message = source.ErrorValue().message,
            });
            snapshot_.phase = AssetImportPhase::Failed;
            snapshot_.canCancel = false;
            snapshot_.revision = ++revision_;
            return Result<AssetImportSnapshot>::Success(snapshot_);
        }
        std::vector<std::uint8_t> fileBytes = std::move(source).Value();
        {
            const std::scoped_lock lock{mutex_};
            if (snapshot_.operationId != operationId || index >= snapshot_.items.size())
                return Result<AssetImportSnapshot>::Failure(Error{CookErrors::MalformedArtifact.code});
            if (cancelled_ || cancellation.IsCancellationRequested()) {
                snapshot_.phase = AssetImportPhase::Cancelled;
                snapshot_.canCancel = false;
                snapshot_.canCommit = false;
                snapshot_.revision = ++revision_;
                return Result<AssetImportSnapshot>::Failure(Error{CookErrors::Cancelled.code});
            }
            auto &item = snapshot_.items[index];
            item.sourceHash = HashAssetImportSource(fileBytes);
            item.sourceByteSize = fileBytes.size();
            item.sourceLastWriteTime = SourceLastWriteTime(absoluteSourcePath);
            snapshot_.revision = ++revision_;
        }

        auto resolved = ResolveImportSettings(*contribution, settings);
        if (resolved.HasError()) {
            const std::scoped_lock lock{mutex_};
            if (snapshot_.operationId != operationId || index >= snapshot_.items.size())
                return Result<AssetImportSnapshot>::Failure(Error{CookErrors::MalformedArtifact.code});
            snapshot_.phase = AssetImportPhase::Failed;
            snapshot_.canCancel = false;
            snapshot_.revision = ++revision_;
            return Result<AssetImportSnapshot>::Failure(resolved.ErrorValue());
        }

        struct ProgressContext final {
            AssetImportOperation *operation;
            std::string_view operationId;
            std::size_t itemIndex;
        } progressContext{.operation = this, .operationId = operationId, .itemIndex = index};

        AssetImportInput input{
            .sourceBytes = fileBytes,
            .sourceExtension = sourceExtension,
            .settings = std::move(resolved).Value(),
        };
        input.progress = {
            .context = &progressContext,
            .report =
                [](void *context, const std::uint64_t completedUnits, const std::uint64_t totalUnits, const std::string_view message) {
            auto &progress = *static_cast<ProgressContext *>(context);
            progress.operation->ReportProgress(progress.operationId, progress.itemIndex, completedUnits, totalUnits, message);
        },
        };

        auto result = contribution->strategy->Import(input, cancellation);
        const std::scoped_lock lock{mutex_};
        if (snapshot_.operationId != operationId || index >= snapshot_.items.size())
            return Result<AssetImportSnapshot>::Failure(Error{CookErrors::MalformedArtifact.code});
        if (cancelled_ || cancellation.IsCancellationRequested()) {
            snapshot_.phase = AssetImportPhase::Cancelled;
            snapshot_.canCancel = false;
            snapshot_.canCommit = false;
            snapshot_.revision = ++revision_;
            return Result<AssetImportSnapshot>::Failure(Error{CookErrors::Cancelled.code});
        }

        auto &item = snapshot_.items[index];
        if (result.HasValue()) {
            PreparedAssetImport prepared = std::move(result).Value();
            item.resolvedType = prepared.type;
            item.diagnostics.insert(item.diagnostics.end(), prepared.diagnostics.begin(), prepared.diagnostics.end());
            item.result = std::move(prepared);
            const bool allImported = std::ranges::all_of(snapshot_.items, [](const AssetImportItem &candidate) {
                return candidate.result.has_value();
            });
            snapshot_.phase = allImported ? AssetImportPhase::ReadyToCommit : AssetImportPhase::Preparing;
            snapshot_.canCommit = allImported;
            snapshot_.canCancel = !allImported;
        } else {
            const auto &err = result.ErrorValue();
            item.diagnostics.push_back(ImportDiagnostic{
                .severity = ImportDiagnostic::Severity::Error,
                .code = err.code.Value(),
                .message = err.message,
            });
            LOG_ERROR("editor.asset_import", "Import failed for %s: %s", displayName.c_str(), err.message.c_str());
            snapshot_.phase = AssetImportPhase::Failed;
            snapshot_.canCancel = false;
        }

        snapshot_.revision = ++revision_;
        return Result<AssetImportSnapshot>::Success(snapshot_);
    }

    Result<AssetImportSnapshot> AssetImportOperation::SetItemSettings(const std::size_t index, TransparentStringMap<std::string> settings) {
        const std::scoped_lock lock{mutex_};
        if (index >= snapshot_.items.size())
            return Result<AssetImportSnapshot>::Failure(Error{CookErrors::MalformedArtifact.code});

        snapshot_.items[index].settings = std::move(settings);
        snapshot_.revision = ++revision_;
        return Result<AssetImportSnapshot>::Success(snapshot_);
    }

    AssetImportSnapshot AssetImportOperation::Snapshot() const {
        const std::scoped_lock lock{mutex_};
        return snapshot_;
    }

    void AssetImportOperation::Cancel() {
        const std::scoped_lock lock{mutex_};
        cancelled_ = true;
        snapshot_.phase = AssetImportPhase::Cancelled;
        snapshot_.canCancel = false;
        snapshot_.canCommit = false;
        snapshot_.revision = ++revision_;
    }

    /** @copydoc AssetImportOperation::ReportProgress */
    void AssetImportOperation::ReportProgress(const std::string_view operationId, const std::size_t index,
                                              const std::uint64_t completedUnits, const std::uint64_t totalUnits,
                                              const std::string_view message) {
        if (totalUnits == 0 || completedUnits > totalUnits || message.size() > 4096)
            return;

        const std::scoped_lock lock{mutex_};
        if (cancelled_ || snapshot_.operationId != operationId || index >= snapshot_.items.size())
            return;
        auto &item = snapshot_.items[index];
        item.progressCompletedUnits = completedUnits;
        item.progressTotalUnits = totalUnits;
        item.progressMessage.assign(message);
        snapshot_.revision = ++revision_;
    }

}  // namespace Horo::Assets
