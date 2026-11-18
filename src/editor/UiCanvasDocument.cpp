#include "Horo/Editor/UiCanvasDocument.h"

#include "Horo/Foundation/PathUtils.h"
#include "editor/UiCanvasDocumentSerialization.h"

#include <cassert>
#include <cctype>
#include <chrono>
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
            if (input.gcount() != static_cast<std::streamsize>(size) || input.bad() || input.peek() != std::char_traits<char>::eof())
                return Failure<std::string>(UiCanvasDocumentErrors::ReadFailed, "UI Canvas source changed during the bounded read.");
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

        /** @brief Bridges the UI session's byte fingerprint to the shared tracked source identity. */
        Result<Assets::AssetSourceIdentity> SourceIdentity(const UiCanvasFileFingerprint &fingerprint) {
            if (!fingerprint.exists)
                return Result<Assets::AssetSourceIdentity>::Success({});
            if (fingerprint.byteSize > MaximumUiCanvasDocumentBytes)
                return Result<Assets::AssetSourceIdentity>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
            auto digest = ParseSha256(fingerprint.checksum);
            if (digest.HasError())
                return Result<Assets::AssetSourceIdentity>::Failure(digest.ErrorValue());
            return Result<Assets::AssetSourceIdentity>::Success({true, static_cast<std::size_t>(fingerprint.byteSize), digest.Value()});
        }

        /** @brief Projects the shared durable source receipt back into the existing UI session API. */
        UiCanvasFileFingerprint Fingerprint(const Assets::AssetSourceIdentity &identity) {
            return identity.exists ? UiCanvasFileFingerprint{true, identity.bytes, FormatSha256(identity.digest)}
                                   : UiCanvasFileFingerprint{};
        }

        /** @brief Rejects reentrant commands while a borrowed filesystem may call back into the editor. */
        class PersistenceGuard final {
        public:
            explicit PersistenceGuard(bool &active) noexcept : active_(active) {
                active_ = true;
            }

            PersistenceGuard(const PersistenceGuard &) = delete;
            PersistenceGuard &operator=(const PersistenceGuard &) = delete;

            ~PersistenceGuard() {
                active_ = false;
            }

        private:
            bool &active_;
        };

        /** @brief Validates and serializes a detached snapshot before admitting any filesystem mutation. */
        Result<std::string> SerializeSnapshot(const UiCanvasDocumentSnapshot &snapshot) {
            if (!snapshot.state.IsValid() || !snapshot.document.Id().IsValid() || !snapshot.document.Revision().IsValid() ||
                snapshot.document.Canvases().empty())
                return Failure<std::string>(UiCanvasDocumentErrors::InvalidDocument);
            return UiCanvasDocumentSerialization::Serialize(snapshot.document);
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
        const auto encoded = SerializeSnapshot(snapshot);
        if (encoded.HasError())
            return Result<UiCanvasDocumentSaveResult>::Failure(encoded.ErrorValue());
        const std::string &serialized = encoded.Value();

        const Result<ProjectMutationLease> lease = mutations.TryAcquire(ProjectMutationRequest{
            .projectRoot = absoluteProjectRoot,
            .owner = ProjectMutationOwner::Save,
            .operationId = "ui-canvas-save",
        });
        if (lease.HasError())
            return Result<UiCanvasDocumentSaveResult>::Failure(lease.ErrorValue());

        auto lockPath = Assets::AssetSourcePublicationLockPath(absoluteProjectRoot, destination.Value());
        if (lockPath.HasError())
            return Result<UiCanvasDocumentSaveResult>::Failure(lockPath.ErrorValue());
        auto nativeLease = files.TryAcquireExclusive(lockPath.Value(), "ui-canvas-save");
        if (nativeLease.HasError())
            return Result<UiCanvasDocumentSaveResult>::Failure(nativeLease.ErrorValue());

        auto expected = SourceIdentity(expectedFingerprint);
        if (overwriteConflict) {
            const auto latest = Assets::CaptureAssetSource(absoluteProjectRoot, destination.Value(), MaximumUiCanvasDocumentBytes);
            if (latest.HasError())
                return Result<UiCanvasDocumentSaveResult>::Failure(latest.ErrorValue());
            expected = Result<Assets::AssetSourceIdentity>::Success(latest.Value().identity);
        }
        if (expected.HasError())
            return Result<UiCanvasDocumentSaveResult>::Failure(expected.ErrorValue());
        const auto published = Assets::PublishAssetSource(absoluteProjectRoot, destination.Value(), expected.Value(),
                                                          std::as_bytes(std::span{serialized}), nativeLease.Value(), files);
        if (published.HasError()) {
            if (published.ErrorValue().code.Value() != Assets::AssetSourceErrors::Conflict.code.Value())
                return Result<UiCanvasDocumentSaveResult>::Failure(published.ErrorValue());
            const auto latest = InspectUiCanvasDocumentFingerprint(destination.Value());
            if (latest.HasError())
                return Result<UiCanvasDocumentSaveResult>::Failure(latest.ErrorValue());
            return Result<UiCanvasDocumentSaveResult>::Success(
                {.status = UiCanvasDocumentSaveStatus::Conflict, .fingerprint = latest.Value()});
        }
        if (published.Value().status != Assets::AssetSourcePublicationStatus::Durable)
            return Result<UiCanvasDocumentSaveResult>::Failure(
                published.Value().durabilityError.value_or(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed)));
        return Result<UiCanvasDocumentSaveResult>::Success(
            {.status = UiCanvasDocumentSaveStatus::Saved, .fingerprint = Fingerprint(published.Value().identity)});
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
        : identity_(std::move(identity)), path_(std::move(path)), document_(std::move(document)), fingerprint_(std::move(fingerprint)) {
        currentState_.instance = identity_.instance;
        savedState_.instance = identity_.instance;
        contentState_.instance = identity_.instance;
        savedContentState_.instance = identity_.instance;
    }

    /** @copydoc UiCanvasDocument::UiCanvasDocument */
    UiCanvasDocument::UiCanvasDocument(UiCanvasDocument &&other) noexcept
        : history_(std::move(other.history_)), historyCursor_(other.historyCursor_), selection_(other.selection_),
          identity_(std::move(other.identity_)), path_(std::move(other.path_)), document_(std::move(other.document_)),
          fingerprint_(std::move(other.fingerprint_)), currentState_(other.currentState_), savedState_(other.savedState_),
          contentState_(other.contentState_), savedContentState_(other.savedContentState_),
          acceptedRecovery_(std::move(other.acceptedRecovery_)), recoveredState_(other.recoveredState_), closed_(other.closed_) {
        other.document_.reset();
        other.history_.clear();
        other.historyCursor_ = 0;
        other.selection_ = {};
        other.closed_ = true;
    }

    /** @copydoc UiCanvasDocument::operator= */
    UiCanvasDocument &UiCanvasDocument::operator=(UiCanvasDocument &&other) noexcept {
        if (this == &other)
            return *this;
        history_ = std::move(other.history_);
        historyCursor_ = other.historyCursor_;
        selection_ = other.selection_;
        identity_ = std::move(other.identity_);
        path_ = std::move(other.path_);
        document_ = std::move(other.document_);
        fingerprint_ = std::move(other.fingerprint_);
        currentState_ = other.currentState_;
        savedState_ = other.savedState_;
        contentState_ = other.contentState_;
        savedContentState_ = other.savedContentState_;
        acceptedRecovery_ = std::move(other.acceptedRecovery_);
        recoveredState_ = other.recoveredState_;
        closed_ = other.closed_;
        other.document_.reset();
        other.history_.clear();
        other.historyCursor_ = 0;
        other.selection_ = {};
        other.closed_ = true;
        return *this;
    }

    /** @copydoc UiCanvasDocument::Document */
    const Runtime::Ui::UiDocument &UiCanvasDocument::Document() const noexcept {
        assert(document_.has_value());
        return *document_;
    }

    /** @copydoc UiCanvasDocument::AdvanceState */
    Result<UiCanvasDocumentStateId> UiCanvasDocument::AdvanceState() const {
        if (currentState_.value == std::numeric_limits<std::uint64_t>::max())
            return Failure<UiCanvasDocumentStateId>(UiCanvasDocumentErrors::StateExhausted);
        return Result<UiCanvasDocumentStateId>::Success({currentState_.value + 1U, identity_.instance});
    }

    /** @copydoc UiCanvasDocument::Replace */
    Result<void> UiCanvasDocument::Replace(Runtime::Ui::UiDocument document) {
        if (closed_ || persisting_)
            return Failure(closed_ ? UiCanvasDocumentErrors::Closed : UiCanvasDocumentErrors::Conflict);
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
        contentState_ = currentState_;
        history_.clear();
        historyCursor_ = 0;
        selection_ = {};
        return Result<void>::Success();
    }

    /** @copydoc UiCanvasDocument::Save */
    Result<UiCanvasDocumentSaveResult> UiCanvasDocument::Save(const std::filesystem::path &absoluteProjectRoot,
                                                              const ProjectMutationCoordinator &mutations, DurableFileSystem &files,
                                                              const bool overwriteConflict) {
        if (closed_ || persisting_)
            return Failure<UiCanvasDocumentSaveResult>(closed_ ? UiCanvasDocumentErrors::Closed : UiCanvasDocumentErrors::Conflict);
        const PersistenceGuard guard(persisting_);
        const Result<UiCanvasDocumentSaveResult> saved =
            SaveUiCanvasDocument(absoluteProjectRoot, path_, UiCanvasDocumentSnapshot{.document = Document(), .state = currentState_},
                                 fingerprint_, overwriteConflict, mutations, files);
        if (saved.HasError() || saved.Value().status != UiCanvasDocumentSaveStatus::Saved)
            return saved;
        fingerprint_ = saved.Value().fingerprint;
        savedState_ = currentState_;
        savedContentState_ = contentState_;
        return saved;
    }

    /** @copydoc UiCanvasDocument::InspectRecovery */
    Result<std::optional<DocumentRecoveryRecord>> UiCanvasDocument::InspectRecovery(const DocumentRecoveryService &recovery) const {
        if (closed_)
            return Failure<std::optional<DocumentRecoveryRecord>>(UiCanvasDocumentErrors::Closed);
        return recovery.Inspect(Assets::AssetId::FromBytes(Document().Id().Bytes()), identity_.key);
    }

    /** @copydoc UiCanvasDocument::CaptureRecovery */
    Result<std::optional<UiCanvasRecoveryCheckpoint>> UiCanvasDocument::CaptureRecovery(const Assets::AssetId sessionOwner) const {
        using Return = Result<std::optional<UiCanvasRecoveryCheckpoint>>;
        if (closed_ || persisting_)
            return Return::Failure(MakeError(closed_ ? UiCanvasDocumentErrors::Closed : UiCanvasDocumentErrors::Conflict));
        if (!IsDirty() || recoveredState_ == currentState_)
            return Return::Success({});
        if (!sessionOwner.IsValid())
            return Return::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        auto encoded = UiCanvasDocumentSerialization::Serialize(Document());
        if (encoded.HasError())
            return Return::Failure(encoded.ErrorValue());
        if (encoded.Value().size() > DocumentRecoveryLimits{}.maximumRecordBytes)
            return Return::Failure(MakeError(Assets::AssetSourceErrors::Limit));
        auto accepted = SourceIdentity(fingerprint_);
        if (accepted.HasError())
            return Return::Failure(accepted.ErrorValue());
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto bytes = std::as_bytes(std::span{encoded.Value()});
        return Return::Success(UiCanvasRecoveryCheckpoint{currentState_,
                                                          {Assets::AssetId::FromBytes(Document().Id().Bytes()),
                                                           identity_.key,
                                                           sessionOwner,
                                                           identity_.instance.Value(),
                                                           currentState_.value,
                                                           static_cast<std::uint64_t>(now),
                                                           accepted.Value(),
                                                           ComputeSha256(bytes),
                                                           {bytes.begin(), bytes.end()}},
                                                          acceptedRecovery_});
    }

    /** @copydoc WriteUiCanvasRecovery */
    Result<void> WriteUiCanvasRecovery(const UiCanvasRecoveryCheckpoint &checkpoint, DocumentRecoveryService &recovery,
                                       const CancellationToken cancellation) {
        return checkpoint.accepted ? recovery.ReplaceAccepted(checkpoint.record, *checkpoint.accepted, cancellation)
                                   : recovery.Write(checkpoint.record, cancellation);
    }

    /** @copydoc UiCanvasDocument::AcknowledgeRecovery */
    void UiCanvasDocument::AcknowledgeRecovery(const UiCanvasRecoveryCheckpoint &checkpoint) noexcept {
        if (closed_ || checkpoint.state.instance != identity_.instance || checkpoint.state.value > currentState_.value ||
            checkpoint.state.value < recoveredState_.value || checkpoint.accepted != acceptedRecovery_ ||
            checkpoint.record.document != identity_.key || checkpoint.record.asset != Assets::AssetId::FromBytes(Document().Id().Bytes()))
            return;
        acceptedRecovery_.reset();
        recoveredState_ = checkpoint.state;
    }

    /** @copydoc UiCanvasDocument::Autosave */
    Result<void> UiCanvasDocument::Autosave(const Assets::AssetId sessionOwner, DocumentRecoveryService &recovery) {
        if (closed_ || persisting_)
            return Failure(closed_ ? UiCanvasDocumentErrors::Closed : UiCanvasDocumentErrors::Conflict);
        if (IsDirty() && recoveredState_ == currentState_) {
            // Explicit shutdown cannot rely on an old receipt after another workspace removed its record.
            const auto current = InspectRecovery(recovery);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            if (current.Value()) {
                const auto &record = *current.Value();
                if ((acceptedRecovery_ && *acceptedRecovery_ == record) ||
                    (record.sessionOwner == sessionOwner && record.sessionGeneration == identity_.instance.Value() &&
                     record.revision == currentState_.value))
                    return Result<void>::Success();
            } else {
                acceptedRecovery_.reset();
            }
            recoveredState_ = {};
        }
        const auto checkpoint = CaptureRecovery(sessionOwner);
        if (checkpoint.HasError())
            return Result<void>::Failure(checkpoint.ErrorValue());
        if (!checkpoint.Value())
            return !IsDirty() && HasRecoveryCheckpoint() ? DiscardRecovery(sessionOwner, recovery) : Result<void>::Success();
        const PersistenceGuard guard(persisting_);
        auto written = WriteUiCanvasRecovery(*checkpoint.Value(), recovery);
        if (written.HasValue())
            AcknowledgeRecovery(*checkpoint.Value());
        return written;
    }

    /** @copydoc UiCanvasDocument::Recover */
    Result<void> UiCanvasDocument::Recover(const UiCanvasDocumentStateId expected, const DocumentRecoveryRecord &record,
                                           const DocumentRecoveryService &recovery) {
        if (closed_ || persisting_)
            return Failure(closed_ ? UiCanvasDocumentErrors::Closed : UiCanvasDocumentErrors::Conflict);
        if (expected != currentState_ || IsDirty())
            return Failure(UiCanvasDocumentErrors::Conflict);
        const auto latest = InspectRecovery(recovery);
        if (latest.HasError())
            return Result<void>::Failure(latest.ErrorValue());
        if (!latest.Value() || *latest.Value() != record || ComputeSha256(record.payload) != record.state)
            return Failure(UiCanvasDocumentErrors::Conflict);
        const std::string_view text{reinterpret_cast<const char *>(record.payload.data()), record.payload.size()};
        auto document = UiCanvasDocumentSerialization::Parse(text);
        if (document.HasError())
            return Result<void>::Failure(document.ErrorValue());
        const auto disk = InspectUiCanvasDocumentFingerprint(path_);
        if (disk.HasError())
            return Result<void>::Failure(disk.ErrorValue());
        if (disk.Value() != fingerprint_ || Fingerprint(record.acceptedSource) != fingerprint_)
            return Failure(UiCanvasDocumentErrors::Conflict);
        auto accepted = std::make_shared<const DocumentRecoveryRecord>(record);
        if (auto restored = Replace(std::move(document).Value()); restored.HasError())
            return restored;
        acceptedRecovery_.swap(accepted);
        recoveredState_ = currentState_;
        return Result<void>::Success();
    }

    /** @copydoc UiCanvasDocument::DiscardRecovery */
    Result<void> UiCanvasDocument::DiscardRecovery(const Assets::AssetId sessionOwner, DocumentRecoveryService &recovery,
                                                   const DocumentRecoveryRecord *explicitRecord) {
        if (persisting_)
            return Failure(UiCanvasDocumentErrors::Conflict);
        const PersistenceGuard guard(persisting_);
        auto latest = InspectRecovery(recovery);
        if (latest.HasError())
            return Result<void>::Failure(latest.ErrorValue());
        if (explicitRecord && (!latest.Value() || *explicitRecord != *latest.Value()))
            return Failure(UiCanvasDocumentErrors::Conflict);
        if (!latest.Value())
            return Result<void>::Success();
        const auto &record = *latest.Value();
        const bool owned = record.sessionOwner == sessionOwner && record.sessionGeneration == identity_.instance.Value();
        if (!explicitRecord && !owned && (!acceptedRecovery_ || *acceptedRecovery_ != record))
            return Result<void>::Success();
        auto result = recovery.Discard(record);
        if (result.HasValue()) {
            acceptedRecovery_.reset();
            recoveredState_ = {};
        }
        return result;
    }

    /** @copydoc UiCanvasDocument::Reload */
    Result<void> UiCanvasDocument::Reload(const UiCanvasReloadPolicy policy) {
        if (closed_ || persisting_)
            return Failure(closed_ ? UiCanvasDocumentErrors::Closed : UiCanvasDocumentErrors::Conflict);
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
        contentState_ = currentState_;
        savedContentState_ = contentState_;
        history_.clear();
        historyCursor_ = 0;
        selection_ = {};
        fingerprint_ = std::move(snapshot.fingerprint);
        return Result<void>::Success();
    }

    /** @copydoc UiCanvasDocument::Close */
    Result<void> UiCanvasDocument::Close(const UiCanvasClosePolicy policy) {
        if (closed_ || persisting_)
            return Failure(closed_ ? UiCanvasDocumentErrors::Closed : UiCanvasDocumentErrors::Conflict);
        if (IsDirty() && policy == UiCanvasClosePolicy::RequireClean)
            return Failure(UiCanvasDocumentErrors::DirtyDocument);
        document_.reset();
        history_.clear();
        historyCursor_ = 0;
        selection_ = {};
        closed_ = true;
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
