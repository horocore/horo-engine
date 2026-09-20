#include "Horo/Editor/SourceFileOpenService.h"

#include "Horo/Foundation/PathUtils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <ranges>
#include <system_error>
#include <utility>

namespace Horo::Editor {
    namespace {
        const ErrorDomainId SourceOpenDomain{"horo.editor.source_open"};

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
            const std::filesystem::file_status status = std::filesystem::status(canonical, error);
            if (error || !std::filesystem::is_directory(status))
                return {};

            valid = true;
            return canonical;
        }

        [[nodiscard]] Error MakePathError(const ErrorCodeDescriptor &descriptor, const std::filesystem::path &path) {
            return MakeError(descriptor, "Source path '" + path.string() + "' cannot be opened safely.");
        }

        [[nodiscard]] Result<std::filesystem::path> ResolveCanonicalProjectPath(const std::filesystem::path &projectRoot,
                                                                                const std::filesystem::path &lexicalPath,
                                                                                const bool allowSymlinkedFiles) {
            if (!Horo::Foundation::Paths::HasPathPrefix(projectRoot, lexicalPath))
                return Result<std::filesystem::path>::Failure(MakePathError(SourceOpenErrors::Unsafe, lexicalPath));

            std::error_code error;
            const std::filesystem::file_status lexicalStatus = std::filesystem::symlink_status(lexicalPath, error);
            if (error)
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
        };
    }

    SourceFileOpenService::SourceFileOpenService(std::filesystem::path projectRoot, SourceFilePolicy policy)
        : projectRootValid_(false), projectRoot_(ResolveRoot(projectRoot, projectRootValid_)), policy_(std::move(policy)) {
        policy_.nativeSourceExtensions = NormalizeValues(policy_.nativeSourceExtensions, true);
        policy_.horoScriptExtensions = NormalizeValues(policy_.horoScriptExtensions, true);
        policy_.projectTextExtensions = NormalizeValues(policy_.projectTextExtensions, true);
        policy_.projectTextFileNames = NormalizeValues(policy_.projectTextFileNames, false);
    }

    SourceFileClassification SourceFileOpenService::Classify(const std::filesystem::path &path) const {
        const std::string extension = NormalizeExtension(path.extension().string());
        const std::string fileName = LowercaseAscii(path.filename().string());
        if (Contains(policy_.nativeSourceExtensions, extension))
            return SourceFileClassification{SourceFileKind::NativeSource, extension};
        if (Contains(policy_.horoScriptExtensions, extension))
            return SourceFileClassification{SourceFileKind::HoroScript, extension};
        if (Contains(policy_.projectTextExtensions, extension) || Contains(policy_.projectTextFileNames, fileName))
            return SourceFileClassification{SourceFileKind::ProjectText, extension};
        return SourceFileClassification{SourceFileKind::Unsupported, extension};
    }

    std::filesystem::path SourceFileOpenService::NormalizeInputPath(const std::filesystem::path &path) const {
        if (path.is_absolute())
            return path.lexically_normal();

        std::string portable = path.generic_string();
#ifndef _WIN32
        std::ranges::replace(portable, '\\', '/');
#endif
        return (projectRoot_ / std::filesystem::path{portable}).lexically_normal();
    }

    Result<SourceOpenLocation> SourceFileOpenService::ResolveLocation(const std::filesystem::path &path) const {
        if (!projectRootValid_ || path.empty())
            return Result<SourceOpenLocation>::Failure(MakeError(SourceOpenErrors::InvalidRequest));

        const std::filesystem::path lexicalPath = NormalizeInputPath(path);
        if (lexicalPath.empty())
            return Result<SourceOpenLocation>::Failure(MakePathError(SourceOpenErrors::Unsafe, lexicalPath));

        const Result<std::filesystem::path> canonical = ResolveCanonicalProjectPath(projectRoot_, lexicalPath, policy_.allowSymlinkedFiles);
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
        const auto document = SourceDocumentId::Parse(relativePath.generic_string());
        if (document.HasError())
            return Result<SourceOpenLocation>::Failure(MakePathError(SourceOpenErrors::Unsafe, canonicalPath));

        return Result<SourceOpenLocation>::Success(SourceOpenLocation{
            .absolutePath = canonicalPath,
            .document = document.Value(),
        });
    }

    Result<SourceOpenResult> SourceFileOpenService::Open(const SourceOpenRequest &request) {
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

        const DocumentOpenKey key{.kind = DocumentKind::Source, .source = location.Value().document};
        const Result<DocumentOpenResult> document = documentRegistry_.Open(key);
        if (document.HasError())
            return Result<SourceOpenResult>::Failure(document.ErrorValue());
        return Result<SourceOpenResult>::Success(SourceOpenResult{
            .origin = request.origin,
            .route = SourceOpenRoute::EmbeddedWorkspace,
            .classification = classification,
            .location = location.Value(),
            .document = document.Value(),
            .line = request.line,
            .column = request.column,
        });
    }
}  // namespace Horo::Editor
