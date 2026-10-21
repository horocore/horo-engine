#include "Horo/Editor/SourceFileOpenService.h"

#include "Horo/Foundation/PathUtils.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <ranges>
#include <system_error>
#include <type_traits>
#include <utility>

namespace Horo::Editor {
    static_assert(std::is_nothrow_move_assignable_v<DocumentIdentityRegistry>);

    namespace {
        const ErrorDomainId SourceOpenDomain{"horo.editor.source_open"};

        /** @brief Encodes native source paths at the existing UTF-8 identity, classification and error-text boundaries. */
        [[nodiscard]] std::string Utf8PathText(const std::filesystem::path &path) {
            const auto encoded = path.generic_u8string();
            return {encoded.begin(), encoded.end()};
        }

        /** @brief Rejects malformed or NUL-terminated aliases before native file access or document admission. */
        [[nodiscard]] bool HasValidPathText(const std::filesystem::path &path) {
            const auto text = Utf8PathText(path);
            return IsValidUtf8ScalarSequence(text) && text.find('\0') == std::string::npos;
        }

        /** @brief Converts a native relative path to the existing UTF-8 identity before any owner publication. */
        [[nodiscard]] Result<SourceDocumentId> SourceIdentity(const std::filesystem::path &path) {
            try {
                const auto text = Utf8PathText(path);
                if (!IsValidUtf8ScalarSequence(text) || text.find('\0') != std::string::npos)
                    return Result<SourceDocumentId>::Failure(MakeError(SourceOpenErrors::Unsafe));
                return SourceDocumentId::Parse(text);
            } catch (const std::system_error &) {
                return Result<SourceDocumentId>::Failure(MakeError(SourceOpenErrors::Unsafe));
            }
        }

        /** @brief Keeps malformed native-path translation inside destination admission, before save callbacks or history. */
        [[nodiscard]] Result<void> AdmitSaveDestination(const SourceFileOpenService &service, const std::filesystem::path &path) {
            try {
                if (!HasValidPathText(path))
                    return Result<void>::Failure(MakeError(SourceOpenErrors::Unsafe));
                const auto classification = service.Classify(path);
                using enum SourceFileKind;
                if (classification.kind != NativeSource && classification.kind != HoroScript && classification.kind != ProjectText)
                    return Result<void>::Failure(MakeError(SourceOpenErrors::Unsupported));
                return Result<void>::Success();
            } catch (const std::system_error &) {
                return Result<void>::Failure(MakeError(SourceOpenErrors::Unsafe));
            }
        }

        [[nodiscard]] std::string LowercaseAscii(std::string value) {
            for (char &character : value)
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            return value;
        }

        [[nodiscard]] std::string NormalizeExtension(std::string value) {
            value = LowercaseAscii(std::move(value));
            if (!value.empty() && value.front() != '.')
                value.insert(value.begin(), '.');
            return value;
        }

        [[nodiscard]] std::vector<std::string> NormalizeValues(const std::vector<std::string> &values, const bool extensions) {
            std::vector<std::string> normalized;
            normalized.reserve(values.size());
            for (const std::string &value : values) {
                std::string candidate = LowercaseAscii(value);
                if (extensions && !candidate.empty() && candidate.front() != '.')
                    candidate.insert(candidate.begin(), '.');
                if (!candidate.empty() && std::ranges::find(normalized, candidate) == normalized.end())
                    normalized.push_back(std::move(candidate));
            }
            return normalized;
        }

        [[nodiscard]] bool Contains(const std::vector<std::string> &values, const std::string_view value) noexcept {
            return std::ranges::find(values, value) != values.end();
        }

        [[nodiscard]] bool IsNoSuchFile(const std::error_code &error) noexcept {
            return error == std::errc::no_such_file_or_directory;
        }

