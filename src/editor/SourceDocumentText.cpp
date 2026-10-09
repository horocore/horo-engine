#include "Horo/Foundation/Utf8.h"
#include "SourceDocumentInternal.h"

#include <array>
#include <fstream>
#include <new>

namespace Horo::Editor {
    namespace {
        /** @brief Defines stable source-document errors without path or buffer disclosure. */
        ErrorCodeDescriptor Descriptor(const char *code, const char *summary) {
            return {.domain = ErrorDomainId{"horo.editor.source_document"},
                    .code = ErrorCode{code},
                    .defaultSeverity = ErrorSeverity::Error,
                    .summary = summary};
        }
    }  // namespace

    namespace SourceDocumentErrors {
        const ErrorCodeDescriptor Invalid = Descriptor("editor.source_document.invalid", "Invalid source document request or path.");
        const ErrorCodeDescriptor TooLarge = Descriptor("editor.source_document.too_large", "Source document reservation exceeded.");
        const ErrorCodeDescriptor Binary = Descriptor("editor.source_document.binary", "Binary content is not editable source text.");
        const ErrorCodeDescriptor Encoding = Descriptor("editor.source_document.encoding", "Source text is not canonical UTF-8.");
        const ErrorCodeDescriptor Removed = Descriptor("editor.source_document.removed", "The source file was removed.");
        const ErrorCodeDescriptor ReadFailed = Descriptor("editor.source_document.read_failed", "The source file could not be read.");
        const ErrorCodeDescriptor DiskChanged = Descriptor("editor.source_document.disk_changed", "Source changed during bounded loading.");
        const ErrorCodeDescriptor Stale = Descriptor("editor.source_document.stale", "Source session or revision is stale.");
        const ErrorCodeDescriptor Closed = Descriptor("editor.source_document.closed", "Source document owner is closed.");
        const ErrorCodeDescriptor WrongThread =
            Descriptor("editor.source_document.wrong_thread", "Source owner called from another thread.");
        const ErrorCodeDescriptor Cancelled = Descriptor("editor.source_document.cancelled", "Source document work was cancelled.");
    }  // namespace SourceDocumentErrors
}  // namespace Horo::Editor

namespace Horo::Editor::Detail {
    namespace {
        template <class T> Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Rejects binary C0 controls while preserving text whitespace. */
        bool IsBinaryControl(const unsigned char byte) {
            return byte < 32 && byte != '\t' && byte != '\n' && byte != '\r' && byte != '\f';
        }

        /** @brief Recognizes one newline family, consuming both bytes of CRLF. */
        unsigned NewlineFamily(const std::string_view bytes, std::size_t &index) {
            if (bytes[index] == '\n')
                return 1;
            if (bytes[index] != '\r')
                return 0;
            if (index + 1 < bytes.size() && bytes[index + 1] == '\n') {
                ++index;
                return 2;
            }
            return 4;
        }

        /** @brief Maps observed newline families without normalizing source bytes. */
        SourceNewlines DecodeNewlines(const unsigned families) {
            using enum SourceNewlines;
            switch (families) {
                case 0:
                    return None;
                case 1:
                    return Lf;
                case 2:
                    return CrLf;
                case 4:
                    return Cr;
                default:
                    return Mixed;
            }
        }

        /** @brief Captures encoding metadata with bounded cancellation checks and binary rejection. */
        Result<SourceTextMetadata> InspectText(const std::string_view bytes, const CancellationToken cancellation) {
            SourceTextMetadata metadata{.utf8Bom = bytes.starts_with("\xef\xbb\xbf")};
            unsigned families{};
            std::size_t nextCancellationCheck{};
            std::size_t index{};
            while (index < bytes.size()) {
                // Check one byte early if needed: CRLF consumption must not skip a 4 KiB fence.
                if (index + 1 >= nextCancellationCheck) {
                    if (cancellation.IsCancellationRequested())
                        return Failure<SourceTextMetadata>(SourceDocumentErrors::Cancelled);
                    nextCancellationCheck = index + 4096;
                }
                if (IsBinaryControl(static_cast<unsigned char>(bytes[index])))
                    return Failure<SourceTextMetadata>(SourceDocumentErrors::Binary);
                families |= NewlineFamily(bytes, index);
                ++index;
            }
            metadata.newlines = DecodeNewlines(families);
            return Result<SourceTextMetadata>::Success(metadata);
        }

