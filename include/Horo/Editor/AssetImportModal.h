#pragma once

/**
 * @file AssetImportModal.h
 * @brief Host-owned Asset Import workflow modal and its transient operation state.
 */

#include "Horo/Assets/AssetImportOperation.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Logging/LogContext.h"
#include "Horo/Foundation/OperationStore.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Horo::Editor::Theme {
    struct Fonts;
}

namespace Horo::Assets {
    class AssetImporterCatalogSnapshot;
    class ProjectAssetImportCommitter;
}  // namespace Horo::Assets

namespace Horo::Editor {
    class ILocalizationService;

    /**
     * @brief Host-owned asset import workflow modal.
     * @details Owns an AssetImportOperation and exposes its snapshots to the GUI
     *          presentation layer. The modal lifecycle is:
     *          OnOpen → Draw (repeated) → CanClose → OnClose.
     *
     *          The modal does not own importer strategies, project mutation,
     *          or ImGui state. Those are injected through context.
     */
    class AssetImportModal : public EditorModal {  // NOSONAR(cpp:S1820)
    public:
        static constexpr std::uint64_t kModalId = 0x41534D504F525449ULL;

        /**
         * @brief Constructs the import workflow modal.
         * @param fonts Theme fonts reference (valid for modal lifetime).
         * @param jobs Job system for background import work.
         * @param catalog Published immutable importer catalog snapshot.
         * @param assetRegistry Optional mutable asset registry updated by committed imports.
         * @param operationStore Optional user-facing operation authority.
         * @param localization Optional editor localization service used by presentation copy.
         */
        AssetImportModal(const Theme::Fonts &fonts, JobSystem &jobs, std::shared_ptr<const Assets::AssetImporterCatalogSnapshot> catalog,
                         Assets::AssetRegistry *assetRegistry = nullptr, OperationStore *operationStore = nullptr,
                         const ILocalizationService *localization = nullptr) noexcept;

        /** @brief Destroys the modal and its target-private project committer. */
        ~AssetImportModal() override;

        [[nodiscard]] ModalId Id() const override;
        [[nodiscard]] ModalPresentation Presentation() const override;
        [[nodiscard]] ModalClosePolicy ClosePolicy() const override;
        [[nodiscard]] Result<void> OnOpen(EditorModalContext &context) override;
        [[nodiscard]] ModalFrameResult Draw() override;
        [[nodiscard]] CloseDecision CanClose(ModalCloseReason reason) override;
        void OnClose(ModalCloseReason reason) override;

        /** @brief Returns the current operation snapshot for presentation rendering. */
        [[nodiscard]] const Assets::AssetImportSnapshot &Snapshot() const noexcept;

        /** @brief Returns mutable access to the operation snapshot for modal editing. */
        [[nodiscard]] Assets::AssetImportSnapshot &MutableSnapshot() noexcept;

        /** @brief Returns the pinned importer catalog snapshot. */
        [[nodiscard]] const Assets::AssetImporterCatalogSnapshot &Catalog() const noexcept;

        /** @brief Returns retained terminal import operations, newest first. */
        [[nodiscard]] std::span<const OperationRecord> ImportHistory() const noexcept;

        /** @brief Resolves presentation copy with a fallback for headless callers. */
        [[nodiscard]] std::string_view Localized(std::string_view key, std::string_view fallback) const;

        /** @brief Sets the project root for asset destination paths. Call before BeginImport when known. */
        void SetProjectRoot(const std::filesystem::path &root) noexcept;

        /**
         * @brief Sets the absolute asset directory applied to files subsequently added to the queue.
         * @param absoluteDirectory Existing directory beneath the active project's assets root.
         */
        void SetDefaultDestination(const std::filesystem::path &absoluteDirectory) noexcept;

        /** @brief Returns the default project-relative destination for newly added files. */
        [[nodiscard]] std::string_view DefaultDestinationFolder() const noexcept;

        /** @brief Returns the stored project root (empty if not set). */
        [[nodiscard]] const std::filesystem::path &ProjectRoot() const noexcept;