        [[nodiscard]] std::filesystem::path ResolveRoot(const std::filesystem::path &input, bool &valid) {
            valid = false;
            if (input.empty())
                return {};

            std::error_code error;
            std::filesystem::path absolute = std::filesystem::absolute(input, error);
            if (error || absolute.empty())
                return {};
            absolute = absolute.lexically_normal();

            error.clear();
            const std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
            if (error || canonical.empty())
                return {};

            error.clear();
            if (const std::filesystem::file_status status = std::filesystem::status(canonical, error);
                error || !std::filesystem::is_directory(status))
                return {};

            valid = true;
            return canonical;
        }

        [[nodiscard]] Error MakePathError(const ErrorCodeDescriptor &descriptor, const std::filesystem::path &path) {
            return MakeError(descriptor, "Source path '" + Utf8PathText(path) + "' cannot be opened safely.");
        }

        [[nodiscard]] Result<std::filesystem::path> ResolveCanonicalProjectPath(const std::filesystem::path &projectRoot,
                                                                                const std::filesystem::path &lexicalPath,
                                                                                const bool allowSymlinkedFiles,
                                                                                const bool allowMissing = false) {
            if (!Horo::Foundation::Paths::HasPathPrefix(projectRoot, lexicalPath))
                return Result<std::filesystem::path>::Failure(MakePathError(SourceOpenErrors::Unsafe, lexicalPath));

            std::error_code error;
            const std::filesystem::file_status lexicalStatus = std::filesystem::symlink_status(lexicalPath, error);
            if (error && !(allowMissing && IsNoSuchFile(error)))
                return Result<std::filesystem::path>::Failure(
                    MakePathError(IsNoSuchFile(error) ? SourceOpenErrors::Missing : SourceOpenErrors::Unsafe, lexicalPath));
            if (std::filesystem::is_symlink(lexicalStatus) && !allowSymlinkedFiles)
                return Result<std::filesystem::path>::Failure(MakePathError(SourceOpenErrors::Unsafe, lexicalPath));

            error.clear();
            const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(lexicalPath, error);
            if (error || canonicalPath.empty() || !Horo::Foundation::Paths::HasPathPrefix(projectRoot, canonicalPath))
                return Result<std::filesystem::path>::Failure(MakePathError(SourceOpenErrors::Unsafe, lexicalPath));

            return Result<std::filesystem::path>::Success(canonicalPath);
        }

        [[nodiscard]] SourceOpenResult MakeExternalFallbackResult(const SourceOpenRequest &request,
                                                                  const SourceFileClassification &classification,
                                                                  const SourceOpenLocation &location) {
            return SourceOpenResult{
                .origin = request.origin,
                .route = SourceOpenRoute::ExternalEditorFallback,
                .classification = classification,
                .location = location,
                .document = std::nullopt,
                .line = request.line,
                .column = request.column,
            };
        }
    }  // namespace

