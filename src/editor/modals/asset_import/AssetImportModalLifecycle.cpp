/**
 * @copydoc AssetImportModal.h
 */

#include "Horo/Assets/AssetImporter.h"
#include "Horo/Editor/AssetImportModal.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/ErrorCode.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Sha256.h"
#include "runtime/assets/importer/ProjectAssetImportCommitter.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <random>
#include <string>

namespace Horo::Editor {
    namespace {
        constexpr std::size_t kMaximumImportHistory = 50;

        [[nodiscard]] std::string PresetScopeKey(const Assets::AssetImportItem &item) {
            return item.importerContributionId + '\n' + item.sourceExtension;
        }

        [[nodiscard]] bool IsValidAssetName(const std::string_view name) {
            if (name.empty() || name == "." || name == "..")
                return false;
            const std::filesystem::path path{name};
            return path.filename() == path && !path.has_parent_path();
        }

        [[nodiscard]] Assets::AssetId MakeAssetId(const Assets::AssetImportItem &item) {
            std::array<std::byte, 16> bytes{};
            if (item.assetIdStrategy == 1) {
                const std::string sourcePath = item.absoluteSourcePath.generic_string();
                const Sha256Digest digest =
                    ComputeSha256(std::span{reinterpret_cast<const std::byte *>(sourcePath.data()), sourcePath.size()});
                for (std::size_t i = 0; i < bytes.size(); ++i)
                    bytes[i] = static_cast<std::byte>(digest.bytes[i]);
            } else {
                std::random_device random;
                for (std::byte &b : bytes) {
                    b = static_cast<std::byte>(random());
                }
            }
            bytes[6] = (bytes[6] & std::byte{0x0f}) | std::byte{0x40};
            bytes[8] = (bytes[8] & std::byte{0x3f}) | std::byte{0x80};
            std::array<std::uint8_t, 16> uintBytes{};
            for (std::size_t i = 0; i < bytes.size(); ++i)
                uintBytes[i] = static_cast<std::uint8_t>(bytes[i]);
            return Assets::AssetId::FromBytes(uintBytes);
        }

        [[nodiscard]] bool EqualsIgnoringAsciiCase(const std::string_view left, const std::string_view right) {
            return left.size() == right.size() && std::ranges::equal(left, right, [](const unsigned char lhs, const unsigned char rhs) {
                return std::tolower(lhs) == std::tolower(rhs);
            });
        }

        [[nodiscard]] std::string ResolveDestinationFolder(const Assets::AssetImportItem &item,
                                                           const Assets::AssetImporterCatalogSnapshot *catalog) {
            std::filesystem::path destination =
                item.destinationFolder.empty() ? std::filesystem::path{"assets"} : std::filesystem::path{item.destinationFolder};
            destination = destination.lexically_normal();
            if (item.subfolderByType != 0 || catalog == nullptr)
                return destination.generic_string();

            const auto *contribution = catalog->FindById(item.importerContributionId);
            if (contribution == nullptr || contribution->subfolderCategory.empty())
                return destination.generic_string();

            if (const std::string selectedFolderName = destination.filename().string();
                !EqualsIgnoringAsciiCase(selectedFolderName, contribution->subfolderCategory)) {
                destination /= contribution->subfolderCategory;
            }
            return destination.generic_string();
        }

        [[nodiscard]] Result<void> ValidateImportItem(const Assets::AssetImportSnapshot &snapshot, const std::size_t index,
                                                      const bool hasOperation) {
            if (!hasOperation)
                return Result<void>::Failure(Error{ErrorCode{"editor.asset_import.no_operation"}, ErrorDomainId{"horo.editor"}});
            if (index >= snapshot.items.size())
                return Result<void>::Failure(Error{ErrorCode{"editor.asset_import.invalid_item"}, ErrorDomainId{"horo.editor"}});
            const auto &pendingItem = snapshot.items[index];
            if (!IsValidAssetName(pendingItem.displayName))
                return Result<void>::Failure(Error{ErrorCode{"editor.asset_import.invalid_asset_name"}, ErrorDomainId{"horo.editor"}});
            if (const std::string pendingDestination = pendingItem.destinationFolder.empty() ? "assets" : pendingItem.destinationFolder;
                !ProjectPath::Parse(pendingDestination).HasValue()) {
                return Result<void>::Failure(Error{ErrorCode{"editor.asset_import.invalid_destination"}, ErrorDomainId{"horo.editor"}});
            }
            return Result<void>::Success();
        }

