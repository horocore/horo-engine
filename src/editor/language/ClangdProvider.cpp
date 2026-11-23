#include "ClangdProvider.h"

#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Foundation/PathUtils.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <new>
#include <ranges>
#include <thread>

namespace Horo::Editor::Language {
    namespace {
        constexpr auto CompileCommandsName = "compile_commands.json";
        constexpr std::uintmax_t MaximumCompileCommandsBytes = 64 * 1024 * 1024;
        constexpr std::size_t MaximumSearchDirectories = 128;
        constexpr std::size_t MaximumPreviewBytes = 256 * 1024;
        constexpr auto PreviewDeadline = std::chrono::seconds{5};
        const ErrorDomainId Domain{"editor.language.clangd"};
        const ErrorCodeDescriptor ExecutableUnavailable{Domain,
                                                        ErrorCode{"clangd_executable_unavailable"},
                                                        ErrorSeverity::Warning,
                                                        "Clangd is unavailable. Install it or select an absolute executable path.",
                                                        {}};
        const ErrorCodeDescriptor DatabaseUnavailable{Domain,
                                                      ErrorCode{"clangd_compile_commands_unavailable"},
                                                      ErrorSeverity::Warning,
                                                      "Compile commands are unavailable. Configure a Ninja or Makefiles build with "
                                                      "CMAKE_EXPORT_COMPILE_COMMANDS enabled, or select compile_commands.json.",
                                                      {}};
        const ErrorCodeDescriptor ConfigurationUnavailable{Domain,
                                                           ErrorCode{"clangd_configuration_unavailable"},
                                                           ErrorSeverity::Warning,
                                                           "Clangd configuration could not be prepared.",
                                                           {}};
        const ErrorCodeDescriptor PreviewTimedOut{Domain,
                                                  ErrorCode{"clangd_preview_timed_out"},
                                                  ErrorSeverity::Warning,
                                                  "Clangd did not publish preview diagnostics before the deadline.",
                                                  {}};

        /** @brief Stages a detached draft or edits only the isolated source owner's buffer. */
        Result<SourceDocumentSnapshot> OpenPreview(SourceFileOpenService &sources, const ClangdPreviewSource &preview) {
            using Output = Result<SourceDocumentSnapshot>;
            if (!preview.base.Revision())
                return sources.StageNew(preview.path, std::string{preview.text});
            const auto original = preview.base.Identity().key.source;
            const auto &encoded = original.Value();
            const std::filesystem::path originalPath{std::u8string{encoded.begin(), encoded.end()}};
            auto opened = sources.Open({.path = originalPath, .mode = SourceOpenMode::EmbeddedOnly});
            if (opened.HasError())
                return Output::Failure(std::move(opened).ErrorValue());
            const auto &base = opened.Value().sourceSnapshot;
            if (!base || base->Identity().key.source != original || base->DiskBase() != preview.base.DiskBase())
                return Output::Failure(MakeError(SourceDocumentErrors::Stale));
            if (preview.path != original)
                return sources.StageNew(preview.path, std::string{preview.text});
            return sources.Documents().Edit(base->Identity().instance,
                                            {base->Revision(), 0, base->Text().size(), std::string{preview.text}});
        }

        /** @brief Detects superseded source publications while allowing previews for new files. */
        bool HasStalePreviewBase(const SourceDocumentSnapshot &base) noexcept {
            return base.Revision() && !base.IsCurrent();
        }