        /** @brief Initiates an import operation with the given source files. */
        [[nodiscard]] Result<void> BeginImport(const std::vector<std::filesystem::path> &sourceFiles,
                                               const CancellationToken &cancellation);
        [[nodiscard]] Result<void> BeginImport(const std::vector<std::filesystem::path> &sourceFiles,
                                               const std::filesystem::path &projectRoot, const CancellationToken &cancellation);

        /** @brief Imports a single item by queue index. */
        [[nodiscard]] Result<void> ImportSingleItem(std::size_t index, const CancellationToken &cancellation);

        /** @brief Returns whether every queued item reached a committed or explicitly skipped terminal result. */
        [[nodiscard]] bool IsImportComplete() const noexcept;

        /** @brief Runs the import preparation phase. */
        [[nodiscard]] Result<void> PrepareImport(const CancellationToken &cancellation) const;

        /** @brief Selects an item by index for the settings panel. */
        void SelectItem(std::size_t index);

        /** @brief Returns whether a queued item is selected for the batch import. */
        [[nodiscard]] bool IsItemIncluded(std::size_t index) const noexcept;

        /** @brief Includes or excludes a queued item before it has been imported. */
        void SetItemIncluded(std::size_t index, bool included) noexcept;

        /** @brief Returns the number of selected items still available to import. */
        [[nodiscard]] std::size_t IncludedItemCount() const noexcept;

        /** @brief Returns the source file size captured when a queued item was added. */
        [[nodiscard]] std::optional<std::uintmax_t> SourceFileSize(std::size_t index) const noexcept;

        /** @brief Representative, in-memory states exposed by the native UI gallery. */
        enum class UiPreviewFixture { Populated, Empty };

        /** @brief Requests an inert representative UI state when the modal opens. */
        void RequestUiPreviewFixture(UiPreviewFixture fixture) noexcept {
            m_pendingUiPreviewFixture = true;
            m_pendingUiPreviewKind = fixture;
        }

        /** @brief Returns whether this modal is displaying an inert UI preview. */
        [[nodiscard]] bool IsUiPreview() const noexcept {
            return m_uiPreviewMode;
        }

        /** @brief Imports selected pending items in queue order. */
        [[nodiscard]] Result<void> ImportIncludedItems(const CancellationToken &cancellation);

        /** @brief Named importer-settings snapshot scoped to one importer contribution. */
        struct ImportPreset {
            std::string name;                                       /**< User-visible preset name, unique within its importer. */
            std::unordered_map<std::string, std::string> settings;  // NOSONAR(cpp:S6045)
            std::string destinationFolder;                          /**< Project-relative destination retained by the preset. */
            int subfolderByType{0};                                 /**< Destination organization mode retained by the preset. */
            int assetIdStrategy{0};                                 /**< Asset identity strategy retained by the preset. */
            bool createMetaSidecar{true};                           /**< Meta-sidecar choice retained by the preset. */
            bool overwriteWithoutPrompt{false};                     /**< Conflict policy retained by the preset. */
        };

        /**
         * @brief Returns preset names available to a queued item's importer and extension.
         * @param index Queue index whose importer/extension preset scope is requested.
         * @return Ordered preset names for the compact footer selector.
         */
        [[nodiscard]] std::vector<std::string> PresetNames(std::size_t index) const;

        /**
         * @brief Returns the active preset name for a queued item.
         * @param index Queue index.
         * @return Active preset name, or Default for an invalid/new item.
         */
        [[nodiscard]] std::string_view ActivePresetName(std::size_t index) const noexcept;

        /**
         * @brief Applies a named importer preset to one queued item.

         * @param index Queue index.
         * @param presetName Preset name; Default restores schema defaults.
         * @return True when the preset exists and was applied.
         */
        [[nodiscard]] bool ApplyPreset(std::size_t index, std::string_view presetName);

        /**
         * @brief Captures the current settings of one queued item as a named preset.
         * @param index Queue index.
         * @param presetName Non-empty name unique within the selected importer.
         * @return True when the preset was created.
         */
        [[nodiscard]] bool CreatePreset(std::size_t index, std::string_view presetName);