        class FixedAssetIdGenerator final : public Assets::IAssetIdGenerator {
        public:
            explicit FixedAssetIdGenerator(const Assets::AssetId id) noexcept : id_(id) {}

            [[nodiscard]] Assets::AssetId Generate() override {
                return id_;
            }

        private:
            Assets::AssetId id_;
        };

        [[nodiscard]] AssetImportModal::ImportPreset CapturePresetValues(const Assets::AssetImportItem &item,
                                                                         const Assets::AssetImporterCatalogSnapshot &catalog,
                                                                         std::string name) {
            AssetImportModal::ImportPreset preset{
                .name = std::move(name),
                .destinationFolder = item.destinationFolder,
                .subfolderByType = item.subfolderByType,
                .assetIdStrategy = item.assetIdStrategy,
                .createMetaSidecar = item.createMetaSidecar,
                .overwriteWithoutPrompt = item.overwriteWithoutPrompt,
            };

            const auto *contribution = catalog.FindById(item.importerContributionId);
            if (!contribution)
                return preset;

            for (const auto &descriptor : contribution->settings) {
                if (!descriptor.includeInPresets)
                    continue;
                const std::string key = "settings." + descriptor.id;
                if (const auto value = item.settings.find(key); value != item.settings.end())
                    preset.settings.try_emplace(key, value->second);
            }
            return preset;
        }

        void ApplyPresetValues(Assets::AssetImportItem &item, const AssetImportModal::ImportPreset &preset,
                               const Assets::AssetImporterCatalogSnapshot &catalog) {
            if (const auto *contribution = catalog.FindById(item.importerContributionId)) {
                for (const auto &descriptor : contribution->settings) {
                    if (!descriptor.includeInPresets)
                        continue;
                    const std::string key = "settings." + descriptor.id;
                    item.settings.erase(key);
                    if (const auto value = preset.settings.find(key); value != preset.settings.end())
                        item.settings.try_emplace(key, value->second);
                }
            }

            item.destinationFolder = preset.destinationFolder;
            item.subfolderByType = preset.subfolderByType;
            item.assetIdStrategy = preset.assetIdStrategy;
            item.createMetaSidecar = preset.createMetaSidecar;
            item.overwriteWithoutPrompt = preset.overwriteWithoutPrompt;
        }
    }  // namespace

    AssetImportModal::AssetImportModal(const Theme::Fonts &fonts, JobSystem &jobs,
                                       std::shared_ptr<const Assets::AssetImporterCatalogSnapshot> catalog,
                                       Assets::AssetRegistry *assetRegistry, OperationStore *operationStore,
                                       const ILocalizationService *localization) noexcept
        : m_fonts(fonts), m_jobs(jobs), m_catalog(std::move(catalog)), m_assetRegistry(assetRegistry), m_operationStore(operationStore),
          m_localization(localization) {}

    /** @copydoc AssetImportModal::~AssetImportModal */
    AssetImportModal::~AssetImportModal() = default;

    ModalId AssetImportModal::Id() const {
        return ModalId{kModalId};
    }

    ModalPresentation AssetImportModal::Presentation() const {
        return {.size = ModalSizePolicy::Large, .dimWorkspace = true};
    }

    ModalClosePolicy AssetImportModal::ClosePolicy() const {
        return {
            .allowCloseButton = true,
            .allowEscape = true,
            .allowOutsideClick = false,
            .allowApplicationShutdown = true,
        };
    }