        /** @brief File size/time observation used for bounded read admission, not native filesystem identity. */
        struct DiskObservation final {
            std::uintmax_t size;
            std::filesystem::file_time_type stamp;
        };

        /** @brief Rejects missing/oversized input before opening or allocating a content buffer. */
        Result<DiskObservation> AdmitDiskRead(const std::filesystem::path &path, const std::size_t maximumBytes) {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error)
                return Failure<DiskObservation>(error == std::errc::no_such_file_or_directory ? SourceDocumentErrors::Removed
                                                                                              : SourceDocumentErrors::ReadFailed);
            if (size > maximumBytes)
                return Failure<DiskObservation>(SourceDocumentErrors::TooLarge);
            const auto stamp = std::filesystem::last_write_time(path, error);
            if (error)
                return Failure<DiskObservation>(SourceDocumentErrors::ReadFailed);
            return Result<DiskObservation>::Success({size, stamp});
        }

        /** @brief Performs bounded chunked I/O; limit rejection precedes appending oversized input. */
        Result<std::string> ReadBytes(std::ifstream &input, const std::size_t maximumBytes, const CancellationToken cancellation) {
            std::string bytes;
            std::array<char, 4096> chunk{};
            while (input) {
                if (cancellation.IsCancellationRequested())
                    return Failure<std::string>(SourceDocumentErrors::Cancelled);
                input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                const auto count = static_cast<std::size_t>(input.gcount());
                if (count > maximumBytes - bytes.size())
                    return Failure<std::string>(SourceDocumentErrors::TooLarge);
                bytes.append(chunk.data(), count);
            }
            if (input.bad())
                return Failure<std::string>(SourceDocumentErrors::ReadFailed);
            return Result<std::string>::Success(std::move(bytes));
        }
    }  // namespace

    /** @copydoc CaptureSourceText */
    Result<std::shared_ptr<const SourceText>> CaptureSourceText(std::string bytes, const std::size_t maximumBytes,
                                                                const CancellationToken cancellation) {
        if (bytes.size() > maximumBytes)
            return Failure<std::shared_ptr<const SourceText>>(SourceDocumentErrors::TooLarge);
        if (cancellation.IsCancellationRequested())
            return Failure<std::shared_ptr<const SourceText>>(SourceDocumentErrors::Cancelled);
        // This Foundation scalar validator is one atomic pass over at most the admitted 8 MiB.
        if (!IsValidUtf8ScalarSequence(bytes))
            return Failure<std::shared_ptr<const SourceText>>(SourceDocumentErrors::Encoding);
        const auto metadata = InspectText(bytes, cancellation);
        if (metadata.HasError())
            return Result<std::shared_ptr<const SourceText>>::Failure(metadata.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Failure<std::shared_ptr<const SourceText>>(SourceDocumentErrors::Cancelled);
        return Result<std::shared_ptr<const SourceText>>::Success(std::make_shared<const SourceText>(std::move(bytes), metadata.Value()));
    }

    /** @copydoc LoadSourceText */
    Result<std::shared_ptr<const SourceText>> LoadSourceText(const std::filesystem::path &path, const std::size_t maximumBytes,
                                                             const CancellationToken cancellation) {
        try {
            if (cancellation.IsCancellationRequested())
                return Failure<std::shared_ptr<const SourceText>>(SourceDocumentErrors::Cancelled);
            const auto admission = AdmitDiskRead(path, maximumBytes);
            if (admission.HasError())
                return Result<std::shared_ptr<const SourceText>>::Failure(admission.ErrorValue());
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return Failure<std::shared_ptr<const SourceText>>(SourceDocumentErrors::ReadFailed);
            auto bytes = ReadBytes(input, maximumBytes, cancellation);
            if (bytes.HasError())
                return Result<std::shared_ptr<const SourceText>>::Failure(bytes.ErrorValue());
            std::error_code error;
            if (const auto finalStamp = std::filesystem::last_write_time(path, error);
                error || finalStamp != admission.Value().stamp || bytes.Value().size() != admission.Value().size)
                return Failure<std::shared_ptr<const SourceText>>(SourceDocumentErrors::DiskChanged);
            return CaptureSourceText(std::move(bytes).Value(), maximumBytes, cancellation);
        } catch (const std::bad_alloc &) {
            return Failure<std::shared_ptr<const SourceText>>(SourceDocumentErrors::TooLarge);
        }
    }
}  // namespace Horo::Editor::Detail