        /** @brief Conflict resolution choice for the popup. */
        enum class ConflictChoice : std::uint8_t {
            Overwrite,
            Rename,
            Skip,
        };

        /** @brief Per-file conflict pending resolution. */
        struct ConflictItem {
            std::string assetName;
            std::string conflictDescription;
            std::size_t snapshotIndex{0};
        };

        /** @brief Returns true when a conflict popup should be shown. */
        [[nodiscard]] bool HasPendingConflicts() const noexcept {
            return !m_conflictQueue.empty();
        }

        /** @brief Returns the current conflict (front of queue). */
        [[nodiscard]] const ConflictItem &CurrentConflict() const {
            return m_conflictQueue[m_conflictCursor];
        }

        /** @brief True when there are remaining conflicts after the current one. */
        [[nodiscard]] bool HasMoreConflicts() const noexcept {
            return m_conflictCursor + 1 < m_conflictQueue.size();
        }

        /** @brief Apply the chosen resolution and advance to next conflict. */
        void ResolveCurrentConflict(ConflictChoice choice, bool applyAll);

    private:
        /** @brief Checks whether importing @p item would overwrite an existing asset. */
        bool WouldConflict(const Assets::AssetImportItem &item) const;

        /** @brief Commits or explicitly skips one conflicted item. */
        [[nodiscard]] bool CommitCurrentItem(const Assets::AssetImportItem &item, ConflictChoice choice, bool applyAll);

        /** @brief Records one terminal item result and completes the visible operation when the queue is finished. */
        void MarkItemCompleted(std::size_t index);

        /** @brief Moves the visible operation to failed and releases its active handle. */
        void FailVisibleOperation(const Error &error, std::string_view phase);

        /** @brief Refreshes the retained import-operation projection from the process store. */
        void RefreshImportHistory();

        /** @brief Installs the requested preview state after normal modal initialization. */
        void LoadUiPreviewFixture(UiPreviewFixture fixture);

        const Theme::Fonts &m_fonts;
        JobSystem &m_jobs;
        std::shared_ptr<const Assets::AssetImporterCatalogSnapshot> m_catalog;
        Assets::AssetRegistry *m_assetRegistry{};
        OperationStore *m_operationStore{};
        const ILocalizationService *m_localization{};
        std::optional<OperationId> m_visibleOperationId;
        std::uint64_t m_historyRevision{};
        OperationId m_lastTerminalImportId{};
        std::unordered_set<OperationId> m_pendingImportOperations;
        std::vector<OperationRecord> m_importHistory;
        std::shared_ptr<CancellationSource> m_operationCancellation;
        EditorDataBus *m_events = nullptr;
        std::filesystem::path m_projectRoot; /**< Stored for committer. */
        std::string m_defaultDestinationFolder;

        std::unique_ptr<Assets::AssetImportOperation> m_operation;
        std::unique_ptr<Assets::ProjectAssetImportCommitter> m_committer;
        Assets::AssetImportSnapshot m_snapshot;
        std::vector<bool> m_itemCompleted;
        std::vector<bool> m_includedItems;
        std::vector<std::optional<std::uintmax_t>> m_sourceFileSizes;
        bool m_uiPreviewMode{false};
        bool m_pendingUiPreviewFixture{false};
        UiPreviewFixture m_pendingUiPreviewKind{UiPreviewFixture::Populated};
        bool m_prepared{false};

        // Conflict resolution popup state
        std::vector<ConflictItem> m_conflictQueue;
        std::size_t m_conflictCursor{0};
        ConflictChoice m_applyAllChoice{ConflictChoice::Skip};

        // Source files queued for import
        std::vector<std::filesystem::path> m_queuedFiles;

        // Presets are separated by stable contribution ID and source extension.
        std::unordered_map<std::string, std::vector<ImportPreset>> m_presetsByImporterAndExtension;  // NOSONAR(cpp:S6045)

        std::vector<std::string> m_activePresetNames;
        std::vector<ImportPreset> m_defaultPresetValues;

        /// @brief RAII MDC frame active for the lifetime of this modal.
        std::unique_ptr<Log::LogContext> m_logCtx;
    };
}  // namespace Horo::Editor