    Result<void> AssetImportModal::OnOpen(EditorModalContext &context) {
        m_events = &context.events;
        m_prepared = false;
        m_queuedFiles.clear();
        m_activePresetNames.clear();
        m_presetsByImporterAndExtension.clear();
        m_defaultPresetValues.clear();
        m_snapshot = Assets::AssetImportSnapshot{};
        m_itemCompleted.clear();
        m_visibleOperationId.reset();
        m_operationCancellation.reset();
        m_historyRevision = 0;
        m_lastTerminalImportId = 0;
        m_pendingImportOperations.clear();
        m_importHistory.clear();
        RefreshImportHistory();

        m_logCtx = std::make_unique<Log::LogContext>("modal", "asset_import", "modal_id", std::to_string(kModalId));
        LOG_INFO("editor.asset_import", "AssetImportModal opened.");

        return Result<void>::Success();
    }

    CloseDecision AssetImportModal::CanClose(const ModalCloseReason reason) {
        using enum CloseDecision;
        if (reason == ModalCloseReason::ApplicationShutdown)
            return Allow;

        // Prevent close while import is in progress
        if (m_snapshot.phase == Assets::AssetImportPhase::Preparing || m_snapshot.phase == Assets::AssetImportPhase::Committing) {
            return Deny;
        }

        return Allow;
    }

    void AssetImportModal::OnClose(const ModalCloseReason reason) {
        const bool completed = reason == ModalCloseReason::Completed || IsImportComplete();
        const char *reasonStr = "app_shutdown";
        if (completed) {
            reasonStr = "completed";
        } else if (reason == ModalCloseReason::Cancelled) {
            reasonStr = "cancelled";
        }
        LOG_INFO("editor.asset_import", "AssetImportModal closed (reason=%s).", reasonStr);

        if (!completed) {
            if (m_operation)
                m_operation->Cancel();
            if (m_operationCancellation)
                m_operationCancellation->RequestCancellation();
        }
        if (m_operationStore != nullptr && m_visibleOperationId.has_value()) {
            static_cast<void>(
                m_operationStore->Update(*m_visibleOperationId,
                                         OperationUpdate{.state = completed ? OperationState::Succeeded : OperationState::Cancelled,
                                                         .phase = completed ? "complete" : "cancelled",
                                                         .message = completed ? "Asset import completed" : "Asset import cancelled",
                                                         .progress = completed ? std::optional{1.0F} : std::nullopt}));
        }
        m_visibleOperationId.reset();

        m_logCtx.reset();
    }

    const Assets::AssetImportSnapshot &AssetImportModal::Snapshot() const noexcept {
        return m_snapshot;
    }

    /** @copydoc AssetImportModal::MutableSnapshot */
    Assets::AssetImportSnapshot &AssetImportModal::MutableSnapshot() noexcept {
        return m_snapshot;
    }

    bool AssetImportModal::IsImportComplete() const noexcept {
        return !m_itemCompleted.empty() && !HasPendingConflicts() && std::ranges::all_of(m_itemCompleted, [](const bool completed) {
            return completed;
        });
    }

    const Assets::AssetImporterCatalogSnapshot &AssetImportModal::Catalog() const noexcept {
        return *m_catalog;
    }

    /** @copydoc AssetImportModal::ImportHistory */
    std::span<const OperationRecord> AssetImportModal::ImportHistory() const noexcept {
        return m_importHistory;
    }

    /** @copydoc AssetImportModal::Localized */
    std::string_view AssetImportModal::Localized(const std::string_view key, const std::string_view fallback) const {
        return m_localization != nullptr ? std::string_view{m_localization->Get("editor", key)} : fallback;
    }

    /** @copydoc AssetImportModal::RefreshImportHistory */
    void AssetImportModal::RefreshImportHistory() {
        if (m_operationStore == nullptr)
            return;
        const auto snapshot = m_operationStore->SnapshotIfChanged(m_historyRevision);
        if (!snapshot.has_value())
            return;
        m_historyRevision = snapshot->revision;
        for (const OperationRecord &operation : snapshot->operations) {
            if (operation.kind != OperationKind::Import)
                continue;
            const bool terminal = operation.state == OperationState::Succeeded || operation.state == OperationState::Failed ||
                                  operation.state == OperationState::Cancelled;
            if (!terminal) {
                m_pendingImportOperations.insert(operation.id);
                continue;
            }
            const bool wasPending = m_pendingImportOperations.erase(operation.id) != 0;
            if (!wasPending && operation.id <= m_lastTerminalImportId)
                continue;
            m_lastTerminalImportId = std::max(m_lastTerminalImportId, operation.id);
            m_importHistory.insert(m_importHistory.begin(), operation);
            if (m_importHistory.size() > kMaximumImportHistory)
                m_importHistory.pop_back();
        }
    }

