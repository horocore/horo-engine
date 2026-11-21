#pragma once

#include "Horo/Editor/SourceDocumentService.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace Horo::Editor::Detail {
    struct SourceText final {
        std::string bytes;
        SourceTextMetadata metadata;
    };

    struct SourceDocumentRoot final {
        DocumentIdentity identity;
        /** @brief Shared per-incarnation witness. The owner writes the exact revision before publication and zero before retirement.
         * Worker snapshots only read it. It outlives the owner until the last immutable root is released.
         */
        std::shared_ptr<std::atomic<std::uint64_t>> currentRevision;
        std::uint64_t revision{1};
        std::uint64_t baseRevision{1};
        std::shared_ptr<const SourceText> text;
        std::shared_ptr<const SourceText> base;
        SourceExternalState external{SourceExternalState::InSync};
        bool saveUnconfirmed{};
    };

    /** @brief Validates bounded scalar text and captures exact non-normalized encoding metadata. */
    [[nodiscard]] Result<std::shared_ptr<const SourceText>> CaptureSourceText(std::string bytes, std::size_t maximumBytes,
                                                                              CancellationToken cancellation);
    /** @brief Reads a complete bounded file with cancellation and concurrent disk-change rejection. */
    [[nodiscard]] Result<std::shared_ptr<const SourceText>> LoadSourceText(const std::filesystem::path &path, std::size_t maximumBytes,
                                                                           CancellationToken cancellation);
    /** @brief Verifies exact UTF-8 canonical key and native project containment without granting file authority. */
    [[nodiscard]] Result<std::filesystem::path> SourcePath(const std::filesystem::path &projectRoot, const DocumentIdentity &identity,
                                                           const std::filesystem::path &path);
    /** @brief Prepares the next immutable-root revision without changing the old publication. */
    [[nodiscard]] Result<std::shared_ptr<SourceDocumentRoot>> NextRoot(const SourceDocumentRoot &previous);
}  // namespace Horo::Editor::Detail

namespace Horo::Editor {
    struct SourceDocumentService::Storage final {
        struct HistoryEntry final {
            std::size_t offset{};
            std::string removed;
            std::string inserted;
            SourceEditingView before;
            SourceEditingView after;

            [[nodiscard]] std::size_t Bytes() const noexcept {
                return removed.size() + inserted.size();
            }
        };

        struct HistoryStep final {
            SourceTextEdit patch;
            SourceEditingView view;
            std::size_t position{};
        };

        struct Record final {
            std::filesystem::path path;
            std::shared_ptr<const Detail::SourceDocumentRoot> root;
            std::vector<HistoryEntry> history{};
            std::size_t historyPosition{};
            std::size_t historyBytes{};
            SourceHistoryBoundary historyBoundary{SourceHistoryBoundary::None};
            std::uint64_t autosaveRevision{};
            float autosaveElapsed{};
            float autosaveRetry{};
            bool autosaveDirty{};

            /** @brief Changes the current publication and invalidates the previous worker witness before exposing the new root. */
            void Publish(std::shared_ptr<const Detail::SourceDocumentRoot> next) noexcept {
                next->currentRevision->store(next->revision);
                root = std::move(next);
            }

            /** @brief Revokes worker admission before the owner removes this record. */
            void Revoke() const noexcept {
                root->currentRevision->store(0);
            }

            /** @brief Borrows one available replay delta without changing history. */
            [[nodiscard]] std::optional<HistoryStep> Step(bool redo) const;
        };

        struct StagedBatchEdit final {
            Record *record;
            std::shared_ptr<const Detail::SourceDocumentRoot> next;
            std::optional<HistoryEntry> history;
        };

        std::filesystem::path projectRoot;
        SourceDocumentLimits limits;
        std::thread::id owner{std::this_thread::get_id()};
        bool closed{};
        bool saving{};
        std::vector<Record> records;

        ~Storage() noexcept {
            RevokeAll();
        }

        /** @brief Revokes all extant document witnesses before removing the owner. */
        void RevokeAll() const noexcept {
            for (const auto &record : records)
                record.Revoke();
        }

        /** @brief Copies and validates one history entry and reserves capacity before publication. */
        [[nodiscard]] Result<HistoryEntry> PrepareHistory(Record &record, const SourceTextEdit &edit,
                                                          const Detail::SourceText &after) const;
        /** @brief Commits a prepared entry without throwing; evicts obsolete redo/oldest entries at finite caps. */
        void CommitHistory(Record &record, HistoryEntry entry) const noexcept;
        /** @brief Validates owner-thread access, open lifecycle and project admission. */
        [[nodiscard]] Result<void> Check() const;
        /** @brief Reserves candidate bytes alongside all currently owned distinct buffers. */
        [[nodiscard]] Result<void> AdmitBytes(std::size_t candidateBytes) const;
        /** @brief Borrows an admitted session record on the owner thread. */
        [[nodiscard]] Record *Find(DocumentInstanceId instance);
        /** @brief Reserves a unique new session and its maximum temporary load. */
        [[nodiscard]] Result<void> AdmitNew(const DocumentIdentity &identity) const;
        /** @brief Prepares and publishes an open root only after validated owner access. */
        [[nodiscard]] Result<std::shared_ptr<const Detail::SourceDocumentRoot>> OpenRoot(const DocumentIdentity &identity,
                                                                                         const std::filesystem::path &absolutePath,
                                                                                         CancellationToken cancellation);
        /** @brief Validates/reserves/copies one borrowed patch without changing the current root. */
        [[nodiscard]] Result<std::shared_ptr<const Detail::SourceText>> PrepareEdit(const Record &record, const SourceTextEdit &edit,
                                                                                    CancellationToken cancellation,
                                                                                    std::size_t reservedBytes = 0) const;
        /** @brief Resolves a supplied snapshot to its exact, still-current record. */
        [[nodiscard]] Record *FindCurrent(const SourceDocumentSnapshot &snapshot);
        /** @brief Reads only the authorized disk source and rejects any newer external bytes before batch publication. */
        [[nodiscard]] Result<void> VerifyBatchBase(const Record &record, CancellationToken cancellation) const;
        /** @brief Prepares a replacement root while accounting for other staged buffers. */
        [[nodiscard]] Result<std::shared_ptr<const Detail::SourceDocumentRoot>> PrepareBatchRoot(const Record &record,
                                                                                                 const SourceTextEdit &edit,
                                                                                                 CancellationToken cancellation,
                                                                                                 std::size_t reservedBytes) const;
        /** @brief Stages one batch edit and reserves its history before any publication. */
        [[nodiscard]] Result<StagedBatchEdit> StageBatchEdit(Record &record, const SourceTextEdit &edit, CancellationToken cancellation,
                                                             std::size_t reservedBytes) const;
        /** @brief Validates save intent/path and read reservation before filesystem callback admission. */
        [[nodiscard]] Result<std::filesystem::path> PrepareSavePath(const Record &record, const SourceSaveRequest &request,
                                                                    const DocumentIdentity &identity, std::filesystem::path path,
                                                                    CancellationToken cancellation) const;
        /** @brief Revalidates the stored path and reserves a bounded complete disk observation. */
        [[nodiscard]] Result<std::shared_ptr<const Detail::SourceText>> ReadRecord(const Record &record,
                                                                                   CancellationToken cancellation) const;
    };
}  // namespace Horo::Editor