        /** @brief Accepts only diagnostics correlated by the isolated connection to the detached revision. */
        Result<LanguageDiagnostics> AwaitPreview(ProjectLanguageWorkspace &workspace, const SourceDocumentSnapshot &detached,
                                                 const SourceDocumentSnapshot &base, std::stop_token stop) {
            using Output = Result<LanguageDiagnostics>;
            using Clock = ProjectLanguageWorkspace::Clock;
            const auto deadline = Clock::now() + PreviewDeadline;
            for (auto now = Clock::now(); now < deadline; now = Clock::now()) {
                if (stop.stop_requested())
                    return Output::Failure(MakeError(SourceDocumentErrors::Cancelled));
                if (HasStalePreviewBase(base))
                    return Output::Failure(MakeError(SourceDocumentErrors::Stale));
                for (auto &result : workspace.Poll(std::span{&detached, 1}, now)) {
                    auto *diagnostics = std::get_if<LanguageDiagnostics>(&result.payload);
                    if (!diagnostics || !result.context.source.SharesStateWith(detached))
                        continue;
                    if (HasStalePreviewBase(base))
                        return Output::Failure(MakeError(SourceDocumentErrors::Stale));
                    return Output::Success(std::move(*diagnostics));
                }
                if (const auto *error = workspace.ProviderError(ClangdProviderId))
                    return Output::Failure(*error);
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
            return Output::Failure(MakeError(PreviewTimedOut));
        }

        /** @brief Checks one candidate without searching relative current-directory entries. */
        std::filesystem::path Executable(const std::filesystem::path &candidate) {
            if (!candidate.is_absolute() ||
                candidate.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
                return {};
            std::error_code error;
            auto resolved = std::filesystem::canonical(candidate, error);
            if (error)
                return {};
            const auto status = std::filesystem::status(resolved, error);
            if (error || !std::filesystem::is_regular_file(status))
                return {};
#ifndef _WIN32
            using enum std::filesystem::perms;
            if ((status.permissions() & (owner_exec | group_exec | others_exec)) == none)
                return {};
#endif
            return resolved;
        }

        /** @brief Resolves an explicit host choice first, then native executable names in trusted search directories. */
        std::filesystem::path ResolveExecutable(const std::filesystem::path &root, const ClangdSettings &settings) {
            if (settings.executable)
                return Executable(*settings.executable);
#ifdef _WIN32
            constexpr auto program = "clangd.exe";
#else
            constexpr auto program = "clangd";
#endif
            for (const auto &directory :
                 std::span{settings.searchDirectories}.first(std::min(settings.searchDirectories.size(), MaximumSearchDirectories))) {
                if (!directory.is_absolute() || Foundation::Paths::HasPathPrefix(root, directory))
                    continue;
                auto resolved = Executable(directory / program);
                if (!resolved.empty() && !Foundation::Paths::HasPathPrefix(root, resolved))
                    return resolved;
            }
            return {};
        }

        /** @brief Locates a regular standard-named file with a finite physical size. */
        std::filesystem::path CompileCommands(const std::filesystem::path &candidate) {
            if (candidate.filename() != CompileCommandsName ||
                candidate.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
                return {};
            std::error_code error;
            if (!std::filesystem::is_regular_file(candidate, error) || error)
                return {};
            const auto size = std::filesystem::file_size(candidate, error);
            if (error || size < 2 || size > MaximumCompileCommandsBytes)
                return {};
            const auto parent = std::filesystem::canonical(candidate.parent_path(), error);
            return error ? std::filesystem::path{} : parent / CompileCommandsName;
        }

        /** @brief Requires workspace-selected metadata to resolve to the same native project. */
        std::filesystem::path ProjectCompileCommands(const std::filesystem::path &root, const std::filesystem::path &candidate) {
            auto path = CompileCommands(candidate);
            if (path.empty())
                return {};
            std::error_code error;
            const auto resolved = std::filesystem::canonical(path, error);
            if (error || !Foundation::Paths::HasPathPrefix(root, path) || !Foundation::Paths::HasPathPrefix(root, resolved))
                return {};
            return path;
        }

        /** @brief Uses an explicit database or the project and gameplay build's conventional locations. */
        std::filesystem::path ResolveCompileCommands(const std::filesystem::path &root, const ClangdSettings &settings) {
            if (settings.compileCommands) {
                return settings.compileCommands->is_absolute() ? CompileCommands(*settings.compileCommands)
                                                               : ProjectCompileCommands(root, root / *settings.compileCommands);
            }
            const std::array candidates{root / CompileCommandsName, Application::GameplayLocalBuildDirectory(root) / CompileCommandsName};
            for (const auto &candidate : candidates) {
                auto path = ProjectCompileCommands(root, candidate);
                if (!path.empty())
                    return path;
            }
            return {};
        }

        /** @brief Preserves Unicode paths in the single argument passed to Clangd. */
        std::string DatabaseArgument(const std::filesystem::path &file) {
            const auto path = file.parent_path().u8string();
            std::string argument{"--compile-commands-dir="};
            argument.append(reinterpret_cast<const char *>(path.data()), path.size());
            return argument;
        }
    }  // namespace

    /** @copydoc CaptureClangdHostSettings */
    ClangdSettings CaptureClangdHostSettings() {
#ifdef _WIN32
        const auto *executable = _wgetenv(L"HORO_CLANGD_EXECUTABLE");
        const auto *database = _wgetenv(L"HORO_CLANGD_COMPILE_COMMANDS");
        const auto *path = _wgetenv(L"PATH");
        constexpr auto separator = L';';
#else
        const auto *executable = std::getenv("HORO_CLANGD_EXECUTABLE");
        const auto *database = std::getenv("HORO_CLANGD_COMPILE_COMMANDS");
        const auto *path = std::getenv("PATH");
        constexpr auto separator = ':';
#endif
        ClangdSettings settings;
        if (executable && *executable)
            settings.executable = executable;
        if (database && *database)
            settings.compileCommands = database;
        if (path) {
            using StringView = std::basic_string_view<std::filesystem::path::value_type>;
            for (const auto part : StringView{path} | std::views::split(separator)) {
                auto directory = std::filesystem::path{part.begin(), part.end()};
                if (directory.is_absolute())
                    settings.searchDirectories.push_back(std::move(directory));
                if (settings.searchDirectories.size() == MaximumSearchDirectories)
                    break;
            }
        }
        return settings;
    }

    /** @copydoc PrepareClangdProvider */
    Result<ProjectLanguageProvider> PrepareClangdProvider(const SourceFileOpenService &sources, const ClangdSettings &settings) {
        using Output = Result<ProjectLanguageProvider>;
        try {
            const auto &root = sources.ProjectRoot();
            auto clangd = ResolveExecutable(root, settings);
            if (clangd.empty())
                return Output::Failure(MakeError(ExecutableUnavailable));
            const auto commands = ResolveCompileCommands(root, settings);
            if (commands.empty())
                return Output::Failure(MakeError(DatabaseUnavailable));
            return Output::Success({.id = ClangdProviderId,
                                    .languageId = "cpp",
                                    .extensions = sources.Policy().nativeSourceExtensions,
                                    .executable = std::move(clangd),
                                    .arguments = {DatabaseArgument(commands), "--enable-config=false",
                                                  settings.backgroundIndex ? "--background-index=true" : "--background-index=false"}});
        } catch (const std::filesystem::filesystem_error &) {
            return Output::Failure(MakeError(ConfigurationUnavailable));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(ConfigurationUnavailable));
        }
    }

    /** @copydoc CheckClangdPreview */
    Result<LanguageDiagnostics> CheckClangdPreview(IExternalProcessRunner &runner, ClangdSettings settings,
                                                   const ClangdPreviewSource &source, std::stop_token stop) {
        using Output = Result<LanguageDiagnostics>;
        if (stop.stop_requested())
            return Output::Failure(MakeError(SourceDocumentErrors::Cancelled));
        if (HasStalePreviewBase(source.base))
            return Output::Failure(MakeError(SourceDocumentErrors::Stale));
        if (source.text.size() > MaximumPreviewBytes)
            return Output::Failure(MakeError(SourceDocumentErrors::TooLarge));
        SourceFileOpenService sources{source.projectRoot};
        auto detached = OpenPreview(sources, source);
        if (detached.HasError())
            return Output::Failure(std::move(detached).ErrorValue());
        settings.backgroundIndex = false;
        const auto provider = PrepareClangdProvider(sources, settings);
        if (provider.HasError())
            return Output::Failure(provider.ErrorValue());
        auto workspace = ProjectLanguageWorkspace::Create(runner, sources, std::span{&provider.Value(), 1});
        if (workspace.HasError())
            return Output::Failure(std::move(workspace).ErrorValue());
        return AwaitPreview(*workspace.Value(), detached.Value(), source.base, stop);
    }
}  // namespace Horo::Editor::Language