    void AssetImportModal::SetProjectRoot(const std::filesystem::path &root) noexcept {
        m_projectRoot = root;
    }

    /** @copydoc AssetImportModal::SetDefaultDestination */
    void AssetImportModal::SetDefaultDestination(const std::filesystem::path &absoluteDirectory) noexcept {
        if (m_projectRoot.empty() || !absoluteDirectory.is_absolute())
            return;
        std::error_code error;
        const std::filesystem::path root = std::filesystem::weakly_canonical(m_projectRoot, error);
        if (error)
            return;
        const std::filesystem::path destination = std::filesystem::weakly_canonical(absoluteDirectory, error);
        if (error)
            return;
        const std::filesystem::path relative = destination.lexically_relative(root);
        if (relative.empty() || relative.is_absolute())
            return;
        for (const auto &segment : relative) {
            if (segment == "..")
                return;
        }
        const std::string candidate = relative.generic_string();
        if (ProjectPath::Parse(candidate).HasValue() && (candidate == "assets" || candidate.starts_with("assets/"))) {
            m_defaultDestinationFolder = candidate;
        }
    }

    void AssetImportModal::SelectItem(std::size_t index) {
        if (index < m_snapshot.items.size())
            m_snapshot.selectedItemIndex = index;
    }

    std::vector<std::string> AssetImportModal::PresetNames(const std::size_t index) const {
        std::vector<std::string> names{"Default"};
        if (index >= m_snapshot.items.size())
            return names;

        const auto presets = m_presetsByImporterAndExtension.find(PresetScopeKey(m_snapshot.items[index]));
        if (presets == m_presetsByImporterAndExtension.end())
            return names;

        names.reserve(presets->second.size() + 1);
        for (const auto &preset : presets->second)
            names.push_back(preset.name);
        return names;
    }

    std::string_view AssetImportModal::ActivePresetName(const std::size_t index) const noexcept {
        static constexpr std::string_view kDefaultPreset = "Default";
        if (index >= m_activePresetNames.size() || m_activePresetNames[index].empty())
            return kDefaultPreset;
        return m_activePresetNames[index];
    }

    bool AssetImportModal::ApplyPreset(const std::size_t index, const std::string_view presetName) {
        if (index >= m_snapshot.items.size())
            return false;

        auto &item = m_snapshot.items[index];
        if (presetName == "Default") {
            if (index >= m_defaultPresetValues.size())
                return false;
            ApplyPresetValues(item, m_defaultPresetValues[index], *m_catalog);
        } else {
            const auto presets = m_presetsByImporterAndExtension.find(PresetScopeKey(item));
            if (presets == m_presetsByImporterAndExtension.end())
                return false;
            const auto preset = std::ranges::find_if(presets->second, [presetName](const ImportPreset &candidate) {
                return candidate.name == presetName;
            });
            if (preset == presets->second.end())
                return false;
            ApplyPresetValues(item, *preset, *m_catalog);
        }

        if (m_activePresetNames.size() < m_snapshot.items.size())
            m_activePresetNames.resize(m_snapshot.items.size(), "Default");
        m_activePresetNames[index] = presetName;
        return true;
    }

