#include "Horo/Editor/UiCanvasDocument.h"

#include "Horo/Foundation/PathUtils.h"
#include "editor/UiCanvasDocumentSerialization.h"

#include <cassert>
#include <cctype>
#include <fstream>
#include <limits>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace Horo::Editor {
    namespace {
        const ErrorDomainId UiCanvasDocumentDomain{"horo.editor.ui_canvas_document"};

        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            return Result<T>::Failure(MakeError(descriptor, std::move(message)));
        }

        [[nodiscard]] std::string LowercaseAscii(std::string value) {
            for (char &character : value)
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            return value;
        }

        [[nodiscard]] bool IsUiCanvasPath(const std::filesystem::path &path) {
            return LowercaseAscii(path.extension().string()) == UiCanvasDocumentExtension;
        }

        [[nodiscard]] std::filesystem::path CanonicalPath(const std::filesystem::path &path, bool &valid) {
            valid = false;
            if (path.empty() || !path.is_absolute())
                return {};
            std::error_code error;
            const std::filesystem::path absolute = path.lexically_normal();
            const std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
            if (error || canonical.empty())
                return {};
            valid = true;
            return canonical;
        }

        [[nodiscard]] Result<std::filesystem::path> ValidateProjectPath(const std::filesystem::path &projectRoot,
                                                                        const std::filesystem::path &documentPath) {
            bool rootValid = false;
            const std::filesystem::path root = CanonicalPath(projectRoot, rootValid);
            bool documentValid = false;
            const std::filesystem::path path = CanonicalPath(documentPath, documentValid);
            if (!rootValid || !documentValid || !IsUiCanvasPath(path) || !Horo::Foundation::Paths::HasPathPrefix(root, path))
                return Failure<std::filesystem::path>(UiCanvasDocumentErrors::InvalidPath);
            return Result<std::filesystem::path>::Success(path);
        }

        [[nodiscard]] Result<std::string> ReadContents(const std::filesystem::path &path) {
            std::error_code error;
            const std::filesystem::file_status status = std::filesystem::status(path, error);
            if (error) {
                if (error == std::errc::no_such_file_or_directory)
                    return Failure<std::string>(UiCanvasDocumentErrors::Missing);
                return Failure<std::string>(UiCanvasDocumentErrors::ReadFailed, error.message());
            }
            if (status.type() == std::filesystem::file_type::not_found)
                return Failure<std::string>(UiCanvasDocumentErrors::Missing);
            if (!std::filesystem::is_regular_file(status))
                return Failure<std::string>(UiCanvasDocumentErrors::ReadFailed, "UI Canvas path is not a regular file.");

            const std::uintmax_t size = std::filesystem::file_size(path, error);
            if (error)
                return Failure<std::string>(UiCanvasDocumentErrors::ReadFailed, error.message());
            if (size > MaximumUiCanvasDocumentBytes)
                return Failure<std::string>(UiCanvasDocumentErrors::ReadFailed, "UI Canvas source exceeds the bounded file size.");

            std::ifstream input(path, std::ios::binary);
            if (!input)
                return Failure<std::string>(UiCanvasDocumentErrors::ReadFailed, "UI Canvas source could not be opened.");
            std::string contents(static_cast<std::size_t>(size), '\0');
            if (size != 0)
                input.read(contents.data(), static_cast<std::streamsize>(size));
            if (!input && !input.eof())
                return Failure<std::string>(UiCanvasDocumentErrors::ReadFailed, "UI Canvas source could not be read completely.");
            return Result<std::string>::Success(std::move(contents));
        }

        [[nodiscard]] UiCanvasFileFingerprint Fingerprint(const std::string_view contents) {
            const auto bytes = std::as_bytes(std::span{contents.data(), contents.size()});
            return UiCanvasFileFingerprint{
                .exists = true,
                .byteSize = contents.size(),
                .checksum = FormatSha256(ComputeSha256(bytes)),
            };
        }

        [[nodiscard]] std::vector<std::byte> Bytes(const std::string_view value) {
            std::vector<std::byte> bytes;
            bytes.reserve(value.size());
            for (const char character : value)
                bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
            return bytes;
        }
    }  // namespace

    namespace UiCanvasDocumentErrors {
        const ErrorCodeDescriptor InvalidPath{UiCanvasDocumentDomain,
                                              ErrorCode{"editor.ui_canvas_document.path_invalid"},
                                              ErrorSeverity::Error,
                                              "The UI Canvas path is invalid.",
                                              "Use an absolute project-contained .uicanvas file.",
                                              false,
                                              true};
        const ErrorCodeDescriptor Missing{UiCanvasDocumentDomain,
                                          ErrorCode{"editor.ui_canvas_document.missing"},
                                          ErrorSeverity::Error,
                                          "The UI Canvas source file is missing.",
                                          "Restore the file before opening it.",
                                          true,
                                          true};
        const ErrorCodeDescriptor ReadFailed{UiCanvasDocumentDomain,
                                             ErrorCode{"editor.ui_canvas_document.read_failed"},
                                             ErrorSeverity::Error,
                                             "The UI Canvas source file could not be read.",
                                             "Check the file and project permissions.",
                                             true,
                                             true};
        const ErrorCodeDescriptor Malformed{UiCanvasDocumentDomain,
                                            ErrorCode{"editor.ui_canvas_document.malformed"},
                                            ErrorSeverity::Error,
                                            "The UI Canvas source is malformed.",
                                            "Repair the file or restore a valid authored revision.",
                                            false,
                                            true};
        const ErrorCodeDescriptor UnsupportedVersion{UiCanvasDocumentDomain,
                                                     ErrorCode{"editor.ui_canvas_document.version_unsupported"},
                                                     ErrorSeverity::Error,
                                                     "The UI Canvas source uses a newer format.",
                                                     "Open the project with an editor that supports this authored format.",
                                                     false,
                                                     true};
        const ErrorCodeDescriptor InvalidDocument{UiCanvasDocumentDomain,
                                                  ErrorCode{"editor.ui_canvas_document.invalid"},
                                                  ErrorSeverity::Error,
                                                  "The UI Canvas authored document is invalid.",
                                                  "Fix the canvas identity, mode, resolution, or dependency values.",
                                                  false,
                                                  true};
        const ErrorCodeDescriptor WriteFailed{UiCanvasDocumentDomain,
                                              ErrorCode{"editor.ui_canvas_document.write_failed"},
                                              ErrorSeverity::Error,
                                              "The UI Canvas source could not be saved.",
                                              "Check the project filesystem and try again.",
                                              true,
                                              true};
        const ErrorCodeDescriptor Conflict{UiCanvasDocumentDomain,
                                           ErrorCode{"editor.ui_canvas_document.conflict"},
                                           ErrorSeverity::Warning,
                                           "The UI Canvas source changed outside the editor.",
                                           "Reload, compare, or explicitly overwrite the external revision.",
                                           false,
                                           true};
        const ErrorCodeDescriptor DirtyDocument{UiCanvasDocumentDomain,
                                                ErrorCode{"editor.ui_canvas_document.dirty"},
                                                ErrorSeverity::Warning,
                                                "The UI Canvas has unsaved changes.",
                                                "Save or explicitly discard the changes.",
                                                false,
                                                true};
        const ErrorCodeDescriptor Closed{UiCanvasDocumentDomain,
                                         ErrorCode{"editor.ui_canvas_document.closed"},
                                         ErrorSeverity::Error,
                                         "The UI Canvas document session is closed.",
                                         "Open a new document session before editing.",
                                         false,
                                         true};
        const ErrorCodeDescriptor StateExhausted{UiCanvasDocumentDomain,
                                                 ErrorCode{"editor.ui_canvas_document.state_exhausted"},
                                                 ErrorSeverity::Critical,
                                                 "The UI Canvas state revision is exhausted.",
                                                 "Close and reopen the project before editing further.",
                                                 true,
                                                 false};
    }  // namespace UiCanvasDocumentErrors

    /** @copydoc InspectUiCanvasDocumentFingerprint */
    Result<UiCanvasFileFingerprint> InspectUiCanvasDocumentFingerprint(const std::filesystem::path &absolutePath) {
        bool valid = false;
        const std::filesystem::path path = CanonicalPath(absolutePath, valid);
        if (!valid || !IsUiCanvasPath(path))
            return Failure<UiCanvasFileFingerprint>(UiCanvasDocumentErrors::InvalidPath);
        const Result<std::string> contents = ReadContents(path);
        if (contents.HasError()) {
            if (contents.ErrorValue().code.Value() == UiCanvasDocumentErrors::Missing.code.Value())
                return Result<UiCanvasFileFingerprint>::Success({});
            return Result<UiCanvasFileFingerprint>::Failure(contents.ErrorValue());
        }
        return Result<UiCanvasFileFingerprint>::Success(Fingerprint(contents.Value()));
    }

    /** @copydoc LoadUiCanvasDocument */
    Result<UiCanvasDocumentSnapshot> LoadUiCanvasDocument(const std::filesystem::path &absolutePath) {
        bool valid = false;
        const std::filesystem::path path = CanonicalPath(absolutePath, valid);
        if (!valid || !IsUiCanvasPath(path))
            return Failure<UiCanvasDocumentSnapshot>(UiCanvasDocumentErrors::InvalidPath);
        const Result<std::string> contents = ReadContents(path);
        if (contents.HasError())
            return Result<UiCanvasDocumentSnapshot>::Failure(contents.ErrorValue());
        Result<Runtime::Ui::UiDocument> document = UiCanvasDocumentSerialization::Parse(contents.Value());
        if (document.HasError())
            return Result<UiCanvasDocumentSnapshot>::Failure(document.ErrorValue());
        return Result<UiCanvasDocumentSnapshot>::Success(
            {.document = std::move(document).Value(), .state = {1}, .fingerprint = Fingerprint(contents.Value())});
    }

    /** @copydoc SaveUiCanvasDocument */
    Result<UiCanvasDocumentSaveResult> SaveUiCanvasDocument(const std::filesystem::path &absoluteProjectRoot,
                                                            const std::filesystem::path &absolutePath,
                                                            const UiCanvasDocumentSnapshot &snapshot,
                                                            const UiCanvasFileFingerprint &expectedFingerprint,
                                                            const bool overwriteConflict, const ProjectMutationCoordinator &mutations,
                                                            DurableFileSystem &files) {
        const Result<std::filesystem::path> destination = ValidateProjectPath(absoluteProjectRoot, absolutePath);
        if (destination.HasError())
            return Result<UiCanvasDocumentSaveResult>::Failure(destination.ErrorValue());
        if (!snapshot.state.IsValid() || !snapshot.document.Id().IsValid() || !snapshot.document.Revision().IsValid() ||
            snapshot.document.Canvases().empty())
            return Failure<UiCanvasDocumentSaveResult>(UiCanvasDocumentErrors::InvalidDocument);

        const Result<ProjectMutationLease> lease = mutations.TryAcquire(ProjectMutationRequest{
            .projectRoot = absoluteProjectRoot,
            .owner = ProjectMutationOwner::Save,
            .operationId = "ui-canvas-save",
        });
        if (lease.HasError())
            return Result<UiCanvasDocumentSaveResult>::Failure(lease.ErrorValue());

        const std::string serialized = UiCanvasDocumentSerialization::Serialize(snapshot.document);
        const Result<UiCanvasFileFingerprint> current = InspectUiCanvasDocumentFingerprint(destination.Value());
        if (current.HasError())
            return Result<UiCanvasDocumentSaveResult>::Failure(current.ErrorValue());
        if (!overwriteConflict && current.Value() != expectedFingerprint)
            return Result<UiCanvasDocumentSaveResult>::Success(
                {.status = UiCanvasDocumentSaveStatus::Conflict, .fingerprint = current.Value()});

        std::filesystem::path prepared = destination.Value();
        prepared += ".save.tmp";
        const std::vector<std::byte> bytes = Bytes(serialized);
        if (const Result<void> written = files.WriteDurable(prepared, bytes); written.HasError()) {
            static_cast<void>(files.RemoveDurable(prepared));
            return Result<UiCanvasDocumentSaveResult>::Failure(written.ErrorValue());
        }
        const Result<UiCanvasFileFingerprint> rechecked = InspectUiCanvasDocumentFingerprint(destination.Value());
        if (rechecked.HasError()) {
            static_cast<void>(files.RemoveDurable(prepared));
            return Result<UiCanvasDocumentSaveResult>::Failure(rechecked.ErrorValue());
        }
        if (!overwriteConflict && rechecked.Value() != expectedFingerprint) {
            static_cast<void>(files.RemoveDurable(prepared));
            return Result<UiCanvasDocumentSaveResult>::Success(
                {.status = UiCanvasDocumentSaveStatus::Conflict, .fingerprint = rechecked.Value()});
        }
        if (const Result<void> replaced = files.AtomicReplace(prepared, destination.Value()); replaced.HasError()) {
            static_cast<void>(files.RemoveDurable(prepared));
            return Result<UiCanvasDocumentSaveResult>::Failure(replaced.ErrorValue());
        }
        return Result<UiCanvasDocumentSaveResult>::Success(
            {.status = UiCanvasDocumentSaveStatus::Saved, .fingerprint = Fingerprint(serialized)});
    }

    /** @copydoc UiCanvasDocument::Open */
    Result<UiCanvasDocument> UiCanvasDocument::Open(const DocumentIdentity identity, const std::filesystem::path &absolutePath) {
        if (!identity.IsValid() || identity.key.kind != DocumentKind::UiCanvas)
            return Failure<UiCanvasDocument>(UiCanvasDocumentErrors::InvalidDocument, "The UI Canvas session identity is invalid.");
        Result<UiCanvasDocumentSnapshot> loaded = LoadUiCanvasDocument(absolutePath);
        if (loaded.HasError())
            return Result<UiCanvasDocument>::Failure(loaded.ErrorValue());
        bool valid = false;
        const std::filesystem::path path = CanonicalPath(absolutePath, valid);
        if (!valid)
            return Failure<UiCanvasDocument>(UiCanvasDocumentErrors::InvalidPath);
        UiCanvasDocumentSnapshot snapshot = std::move(loaded).Value();
        return Result<UiCanvasDocument>::Success(
            UiCanvasDocument{identity, path, std::move(snapshot.document), std::move(snapshot.fingerprint)});
    }

    UiCanvasDocument::UiCanvasDocument(DocumentIdentity identity, std::filesystem::path path, Runtime::Ui::UiDocument document,
                                       UiCanvasFileFingerprint fingerprint) noexcept
        : identity_(std::move(identity)), path_(std::move(path)), document_(std::move(document)), fingerprint_(std::move(fingerprint)) {}

    /** @copydoc UiCanvasDocument::Document */
    const Runtime::Ui::UiDocument &UiCanvasDocument::Document() const noexcept {
        assert(document_.has_value());
        return *document_;
    }

    /** @copydoc UiCanvasDocument::AdvanceState */
    Result<UiCanvasDocumentStateId> UiCanvasDocument::AdvanceState() const {
        if (currentState_.value == std::numeric_limits<std::uint64_t>::max())
            return Failure<UiCanvasDocumentStateId>(UiCanvasDocumentErrors::StateExhausted);
        return Result<UiCanvasDocumentStateId>::Success({currentState_.value + 1U});
    }

    /** @copydoc UiCanvasDocument::Replace */
    Result<void> UiCanvasDocument::Replace(Runtime::Ui::UiDocument document) {
        if (closed_)
            return Failure(UiCanvasDocumentErrors::Closed);
        if (!document.Id().IsValid() || !document.Revision().IsValid() || document.Canvases().empty())
            return Failure(UiCanvasDocumentErrors::InvalidDocument);
        if (document.Id() != Document().Id())
            return Failure(UiCanvasDocumentErrors::InvalidDocument);
        if (document.Revision() <= Document().Revision())
            return Failure(Runtime::Ui::UiErrors::RevisionStale);
        const Result<UiCanvasDocumentStateId> nextState = AdvanceState();
        if (nextState.HasError())
            return Result<void>::Failure(nextState.ErrorValue());
        document_ = std::move(document);
        currentState_ = nextState.Value();
        return Result<void>::Success();
    }

    /** @copydoc UiCanvasDocument::Save */
    Result<UiCanvasDocumentSaveResult> UiCanvasDocument::Save(const std::filesystem::path &absoluteProjectRoot,
                                                              const ProjectMutationCoordinator &mutations, DurableFileSystem &files,
                                                              const bool overwriteConflict) {
        if (closed_)
            return Failure<UiCanvasDocumentSaveResult>(UiCanvasDocumentErrors::Closed);
        const Result<UiCanvasDocumentSaveResult> saved =
            SaveUiCanvasDocument(absoluteProjectRoot, path_, UiCanvasDocumentSnapshot{.document = Document(), .state = currentState_},
                                 fingerprint_, overwriteConflict, mutations, files);
        if (saved.HasError() || saved.Value().status != UiCanvasDocumentSaveStatus::Saved)
            return saved;
        fingerprint_ = saved.Value().fingerprint;
        savedState_ = currentState_;
        return saved;
    }

    /** @copydoc UiCanvasDocument::Reload */
    Result<void> UiCanvasDocument::Reload(const UiCanvasReloadPolicy policy) {
        if (closed_)
            return Failure(UiCanvasDocumentErrors::Closed);
        if (IsDirty() && policy == UiCanvasReloadPolicy::RequireClean)
            return Failure(UiCanvasDocumentErrors::DirtyDocument);
        Result<UiCanvasDocumentSnapshot> loaded = LoadUiCanvasDocument(path_);
        if (loaded.HasError())
            return Result<void>::Failure(loaded.ErrorValue());
        if (loaded.Value().document.Id() != Document().Id())
            return Failure(UiCanvasDocumentErrors::InvalidDocument);
        const Result<UiCanvasDocumentStateId> nextState = AdvanceState();
        if (nextState.HasError())
            return Result<void>::Failure(nextState.ErrorValue());
        UiCanvasDocumentSnapshot snapshot = std::move(loaded).Value();
        document_ = std::move(snapshot.document);
        currentState_ = nextState.Value();
        savedState_ = currentState_;
        fingerprint_ = std::move(snapshot.fingerprint);
        return Result<void>::Success();
    }

    /** @copydoc UiCanvasDocument::Close */
    Result<void> UiCanvasDocument::Close(const UiCanvasClosePolicy policy) {
        if (closed_)
            return Failure(UiCanvasDocumentErrors::Closed);
        if (IsDirty() && policy == UiCanvasClosePolicy::RequireClean)
            return Failure(UiCanvasDocumentErrors::DirtyDocument);
        document_.reset();
        closed_ = true;
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
