#include "ClangdProvider.h"

#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Foundation/PathUtils.h"

#include <algorithm>
#include <array>
#include <new>

namespace Horo::Editor::Language {
    namespace {
        constexpr auto CompileCommandsName = "compile_commands.json";
        constexpr std::uintmax_t MaximumCompileCommandsBytes = 64 * 1024 * 1024;
        constexpr std::size_t MaximumSearchDirectories = 128;
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
            return Output::Success({.id = "clangd",
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
}  // namespace Horo::Editor::Language