    bool AssetImportModal::CreatePreset(const std::size_t index, const std::string_view presetName) {
        if (index >= m_snapshot.items.size() || presetName.empty() || presetName == "Default")
            return false;

        const auto &item = m_snapshot.items[index];
        if (item.importerContributionId.empty())
            return false;

        auto &presets = m_presetsByImporterAndExtension[PresetScopeKey(item)];
        if (std::ranges::any_of(presets, [presetName](const ImportPreset &preset) {
            return preset.name == presetName;
        }))
            return false;

        presets.push_back(CapturePresetValues(item, *m_catalog, std::string{presetName}));
        if (m_activePresetNames.size() < m_snapshot.items.size())
            m_activePresetNames.resize(m_snapshot.items.size(), "Default");
        m_activePresetNames[index] = presetName;
        return true;
    }

    Result<void> AssetImportModal::BeginImport(const std::vector<std::filesystem::path> &sourceFiles,
                                               const CancellationToken &cancellation) {
        // Walk up from the first file to find the project root (the directory containing .horo/).
        std::filesystem::path projectRoot = sourceFiles.front().parent_path();
        while (!projectRoot.empty() && projectRoot != projectRoot.root_path()) {
            if (std::filesystem::exists(projectRoot / ".horo" / "project.json"))
                break;
            projectRoot = projectRoot.parent_path();
        }
        if (projectRoot.empty() || projectRoot == projectRoot.root_path())
            projectRoot = sourceFiles.front().parent_path();  // Fallback
        return BeginImport(sourceFiles, projectRoot, cancellation);
    }

    Result<void> AssetImportModal::BeginImport(const std::vector<std::filesystem::path> &sourceFiles,
                                               const std::filesystem::path &projectRoot, const CancellationToken &cancellation) {
        if (!m_catalog) {
            Error err;
            err.code = ErrorCode{"editor.asset_import.no_catalog"};
            err.domain = ErrorDomainId{"horo.editor"};
            return Result<void>::Failure(err);
        }

        m_projectRoot = projectRoot;
        m_committer = Assets::MakeProjectCommitter(m_assetRegistry);

        // If operation already exists, append files; otherwise start a new one.
        if (m_operation) {
            const std::size_t previousItemCount = m_snapshot.items.size();
            auto result = m_operation->AddFiles(sourceFiles, projectRoot, cancellation);
            if (result.HasError())
                return Result<void>::Failure(result.ErrorValue());
            m_snapshot = result.Value();
            m_itemCompleted.resize(m_snapshot.items.size(), false);
            if (!m_defaultDestinationFolder.empty()) {
                for (std::size_t index = previousItemCount; index < m_snapshot.items.size(); ++index)
                    m_snapshot.items[index].destinationFolder = m_defaultDestinationFolder;
            }
            m_activePresetNames.resize(m_snapshot.items.size(), "Default");
            m_defaultPresetValues.reserve(m_snapshot.items.size());
            for (std::size_t index = previousItemCount; index < m_snapshot.items.size(); ++index)
                m_defaultPresetValues.push_back(CapturePresetValues(m_snapshot.items[index], *m_catalog, "Default"));
            LOG_INFO("editor.asset_import", "Added %zu files to queue: %zu total.", sourceFiles.size(), m_snapshot.items.size());
            return Result<void>::Success();
        }

        m_operation = std::make_unique<Assets::AssetImportOperation>(m_jobs, m_catalog);

        Assets::AssetImportRequest request{
            .projectRoot = projectRoot,
            .sourceFiles = sourceFiles,
        };

        m_operationCancellation = std::make_shared<CancellationSource>(cancellation);
        auto result = m_operation->Start(request, m_operationCancellation->Token());
        if (result.HasError())
            return Result<void>::Failure(result.ErrorValue());

        m_snapshot = result.Value();
        m_itemCompleted.assign(m_snapshot.items.size(), false);
        if (!m_defaultDestinationFolder.empty()) {
            for (auto &item : m_snapshot.items)
                item.destinationFolder = m_defaultDestinationFolder;
        }
        m_activePresetNames.assign(m_snapshot.items.size(), "Default");
        m_defaultPresetValues.clear();
        m_defaultPresetValues.reserve(m_snapshot.items.size());
        for (const auto &item : m_snapshot.items)
            m_defaultPresetValues.push_back(CapturePresetValues(item, *m_catalog, "Default"));
        LOG_INFO("editor.asset_import", "Import started: %zu files.", m_snapshot.items.size());
        if (m_operationStore != nullptr) {
            std::string historyTitle = m_snapshot.items.front().displayName;
            if (m_snapshot.items.size() > 1)
                historyTitle += std::format(" +{}", m_snapshot.items.size() - 1);
            const std::weak_ptr weakCancellation = m_operationCancellation;
            m_visibleOperationId = m_operationStore->Begin(OperationDescriptor{
                .kind = OperationKind::Import,
                .title = historyTitle,
                .phase = "prepare",
                .message = std::format("{} files", m_snapshot.items.size()),
                .progress = 0.0F,
                .cancellable = true,
                .requestCancel =
                    [weakCancellation] {
                if (const auto cancellation = weakCancellation.lock())
                    cancellation->RequestCancellation();
            },
            });
            if (m_visibleOperationId.has_value())
                static_cast<void>(m_operationStore->Update(*m_visibleOperationId, OperationUpdate{.state = OperationState::Running,
                                                                                                  .phase = "import",
                                                                                                  .message = "Importing assets",
                                                                                                  .progress = 0.0F}));
        }

        return Result<void>::Success();
    }

