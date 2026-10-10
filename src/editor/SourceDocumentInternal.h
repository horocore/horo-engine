#pragma once

#include "Horo/Editor/SourceDocumentService.h"

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
    /** @brief Verifies exact canonical key and project containment without granting file authority. */
    [[nodiscard]] Result<std::filesystem::path> SourcePath(const std::filesystem::path &projectRoot, const DocumentIdentity &identity,
                                                           const std::filesystem::path &path);
    /** @brief Prepares the next immutable-root revision without changing the old publication. */
    [[nodiscard]] Result<std::shared_ptr<SourceDocumentRoot>> NextRoot(const SourceDocumentRoot &previous);
}  // namespace Horo::Editor::Detail

namespace Horo::Editor {
    struct SourceDocumentService::Storage final {
        struct Record final {
            std::filesystem::path path;
            std::shared_ptr<const Detail::SourceDocumentRoot> root;
        };

        std::filesystem::path projectRoot;
        SourceDocumentLimits limits;
        std::thread::id owner{std::this_thread::get_id()};
        bool closed{};
        bool saving{};
        std::vector<Record> records;
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
                                                                                    CancellationToken cancellation) const;
        /** @brief Validates save intent/path and read reservation before filesystem callback admission. */
        [[nodiscard]] Result<std::filesystem::path> PrepareSavePath(const Record &record, const SourceSaveRequest &request,
                                                                    const DocumentIdentity &identity, std::filesystem::path path,
                                                                    CancellationToken cancellation) const;
        /** @brief Revalidates the stored path and reserves a bounded complete disk observation. */
        [[nodiscard]] Result<std::shared_ptr<const Detail::SourceText>> ReadRecord(const Record &record,
                                                                                   CancellationToken cancellation) const;
    };
}  // namespace Horo::Editor
