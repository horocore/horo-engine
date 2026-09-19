/**
 * @copydoc AssetImportOperation.h
 */

#include "Horo/Assets/AssetImportOperation.h"

#include "../AssetErrors.h"
#include "AssetImportFileFacts.h"
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

        std::string SerializeChoiceDefault(const ImportSettingDescriptor &descriptor) {
            if (std::holds_alternative<std::size_t>(descriptor.defaultValue))
                return std::to_string(std::get<std::size_t>(descriptor.defaultValue));
            for (std::size_t index = 0; index < descriptor.choices.size(); ++index) {
                if (descriptor.choices[index].value == descriptor.defaultValue)
                    return std::to_string(index);
            }
            return "0";
        }

        std::string SerializeBooleanDefault(const ImportSettingValue &value) {
            const auto *typed = std::get_if<bool>(&value);
            return typed != nullptr && *typed ? "true" : "false";
        }

        std::string SerializeIntegerDefault(const ImportSettingValue &value) {
            const auto *typed = std::get_if<std::int64_t>(&value);
            return typed != nullptr ? std::to_string(*typed) : "0";
        }

        std::string SerializeFloatDefault(const ImportSettingValue &value) {
            const auto *typed = std::get_if<double>(&value);
            return typed != nullptr ? std::to_string(*typed) : "0.000000";
        }

        std::string SerializeTextDefault(const ImportSettingValue &value) {
            const auto *typed = std::get_if<std::string>(&value);
            return typed != nullptr ? *typed : std::string{};
        }

        std::string SerializeDefaultSetting(const ImportSettingDescriptor &descriptor) {
            using enum ImportSettingKind;
            switch (descriptor.kind) {
                case Boolean:
                    return SerializeBooleanDefault(descriptor.defaultValue);
                case Integer:
                    return SerializeIntegerDefault(descriptor.defaultValue);
                case Float:
                    return SerializeFloatDefault(descriptor.defaultValue);
                case Text:
                    return SerializeTextDefault(descriptor.defaultValue);
                case Choice:
                    return SerializeChoiceDefault(descriptor);
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

        [[nodiscard]] std::optional<ProjectPath> ResolveSourcePath(const std::filesystem::path &absoluteSource,
                                                                   const std::filesystem::path &projectRoot) {
            auto parsed = ProjectPath::Parse(std::filesystem::relative(absoluteSource, projectRoot).string());
            if (parsed.HasValue())
                return std::move(parsed).Value();
            parsed = ProjectPath::Parse(absoluteSource.filename().string());
            if (parsed.HasValue())
                return std::move(parsed).Value();
            return std::nullopt;
        }

        void AppendMissingImporterDiagnostic(AssetImportItem &item) {
            item.diagnostics.push_back(ImportDiagnostic{
                .severity = ImportDiagnostic::Severity::Error,
                .code = ImportErrors::NoImporter.code.Value(),
                .message = "No importer registered for extension ." + item.sourceExtension,
            });
            LOG_ERROR("editor.asset_import", "No importer for .%s — %s", item.sourceExtension.c_str(), item.displayName.c_str());
        }

        [[nodiscard]] std::optional<AssetImportItem> BuildImportItem(const std::filesystem::path &sourceFile,
                                                                     const std::filesystem::path &projectRoot,
                                                                     const std::string_view destinationFolder,
                                                                     const AssetImporterCatalogSnapshot &catalog) {
            const std::filesystem::path absoluteSource = Detail::NormalizeAssetImportPath(sourceFile);
            const std::string extension = LowerExtension(absoluteSource);
            auto sourcePath = ResolveSourcePath(absoluteSource, projectRoot);
            if (!sourcePath.has_value())
                return std::nullopt;

            const auto *contribution = catalog.FindContributionByExtension(extension);
            AssetImportItem item{
                .sourceFile = std::move(*sourcePath),
                .absoluteSourcePath = absoluteSource,
                .importerContributionId = contribution != nullptr ? contribution->contributionId : std::string{},
                .importerVersion = contribution != nullptr ? contribution->version : std::string{},
                .importerPackageId = contribution != nullptr ? contribution->packageId : std::string{},
                .importerModuleId = contribution != nullptr ? contribution->moduleId : std::string{},
                .importerModuleVersion = contribution != nullptr ? contribution->moduleVersion : std::string{},
                .sourceExtension = extension,
                .displayName = absoluteSource.stem().string(),
                .destinationFolder = std::string{destinationFolder},
                .targetExtension = contribution != nullptr ? contribution->targetExtension : ".horoasset",
                .supportsMetaSidecar = contribution == nullptr || contribution->supportsMetaSidecar,
                .importReasons = {AssetImportReason::InitialImport},
            };
            MaterializeDefaultSettings(item, contribution);
            if (catalog.FindByExtension(extension) == nullptr)
                AppendMissingImporterDiagnostic(item);
            return item;
        }

        void MarkCancelled(AssetImportSnapshot &snapshot, std::uint64_t &revision) {
            snapshot.phase = AssetImportPhase::Cancelled;
            snapshot.canCancel = false;
            snapshot.canCommit = false;
            snapshot.revision = ++revision;
        }

        void MarkFailed(AssetImportSnapshot &snapshot, std::uint64_t &revision) {
            snapshot.phase = AssetImportPhase::Failed;
            snapshot.canCancel = false;
            snapshot.canCommit = false;
            snapshot.revision = ++revision;
        }

        [[nodiscard]] bool IsCurrentImport(const AssetImportSnapshot &snapshot, const std::string_view operationId,
                                           const std::size_t index) {
            return snapshot.operationId == operationId && index < snapshot.items.size();
        }

        struct PendingImport final {
            const AssetImporterContribution *contribution{};
            std::filesystem::path absoluteSourcePath;
            std::string sourceExtension;
            std::string displayName;
            std::string operationId;
            TransparentStringMap<std::string> settings;
        };

        [[nodiscard]] Result<void> ValidateImportAdmission(const std::shared_ptr<const AssetImporterCatalogSnapshot> &catalog,
                                                           const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(Error{CookErrors::Cancelled.code});
            if (catalog == nullptr)
                return Result<void>::Failure(Error{CookErrors::MalformedArtifact.code});
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::optional<PendingImport>> PrepareImportLocked(AssetImportSnapshot &snapshot, std::uint64_t &revision,
                                                                               const bool cancelled, const std::size_t index,
                                                                               const CancellationToken &cancellation,
                                                                               const AssetImporterCatalogSnapshot &catalog) {
            if (cancelled || cancellation.IsCancellationRequested()) {
                MarkCancelled(snapshot, revision);
                return Result<std::optional<PendingImport>>::Failure(Error{CookErrors::Cancelled.code});
            }
            if (index >= snapshot.items.size())
                return Result<std::optional<PendingImport>>::Failure(Error{CookErrors::MalformedArtifact.code});

            auto &item = snapshot.items[index];
            if (item.result.has_value())
                return Result<std::optional<PendingImport>>::Success(std::nullopt);
            const auto *contribution = catalog.FindById(item.importerContributionId);
            if (contribution == nullptr || contribution->strategy == nullptr || !contribution->HandlesExtension(item.sourceExtension)) {
                AppendMissingImporterDiagnostic(item);
                MarkFailed(snapshot, revision);
                return Result<std::optional<PendingImport>>::Success(std::nullopt);
            }

            PendingImport pending{
                .contribution = contribution,
                .absoluteSourcePath = item.absoluteSourcePath,
                .sourceExtension = item.sourceExtension,
                .displayName = item.displayName,
                .operationId = snapshot.operationId,
                .settings = item.settings,
            };
            snapshot.phase = AssetImportPhase::Preparing;
            snapshot.canCommit = false;
            snapshot.revision = ++revision;
            return Result<std::optional<PendingImport>>::Success(std::move(pending));
        }

        enum class ImportFailureDisposition : std::uint8_t {
            SourceRead,
            Settings
        };

        [[nodiscard]] Result<AssetImportSnapshot> RecordImportFailureLocked(AssetImportSnapshot &snapshot, std::uint64_t &revision,
                                                                            const PendingImport &pending, const std::size_t index,
                                                                            const Error &error,
                                                                            const ImportFailureDisposition disposition) {
            if (!IsCurrentImport(snapshot, pending.operationId, index))
                return Result<AssetImportSnapshot>::Failure(Error{CookErrors::MalformedArtifact.code});
            if (disposition == ImportFailureDisposition::SourceRead) {
                snapshot.items[index].diagnostics.push_back(ImportDiagnostic{
                    .severity = ImportDiagnostic::Severity::Error,
                    .code = error.code.Value(),
                    .message = error.message,
                });
            }
            MarkFailed(snapshot, revision);
            if (disposition == ImportFailureDisposition::Settings)
                return Result<AssetImportSnapshot>::Failure(error);
            return Result<AssetImportSnapshot>::Success(snapshot);
        }

        [[nodiscard]] Result<void> StageSourceLocked(AssetImportSnapshot &snapshot, std::uint64_t &revision, const bool cancelled,
                                                     const PendingImport &pending, const std::size_t index,
                                                     const std::span<const std::uint8_t> fileBytes, const CancellationToken &cancellation) {
            if (!IsCurrentImport(snapshot, pending.operationId, index))
                return Result<void>::Failure(Error{CookErrors::MalformedArtifact.code});
            if (cancelled || cancellation.IsCancellationRequested()) {
                MarkCancelled(snapshot, revision);
                return Result<void>::Failure(Error{CookErrors::Cancelled.code});
            }
            auto &item = snapshot.items[index];
            item.sourceHash = HashAssetImportSource(fileBytes);
            item.sourceByteSize = fileBytes.size();
            item.sourceLastWriteTime = Detail::AssetImportSourceLastWriteTime(pending.absoluteSourcePath);
            snapshot.revision = ++revision;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<AssetImportSnapshot> FinalizeImportLocked(AssetImportSnapshot &snapshot, std::uint64_t &revision,
                                                                       const bool cancelled, const PendingImport &pending,
                                                                       const std::size_t index, Result<PreparedAssetImport> result,
                                                                       const CancellationToken &cancellation) {
            if (!IsCurrentImport(snapshot, pending.operationId, index))
                return Result<AssetImportSnapshot>::Failure(Error{CookErrors::MalformedArtifact.code});
            if (cancelled || cancellation.IsCancellationRequested()) {
                MarkCancelled(snapshot, revision);
                return Result<AssetImportSnapshot>::Failure(Error{CookErrors::Cancelled.code});
            }

            auto &item = snapshot.items[index];
            if (result.HasError()) {
                const auto &error = result.ErrorValue();
                item.diagnostics.push_back(
                    {.severity = ImportDiagnostic::Severity::Error, .code = error.code.Value(), .message = error.message});
                LOG_ERROR("editor.asset_import", "Import failed for %s: %s", pending.displayName.c_str(), error.message.c_str());
                MarkFailed(snapshot, revision);
                return Result<AssetImportSnapshot>::Success(snapshot);
            }

            PreparedAssetImport prepared = std::move(result).Value();
            item.resolvedType = prepared.type;
            item.diagnostics.insert(item.diagnostics.end(), prepared.diagnostics.begin(), prepared.diagnostics.end());
            item.result = std::move(prepared);
            const bool allImported = std::ranges::all_of(snapshot.items, [](const AssetImportItem &candidate) {
                return candidate.result.has_value();
            });
            snapshot.phase = allImported ? AssetImportPhase::ReadyToCommit : AssetImportPhase::Preparing;
            snapshot.canCommit = allImported;
            snapshot.canCancel = !allImported;
            snapshot.revision = ++revision;
            return Result<AssetImportSnapshot>::Success(snapshot);
        }

    }  // namespace

    AssetImportOperation::AssetImportOperation(JobSystem &jobs, std::shared_ptr<const AssetImporterCatalogSnapshot> catalog)
        : jobs_(jobs), catalog_(std::move(catalog)) {}

    Result<AssetImportSnapshot> AssetImportOperation::Start(const AssetImportRequest &request, const CancellationToken &cancellation) {
        if (auto admission = ValidateImportAdmission(catalog_, cancellation); admission.HasError())
            return Result<AssetImportSnapshot>::Failure(admission.ErrorValue());

        const std::scoped_lock lock{mutex_};
        cancelled_ = false;
        snapshot_ = AssetImportSnapshot{
            .operationId = std::format("import-{}", ++revision_),
            .revision = revision_,
            .phase = AssetImportPhase::Selecting,
            .canCancel = true,
        };

        for (const auto &sourceFile : request.sourceFiles) {
            auto item = BuildImportItem(sourceFile, request.projectRoot, request.destinationFolder, *catalog_);
            if (item.has_value())
                snapshot_.items.push_back(std::move(*item));
        }

        return Result<AssetImportSnapshot>::Success(snapshot_);
    }

    Result<AssetImportSnapshot> AssetImportOperation::AddFiles(const std::vector<std::filesystem::path> &sourceFiles,
                                                               const std::filesystem::path &projectRoot,
                                                               const CancellationToken &cancellation) {
        if (auto admission = ValidateImportAdmission(catalog_, cancellation); admission.HasError())
            return Result<AssetImportSnapshot>::Failure(admission.ErrorValue());

        const std::scoped_lock lock{mutex_};
        for (const auto &sourceFile : sourceFiles) {
            auto item = BuildImportItem(sourceFile, projectRoot, "assets", *catalog_);
            if (item.has_value())
                snapshot_.items.push_back(std::move(*item));
        }

        snapshot_.revision = ++revision_;
        return Result<AssetImportSnapshot>::Success(snapshot_);
    }

    Result<AssetImportSnapshot> AssetImportOperation::ImportSingleItem(std::size_t index, const CancellationToken &cancellation) {
        PendingImport pending;
        {
            const std::scoped_lock lock{mutex_};
            auto prepared = PrepareImportLocked(snapshot_, revision_, cancelled_, index, cancellation, *catalog_);
            if (prepared.HasError())
                return Result<AssetImportSnapshot>::Failure(prepared.ErrorValue());
            if (!prepared.Value().has_value())
                return Result<AssetImportSnapshot>::Success(snapshot_);
            pending = std::move(*prepared.Value());
        }

        auto source = ReadAssetImportSource(pending.absoluteSourcePath);
        if (source.HasError()) {
            const std::scoped_lock lock{mutex_};
            return RecordImportFailureLocked(snapshot_, revision_, pending, index, source.ErrorValue(),
                                             ImportFailureDisposition::SourceRead);
        }
        std::vector<std::uint8_t> fileBytes = std::move(source).Value();
        {
            const std::scoped_lock lock{mutex_};
            auto staged = StageSourceLocked(snapshot_, revision_, cancelled_, pending, index, fileBytes, cancellation);
            if (staged.HasError())
                return Result<AssetImportSnapshot>::Failure(staged.ErrorValue());
        }

        auto resolved = ResolveImportSettings(*pending.contribution, pending.settings);
        if (resolved.HasError()) {
            const std::scoped_lock lock{mutex_};
            return RecordImportFailureLocked(snapshot_, revision_, pending, index, resolved.ErrorValue(),
                                             ImportFailureDisposition::Settings);
        }

        ProgressContext progressContext{.operation = this, .operationId = pending.operationId, .itemIndex = index};

        AssetImportInput input{
            .sourceBytes = fileBytes,
            .sourceExtension = pending.sourceExtension,
            .settings = std::move(resolved).Value(),
        };
        input.progress = {.context = &progressContext, .report = ProjectProgress};

        auto result = pending.contribution->strategy->Import(input, cancellation);
        const std::scoped_lock lock{mutex_};
        return FinalizeImportLocked(snapshot_, revision_, cancelled_, pending, index, std::move(result), cancellation);
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

    /** @copydoc AssetImportOperation::ProjectProgress */
    void AssetImportOperation::ProjectProgress(void *context, const std::uint64_t completedUnits,  // NOSONAR(cpp:S5008) ABI sink.
                                               const std::uint64_t totalUnits, const std::string_view message) {
        auto &progress = *static_cast<ProgressContext *>(context);
        progress.operation->ReportProgress(progress.operationId, progress.itemIndex, completedUnits, totalUnits, message);
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