    Result<void> AssetImportModal::PrepareImport(const CancellationToken &cancellation) const {
        // No longer needed — import happens per-file via ImportSingleItem.
        // Kept for API compatibility with headless tests.
        (void)cancellation;
        return Result<void>::Success();
    }

    Result<void> AssetImportModal::ImportSingleItem(const std::size_t index, const CancellationToken &cancellation) {
        if (const auto validated = ValidateImportItem(m_snapshot, index, m_operation != nullptr); validated.HasError())
            return validated;

        if (const auto settingsResult = m_operation->SetItemSettings(index, m_snapshot.items[index].settings); settingsResult.HasError()) {
            return Result<void>::Failure(settingsResult.ErrorValue());
        }

        const CancellationToken operationCancellation = m_operationCancellation ? m_operationCancellation->Token() : cancellation;
        auto result = m_operation->ImportSingleItem(index, operationCancellation);
        if (result.HasError()) {
            if (m_operationStore != nullptr && m_visibleOperationId.has_value())
                static_cast<void>(
                    m_operationStore->Update(*m_visibleOperationId, OperationUpdate{.state = operationCancellation.IsCancellationRequested()
                                                                                                 ? OperationState::Cancelled
                                                                                                 : OperationState::Failed,
                                                                                    .phase = "import",
                                                                                    .message = result.ErrorValue().message,
                                                                                    .error = result.ErrorValue()}));
            return Result<void>::Failure(result.ErrorValue());
        }

        // Merge only the imported item's fields from the operation result,
        // preserving user-modified UI state (e.g. destinationFolder) on other items.
        const auto &opSnapshot = result.Value();
        auto &targetItem = m_snapshot.items[index];
        targetItem.diagnostics = opSnapshot.items[index].diagnostics;
        targetItem.result = opSnapshot.items[index].result;
        targetItem.resolvedType = opSnapshot.items[index].resolvedType;
        targetItem.sourceHash = opSnapshot.items[index].sourceHash;
        targetItem.sourceByteSize = opSnapshot.items[index].sourceByteSize;
        targetItem.sourceLastWriteTime = opSnapshot.items[index].sourceLastWriteTime;
        m_snapshot.phase = opSnapshot.phase;
        m_snapshot.revision = opSnapshot.revision;

        LOG_INFO("editor.asset_import", "Imported item %zu: %s", index, m_snapshot.items[index].displayName.c_str());

        // Check for conflicts before committing
        auto commitItem = m_snapshot.items[index];  // mutable copy for name/folder transforms

        // Skip items that failed to import (no result payload)
        if (!commitItem.result.has_value()) {
            LOG_INFO("editor.asset_import", "Skipping commit for %s — import produced no result.", commitItem.displayName.c_str());
            MarkItemCompleted(index);
            return Result<void>::Success();
        }

        const Assets::AssetId assetId = MakeAssetId(commitItem);

        // Apply naming convention
        if (commitItem.namingConvention == 1)  // Lowercase + underscore
        {
            std::string name = commitItem.displayName;
            std::ranges::transform(name, name.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            std::ranges::replace(name, ' ', '_');
            commitItem.displayName = name;
        } else if (commitItem.namingConvention == 2)  // AssetId prefix
        {
            commitItem.displayName = assetId.ToString().substr(0, 12) + "_" + commitItem.displayName;
        }
        // convention 0 = Preserve source name (no transform)

        const std::string destFolder = ResolveDestinationFolder(commitItem, m_catalog.get());

        // Conflict check (skip if overwriteWithoutPrompt)
        if (!commitItem.overwriteWithoutPrompt && WouldConflict(commitItem)) {
            ConflictItem conflict;
            conflict.assetName = commitItem.displayName + "." + commitItem.sourceExtension;
            conflict.conflictDescription = "An asset with this name already exists.";
            conflict.snapshotIndex = index;
            m_conflictQueue.push_back(std::move(conflict));
            LOG_INFO("editor.asset_import", "Conflict detected for %s — queued for resolution", commitItem.displayName.c_str());
            if (m_operationStore != nullptr && m_visibleOperationId.has_value())
                static_cast<void>(
                    m_operationStore->Update(*m_visibleOperationId, OperationUpdate{.state = OperationState::Waiting,
                                                                                    .phase = "resolve conflicts",
                                                                                    .message = "Waiting for conflict resolution"}));
        } else {
            // Commit to project storage
            if (m_committer) {
                Assets::PreparedAssetImportBatch batch{
                    .operationId = m_snapshot.operationId,
                    .projectRoot = m_projectRoot,
                    .destinationFolder = ProjectPath::Parse(destFolder).Value(),
                    .items = {commitItem},
                };

                FixedAssetIdGenerator idGen{assetId};
                CancellationToken noCancel;
                auto commitResult = m_committer->Commit(batch, idGen, noCancel);
                if (commitResult.HasError()) {
                    LOG_ERROR("editor.asset_import", "Commit failed for item %zu: %s", index, commitResult.ErrorValue().message.c_str());
                    FailVisibleOperation(commitResult.ErrorValue(), "commit");
                    return Result<void>::Failure(commitResult.ErrorValue());
                } else {
                    LOG_INFO("editor.asset_import", "Committed item %zu to project storage.", index);
                }
            }
            MarkItemCompleted(index);
        }

        return Result<void>::Success();
    }

    void AssetImportModal::MarkItemCompleted(const std::size_t index) {
        if (index >= m_itemCompleted.size())
            return;
        m_itemCompleted[index] = true;

        if (m_operationStore == nullptr || !m_visibleOperationId.has_value())
            return;

        const auto completedItems = static_cast<std::size_t>(std::ranges::count(m_itemCompleted, true));
        const float progress =
            m_itemCompleted.empty() ? 1.0F : static_cast<float>(completedItems) / static_cast<float>(m_itemCompleted.size());
        if (IsImportComplete()) {
            static_cast<void>(m_operationStore->Update(*m_visibleOperationId, OperationUpdate{.state = OperationState::Succeeded,
                                                                                              .phase = "complete",
                                                                                              .message = "Asset import completed",
                                                                                              .progress = 1.0F}));
            m_visibleOperationId.reset();
            return;
        }

        const bool waitingForConflict = HasPendingConflicts();
        static_cast<void>(
            m_operationStore->Update(*m_visibleOperationId,
                                     OperationUpdate{.state = waitingForConflict ? OperationState::Waiting : OperationState::Running,
                                                     .phase = waitingForConflict ? "resolve conflicts" : "import",
                                                     .message = waitingForConflict
                                                                    ? "Waiting for conflict resolution"
                                                                    : std::format("{} of {}", completedItems, m_itemCompleted.size()),
                                                     .progress = progress}));
    }

    void AssetImportModal::FailVisibleOperation(const Error &error, const std::string_view phase) {
        if (m_operationStore == nullptr || !m_visibleOperationId.has_value())
            return;
        static_cast<void>(m_operationStore->Update(*m_visibleOperationId, OperationUpdate{.state = OperationState::Failed,
                                                                                          .phase = std::string{phase},
                                                                                          .message = error.message,
                                                                                          .error = error}));
        m_visibleOperationId.reset();
    }

    // -------------------------------------------------------------------------
    // Conflict resolution
    // -------------------------------------------------------------------------

    bool AssetImportModal::WouldConflict(const Assets::AssetImportItem &item) const {
        if (m_projectRoot.empty() || !m_catalog)
            return false;

        const std::string destinationFolder = ResolveDestinationFolder(item, m_catalog.get());
        std::filesystem::path targetPath = m_projectRoot / destinationFolder / (item.displayName + item.targetExtension);
        std::error_code ec;
        return std::filesystem::exists(targetPath, ec);
    }

    bool AssetImportModal::CommitCurrentItem(const Assets::AssetImportItem &item, const ConflictChoice choice,
                                             [[maybe_unused]] bool applyAll) {
        if (choice == ConflictChoice::Skip) {
            LOG_INFO("editor.asset_import", "Skipped import for %s (user chose Skip)", item.displayName.c_str());
            return true;
        }

        auto commitItem = item;  // mutable copy

        const Assets::AssetId assetId = MakeAssetId(commitItem);

        // Apply naming convention
        if (commitItem.namingConvention == 1)  // Lowercase + underscore
        {
            std::string name = commitItem.displayName;
            std::ranges::transform(name, name.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            std::ranges::replace(name, ' ', '_');
            commitItem.displayName = name;
        } else if (commitItem.namingConvention == 2)  // AssetId prefix
        {
            commitItem.displayName = assetId.ToString().substr(0, 12) + "_" + commitItem.displayName;
        }

        const std::string destFolder = ResolveDestinationFolder(commitItem, m_catalog.get());

        std::string assetName = commitItem.displayName;
        if (choice == ConflictChoice::Rename) {
            // Simple rename: append _1, _2, etc.
            int suffix = 1;
            std::filesystem::path base = m_projectRoot / destFolder;
            while (std::filesystem::exists(base / (assetName + commitItem.targetExtension)))
                assetName = std::format("{}_{}", commitItem.displayName, ++suffix);
        }

        // Build a modified batch with the (possibly renamed) name
        commitItem.displayName = assetName;
        Assets::PreparedAssetImportBatch batch{
            .operationId = m_snapshot.operationId,
            .projectRoot = m_projectRoot,
            .destinationFolder = ProjectPath::Parse(destFolder).Value(),
            .items = {commitItem},
        };

        FixedAssetIdGenerator idGen{assetId};

        CancellationToken noCancel;
        if (const auto commitResult = m_committer->Commit(batch, idGen, noCancel); commitResult.HasError()) {
            LOG_ERROR("editor.asset_import", "Commit failed for %s: %s", assetName.c_str(), commitResult.ErrorValue().message.c_str());
            FailVisibleOperation(commitResult.ErrorValue(), "commit");
            return false;
        } else {
            LOG_INFO("editor.asset_import", "Committed %s (choice=%d)", assetName.c_str(), static_cast<int>(choice));
        }
        return true;
    }

    void AssetImportModal::ResolveCurrentConflict(ConflictChoice choice, bool applyAll) {
        if (m_conflictCursor >= m_conflictQueue.size())
            return;

        if (applyAll)
            m_applyAllChoice = choice;

        const auto &conflict = m_conflictQueue[m_conflictCursor];
        const auto &item = m_snapshot.items[conflict.snapshotIndex];
        const std::size_t completedIndex = conflict.snapshotIndex;
        const bool resolved = CommitCurrentItem(item, choice, applyAll);

        ++m_conflictCursor;
        if (m_conflictCursor >= m_conflictQueue.size()) {
            m_conflictQueue.clear();
            m_conflictCursor = 0;
        }
        if (resolved)
            MarkItemCompleted(completedIndex);
    }
}  // namespace Horo::Editor