    namespace SourceOpenErrors {
        const ErrorCodeDescriptor InvalidRequest{
            .domain = SourceOpenDomain,
            .code = ErrorCode{"editor.source_open.request_invalid"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "The source-open request is invalid.",
            .remediationHint = "Provide a non-empty absolute or project-relative source path.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor Missing{
            .domain = SourceOpenDomain,
            .code = ErrorCode{"editor.source_open.missing"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "The requested source file is missing.",
            .remediationHint = "Restore the file or refresh the project before opening it.",
            .retryable = true,
            .userActionable = true,
        };
        const ErrorCodeDescriptor Unsafe{
            .domain = SourceOpenDomain,
            .code = ErrorCode{"editor.source_open.unsafe"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "The requested source path is outside the project safety boundary.",
            .remediationHint = "Choose a project-contained file without an escaping symlink or traversal.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor Unsupported{
            .domain = SourceOpenDomain,
            .code = ErrorCode{"editor.source_open.unsupported"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "The requested file type is not supported by the selected editor route.",
            .remediationHint = "Use the explicit external fallback or add the extension to the project source policy.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor EditorUnavailable{
            .domain = SourceOpenDomain,
            .code = ErrorCode{"editor.source_open.editor_unavailable"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "No editor route is available for the requested source file.",
            .remediationHint = "Enable the embedded editor or configure an external editor fallback.",
            .retryable = true,
            .userActionable = true,
        };
    }  // namespace SourceOpenErrors

    SourceFilePolicy SourceFilePolicy::Default() {
        return SourceFilePolicy{
            .nativeSourceExtensions = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".tpp"},
            .horoScriptExtensions = {".horo_script", ".lua"},
            .projectTextExtensions = {".cfg", ".cmake", ".glsl", ".hlsl", ".ini", ".json", ".md", ".metal", ".shader", ".toml", ".txt",
                                      ".vert", ".frag", ".xml", ".yaml", ".yml"},
            .projectTextFileNames = {".gitignore", "LICENSE"},
            .allowSymlinkedFiles = true,
            .embeddedEditorAvailable = true,
            .externalEditorAvailable = true,
            .uiCanvasExtensions = {".uicanvas"},
            .pcgGraphExtensions = {".hpcg"},
            .sequenceExtensions = {".hsequence"},
            .prefabExtensions = {".prefab"},
        };
    }

    SourceFileOpenService::SourceFileOpenService(const std::filesystem::path &projectRoot, SourceFilePolicy policy)
        : SourceFileOpenService(projectRoot, nullptr, std::move(policy)) {}

    SourceFileOpenService::SourceFileOpenService(const std::filesystem::path &projectRoot, DocumentIdentityRegistry &documentRegistry,
                                                 SourceFilePolicy policy)
        : SourceFileOpenService(projectRoot, &documentRegistry, std::move(policy)) {}

    SourceFileOpenService::SourceFileOpenService(const std::filesystem::path &projectRoot, DocumentIdentityRegistry *documentRegistry,
                                                 SourceFilePolicy policy)
        : projectRootValid_(false), projectRoot_(ResolveRoot(projectRoot, projectRootValid_)), policy_(std::move(policy)),
          documentRegistry_(documentRegistry != nullptr ? documentRegistry : &ownedDocumentRegistry_), sourceDocuments_(projectRoot_) {
        policy_.nativeSourceExtensions = NormalizeValues(policy_.nativeSourceExtensions, true);
        policy_.horoScriptExtensions = NormalizeValues(policy_.horoScriptExtensions, true);
        policy_.projectTextExtensions = NormalizeValues(policy_.projectTextExtensions, true);
        policy_.projectTextFileNames = NormalizeValues(policy_.projectTextFileNames, false);
        policy_.pcgGraphExtensions = NormalizeValues(policy_.pcgGraphExtensions, true);
        policy_.sequenceExtensions = NormalizeValues(policy_.sequenceExtensions, true);
        policy_.uiCanvasExtensions = NormalizeValues(policy_.uiCanvasExtensions, true);
        policy_.prefabExtensions = NormalizeValues(policy_.prefabExtensions, true);
    }

    SourceFileOpenService::SourceFileOpenService(SourceFileOpenService &&other) noexcept
        : projectRootValid_(other.projectRootValid_), projectRoot_(std::move(other.projectRoot_)), policy_(std::move(other.policy_)),
          ownedDocumentRegistry_(std::move(other.ownedDocumentRegistry_)),
          documentRegistry_(other.documentRegistry_ == &other.ownedDocumentRegistry_ ? &ownedDocumentRegistry_ : other.documentRegistry_),
          sourceDocuments_(std::move(other.sourceDocuments_)) {
        other.projectRootValid_ = false;
    }

    SourceFileOpenService &SourceFileOpenService::operator=(SourceFileOpenService &&other) noexcept {
        if (this == &other)
            return *this;
        const bool otherUsesOwnedRegistry = other.documentRegistry_ == &other.ownedDocumentRegistry_;
        projectRootValid_ = other.projectRootValid_;
        projectRoot_ = std::move(other.projectRoot_);
        policy_ = std::move(other.policy_);
        ownedDocumentRegistry_ = std::move(other.ownedDocumentRegistry_);
        documentRegistry_ = otherUsesOwnedRegistry ? &ownedDocumentRegistry_ : other.documentRegistry_;
        sourceDocuments_ = std::move(other.sourceDocuments_);
        other.projectRootValid_ = false;
        return *this;
    }

    /** @copydoc SourceFileOpenService::Classify */
    SourceFileClassification SourceFileOpenService::Classify(const std::filesystem::path &path) const {
        using enum SourceFileKind;
        const std::string extension = NormalizeExtension(Utf8PathText(path.extension()));
        const std::string fileName = LowercaseAscii(Utf8PathText(path.filename()));
        if (Contains(policy_.nativeSourceExtensions, extension))
            return SourceFileClassification{NativeSource, extension};
        if (Contains(policy_.horoScriptExtensions, extension))
            return SourceFileClassification{HoroScript, extension};
        if (Contains(policy_.prefabExtensions, extension))
            return SourceFileClassification{Prefab, extension};
        if (Contains(policy_.pcgGraphExtensions, extension))
            return SourceFileClassification{PCGGraph, extension};
        if (Contains(policy_.sequenceExtensions, extension))
            return SourceFileClassification{Sequence, extension};
        if (Contains(policy_.uiCanvasExtensions, extension))
            return SourceFileClassification{UiCanvas, extension};
        if (Contains(policy_.projectTextExtensions, extension) || Contains(policy_.projectTextFileNames, fileName))
            return SourceFileClassification{ProjectText, extension};
        return SourceFileClassification{Unsupported, extension};
    }

    /** @copydoc SourceFileOpenService::NormalizeInputPath */
    std::filesystem::path SourceFileOpenService::NormalizeInputPath(const std::filesystem::path &path) const {
        if (path.is_absolute())
            return path.lexically_normal();

#ifdef _WIN32
        return (projectRoot_ / path).lexically_normal();
#else
        std::string portable = path.native();
        std::ranges::replace(portable, '\\', '/');
        return (projectRoot_ / std::filesystem::path{portable}).lexically_normal();
#endif
    }

    /** @copydoc SourceFileOpenService::ResolveLocation */
    Result<SourceOpenLocation> SourceFileOpenService::ResolveLocation(const std::filesystem::path &path) const {
        try {
            if (!projectRootValid_ || path.empty())
                return Result<SourceOpenLocation>::Failure(MakeError(SourceOpenErrors::InvalidRequest));
            if (!HasValidPathText(path))
                return Result<SourceOpenLocation>::Failure(MakeError(SourceOpenErrors::Unsafe));

            const std::filesystem::path lexicalPath = NormalizeInputPath(path);
            if (lexicalPath.empty())
                return Result<SourceOpenLocation>::Failure(MakePathError(SourceOpenErrors::Unsafe, lexicalPath));

            const Result<std::filesystem::path> canonical =
                ResolveCanonicalProjectPath(projectRoot_, lexicalPath, policy_.allowSymlinkedFiles);
            if (canonical.HasError())
                return Result<SourceOpenLocation>::Failure(canonical.ErrorValue());

            std::error_code error;
            const std::filesystem::path &canonicalPath = canonical.Value();
            error.clear();
            const std::filesystem::file_status status = std::filesystem::status(canonicalPath, error);
            if (error) {
                if (IsNoSuchFile(error))
                    return Result<SourceOpenLocation>::Failure(MakePathError(SourceOpenErrors::Missing, canonicalPath));
                return Result<SourceOpenLocation>::Failure(MakePathError(SourceOpenErrors::Unsafe, canonicalPath));
            }
            if (!std::filesystem::is_regular_file(status))
                return Result<SourceOpenLocation>::Failure(MakePathError(SourceOpenErrors::Unsupported, canonicalPath));

            const std::filesystem::path relativePath = canonicalPath.lexically_relative(projectRoot_);
            const auto document = SourceIdentity(relativePath);
            if (document.HasError())
                return Result<SourceOpenLocation>::Failure(MakePathError(SourceOpenErrors::Unsafe, canonicalPath));

            return Result<SourceOpenLocation>::Success(SourceOpenLocation{
                .absolutePath = canonicalPath,
                .document = document.Value(),
            });
        } catch (const std::system_error &) {
            return Result<SourceOpenLocation>::Failure(MakeError(SourceOpenErrors::Unsafe));
        }
    }

    Result<SourceOpenResult> SourceFileOpenService::Open(const SourceOpenRequest &request) {
        if (publishing_)
            return Result<SourceOpenResult>::Failure(MakeError(SourceDocumentErrors::Busy));
        if (request.path.empty())
            return Result<SourceOpenResult>::Failure(MakeError(SourceOpenErrors::InvalidRequest));

        const Result<SourceOpenLocation> location = ResolveLocation(request.path);
        if (location.HasError())
            return Result<SourceOpenResult>::Failure(location.ErrorValue());

        const SourceFileClassification classification = Classify(location.Value().absolutePath);
        if (!classification.IsSupported()) {
            if (request.mode == SourceOpenMode::AllowExternalFallback && policy_.externalEditorAvailable) {
                return Result<SourceOpenResult>::Success(MakeExternalFallbackResult(request, classification, location.Value()));
            }
            return Result<SourceOpenResult>::Failure(MakePathError(SourceOpenErrors::Unsupported, location.Value().absolutePath));
        }

        if (!policy_.embeddedEditorAvailable) {
            if (request.mode == SourceOpenMode::AllowExternalFallback && policy_.externalEditorAvailable) {
                return Result<SourceOpenResult>::Success(MakeExternalFallbackResult(request, classification, location.Value()));
            }
            return Result<SourceOpenResult>::Failure(MakePathError(SourceOpenErrors::EditorUnavailable, location.Value().absolutePath));
        }

        return OpenEmbedded(request, classification, location.Value());
    }

    /** @copydoc SourceFileOpenService::OpenEmbedded */
    Result<SourceOpenResult> SourceFileOpenService::OpenEmbedded(const SourceOpenRequest &request,
                                                                 const SourceFileClassification &classification,
                                                                 const SourceOpenLocation &location) {
        DocumentKind documentKind = DocumentKind::Source;
        if (classification.kind == SourceFileKind::Prefab)
            documentKind = DocumentKind::Prefab;
        else if (classification.kind == SourceFileKind::PCGGraph)
            documentKind = DocumentKind::PCGGraph;
        else if (classification.kind == SourceFileKind::Sequence)
            documentKind = DocumentKind::Sequence;
        else if (classification.kind == SourceFileKind::UiCanvas)
            documentKind = DocumentKind::UiCanvas;
        const DocumentOpenKey key{.kind = documentKind, .source = location.document};
        const Result<DocumentOpenResult> document = documentRegistry_->Open(key);
        if (document.HasError())
            return Result<SourceOpenResult>::Failure(document.ErrorValue());
        std::optional<SourceDocumentSnapshot> source;
        if (documentKind == DocumentKind::Source) {
            auto admitted = sourceDocuments_.Open(document.Value().identity, location.absolutePath);
            if (admitted.HasError()) {
                if (document.Value().disposition == DocumentOpenDisposition::Opened)
                    static_cast<void>(documentRegistry_->Close(document.Value().identity.instance));
                return Result<SourceOpenResult>::Failure(admitted.ErrorValue());
            }
            source = std::move(admitted).Value();
        }
        return Result<SourceOpenResult>::Success(SourceOpenResult{
            .origin = request.origin,
            .route = SourceOpenRoute::EmbeddedWorkspace,
            .classification = classification,
            .location = location,
            .document = document.Value(),
            .line = request.line,
            .column = request.column,
            .sourceSnapshot = std::move(source),
        });
    }

    /** @copydoc SourceFileOpenService::SaveAs */
    Result<SourceSaveResult> SourceFileOpenService::SaveAs(const SourceSaveRequest &request, const std::filesystem::path &destination,
                                                           DurableFileSystem &files, const CancellationToken cancellation) {
        try {
            if (publishing_)
                return Result<SourceSaveResult>::Failure(MakeError(SourceDocumentErrors::Busy));
            if (const auto admitted = AdmitSaveDestination(*this, destination); admitted.HasError())
                return Result<SourceSaveResult>::Failure(admitted.ErrorValue());
            const auto current = sourceDocuments_.Snapshot(request.instance);
            if (current.HasError())
                return Result<SourceSaveResult>::Failure(current.ErrorValue());
            const auto resolved =
                ResolveCanonicalProjectPath(projectRoot_, NormalizeInputPath(destination), policy_.allowSymlinkedFiles, true);
            if (resolved.HasError())
                return Result<SourceSaveResult>::Failure(resolved.ErrorValue());
            const auto &path = resolved.Value();
            auto source = SourceIdentity(path.lexically_relative(projectRoot_));
            if (source.HasError())
                return Result<SourceSaveResult>::Failure(source.ErrorValue());
            if (const auto identity = documentRegistry_->Find(request.instance); !identity || *identity != current.Value().Identity())
                return Result<SourceSaveResult>::Failure(MakeError(SourceDocumentErrors::Stale));
            auto registry = *documentRegistry_;
            DocumentIdentity replacement{{DocumentKind::Source, std::move(source).Value()}, request.instance};
            if (auto retargeted = registry.RetargetSource(request.instance, replacement.key); retargeted.HasError())
                return Result<SourceSaveResult>::Failure(retargeted.ErrorValue());

            struct Guard final {
                bool *value;

                explicit Guard(bool &busy) noexcept : value(&busy) {
                    *value = true;
                }

                ~Guard() noexcept {
                    *value = false;
                }

                Guard(const Guard &) = delete;
                Guard &operator=(const Guard &) = delete;
            };

            Guard guard{publishing_};

            auto saved = sourceDocuments_.SaveTo(request, files, std::move(replacement), path, cancellation);
            if (saved.HasValue())
                *documentRegistry_ = std::move(registry);
            return saved;
        } catch (const std::bad_alloc &) {
            return Result<SourceSaveResult>::Failure(MakeError(SourceDocumentErrors::TooLarge));
        }
    }

    /** @copydoc SourceFileOpenService::ResolveClose */
    Result<bool> SourceFileOpenService::ResolveClose(const SourceSaveRequest &request, const SourceCloseDecision decision,
                                                     DurableFileSystem &files) {
        using enum SourceCloseDecision;
        const auto current = sourceDocuments_.Snapshot(request.instance);
        if (current.HasError())
            return Result<bool>::Failure(current.ErrorValue());
        if (request.expectedRevision != current.Value().Revision())
            return Result<bool>::Failure(MakeError(SourceDocumentErrors::Stale));
        if (decision == Cancel)
            return Result<bool>::Success(false);
        if (decision == Save && current.Value().Dirty()) {
            auto saved = sourceDocuments_.Save(request, files);
            if (saved.HasError())
                return Result<bool>::Failure(saved.ErrorValue());
            if (saved.Value().disposition != SourceSaveDisposition::Durable)
                return Result<bool>::Failure(*saved.Value().diagnostic);
        } else if (decision != Save && decision != Discard) {
            return Result<bool>::Failure(MakeError(SourceDocumentErrors::Invalid));
        }
        if (auto closed = CloseDocument(request.instance, decision == Discard); closed.HasError())
            return Result<bool>::Failure(closed.ErrorValue());
        return Result<bool>::Success(true);
    }

    /** @copydoc SourceFileOpenService::CloseDocument */
    Result<void> SourceFileOpenService::CloseDocument(const DocumentInstanceId instance, const bool discardDirty) {
        if (publishing_)
            return Result<void>::Failure(MakeError(SourceDocumentErrors::Busy));
        const auto identity = documentRegistry_->Find(instance);
        if (!identity)
            return Result<void>::Failure(MakeError(SourceDocumentErrors::Stale));
        if (identity->key.kind == DocumentKind::Source) {
            const auto snapshot = sourceDocuments_.Snapshot(instance);
            if (snapshot.HasError())
                return Result<void>::Failure(snapshot.ErrorValue());
            if (snapshot.Value().Dirty() && !discardDirty)
                return Result<void>::Failure(MakeError(SourceDocumentErrors::Invalid));
            if (const auto closed = sourceDocuments_.Close(instance); closed.HasError())
                return closed;
        }
        return documentRegistry_->Close(instance);
    }
}  // namespace Horo::Editor
