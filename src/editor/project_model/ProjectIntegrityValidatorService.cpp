#include "Horo/Editor/ProjectIntegrityValidatorService.h"

#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Paths.h"
#include "editor/EditorServiceErrors.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace Horo::Editor {

    namespace {
        [[nodiscard]] bool HasNativeGameplaySources(const std::filesystem::path &projectRoot) {
            for (const std::filesystem::path root : {projectRoot / "source" / "gameplay", projectRoot / "src" / "gameplay"}) {
                std::error_code error;
                if (!std::filesystem::is_directory(root, error))
                    continue;
                std::filesystem::recursive_directory_iterator iterator(root, std::filesystem::directory_options::skip_permission_denied,
                                                                       error);
                const std::filesystem::recursive_directory_iterator end;
                while (iterator != end && !error) {
                    if (iterator->is_regular_file(error) &&
                        (iterator->path().extension() == ".cpp" || iterator->path().extension() == ".cc" ||
                         iterator->path().extension() == ".cxx") &&
                        iterator->path().filename() != "GameModule.cpp")
                        return true;
                    iterator.increment(error);
                }
            }
            return false;
        }

        [[nodiscard]] std::string ModuleIdFromProjectName(const std::filesystem::path &projectRoot) {
            std::string slug = projectRoot.filename().string();
            std::ranges::transform(slug, slug.begin(), [](const unsigned char value) {
                return std::isalnum(value) ? static_cast<char>(std::tolower(value)) : '_';
            });
            if (slug.empty())
                slug = "game";
            return "game." + slug;
        }
    }  // namespace

    bool ProjectIntegrityReport::HasAutoFixableIssues() const noexcept {
        return std::ranges::any_of(issues, [](const ProjectIntegrityIssue &issue) {
            return issue.isAutoFixable;
        });
    }

    ProjectIntegrityValidatorService::ProjectIntegrityValidatorService(DurableFileSystem &durableFiles) : durableFiles_(durableFiles) {}

    /** @copydoc ProjectIntegrityValidatorService::Inspect */
    ProjectIntegrityReport ProjectIntegrityValidatorService::Inspect(const std::filesystem::path &projectRoot) const {
        ProjectIntegrityReport report{.projectRoot = projectRoot};
        std::error_code error;

        const bool hasNativeSources = HasNativeGameplaySources(projectRoot);
        if (const std::filesystem::path cmakeLists = projectRoot / "CMakeLists.txt";
            hasNativeSources && !std::filesystem::is_regular_file(cmakeLists, error)) {
            report.issues.push_back(ProjectIntegrityIssue{
                .kind = ProjectIntegrityIssueKind::MissingCMakeLists,
                .targetPath = cmakeLists,
                .description = "Native C++ gameplay sources exist, but CMakeLists.txt is missing from project root.",
                .isAutoFixable = true,
            });
        }
        error.clear();

        if (const std::filesystem::path gameModule = projectRoot / "source" / "gameplay" / "GameModule.cpp";
            hasNativeSources && !std::filesystem::is_regular_file(gameModule, error)) {
            report.issues.push_back(ProjectIntegrityIssue{
                .kind = ProjectIntegrityIssueKind::MissingGameModuleBootstrap,
                .targetPath = gameModule,
                .description = "Native C++ gameplay sources exist, but GameModule.cpp bootstrap file is missing.",
                .isAutoFixable = true,
            });
        }
        error.clear();

        if (const std::filesystem::path scriptsDir = ProjectLayout::ScriptsRoot(projectRoot);
            !std::filesystem::is_directory(scriptsDir, error)) {
            report.issues.push_back(ProjectIntegrityIssue{
                .kind = ProjectIntegrityIssueKind::MissingScriptsDirectory,
                .targetPath = scriptsDir,
                .description = "Project scripts directory is missing.",
                .isAutoFixable = true,
            });
        }
        error.clear();

        if (const std::filesystem::path scenesDir = ProjectLayout::ScenesRoot(projectRoot); !std::filesystem::is_directory(scenesDir, error)) {
            report.issues.push_back(ProjectIntegrityIssue{
                .kind = ProjectIntegrityIssueKind::MissingScenesDirectory,
                .targetPath = scenesDir,
                .description = "Project scenes directory is missing.",
                .isAutoFixable = true,
            });
        }

        return report;
    }

    /** @copydoc ProjectIntegrityValidatorService::Repair */
    Result<void> ProjectIntegrityValidatorService::Repair(const std::filesystem::path &projectRoot) const {
        const ProjectIntegrityReport report = Inspect(projectRoot);
        if (!report.HasIssues())
            return Result<void>::Success();

        LOG_INFO("editor.project_validator", "Repairing %zu integrity issues for project '%s'...", report.issues.size(),
                 projectRoot.string().c_str());

        for (const ProjectIntegrityIssue &issue : report.issues) {
            if (!issue.isAutoFixable)
                continue;

            switch (issue.kind) {
                case ProjectIntegrityIssueKind::MissingCMakeLists: {
                    const std::string moduleId = ModuleIdFromProjectName(projectRoot);
                    const std::string cmakeContent =
                        "cmake_minimum_required(VERSION 3.25)\n"
                        "project(HoroGame LANGUAGES CXX)\n\n"
                        "find_package(HoroEngineGameplay CONFIG REQUIRED)\n\n"
                        "file(GLOB_RECURSE HORO_GAMEPLAY_SOURCES CONFIGURE_DEPENDS \"source/gameplay/*.cpp\")\n"
                        "if(HORO_GAMEPLAY_SOURCES)\n"
                        "    horo_add_gameplay_module(HoroGameGameplay\n"
                        "        MODULE_ID " +
                        moduleId +
                        "\n"
                        "        SOURCES ${HORO_GAMEPLAY_SOURCES}\n"
                        "    )\n"
                        "endif()\n";
                    if (const Result<void> written = durableFiles_.WriteDurable(issue.targetPath, std::as_bytes(std::span{cmakeContent}));
                        written.HasError()) {
                        LOG_ERROR("editor.project_validator", "Failed to repair CMakeLists.txt for '%s': %s", projectRoot.string().c_str(),
                                  written.ErrorValue().message.c_str());
                        return written;
                    }
                    LOG_INFO("editor.project_validator", "Repaired missing CMakeLists.txt at '%s'", issue.targetPath.string().c_str());
                    break;
                }
                case ProjectIntegrityIssueKind::MissingGameModuleBootstrap: {
                    const std::string moduleContent =
                        "#include <Horo/Gameplay/GameModule.h>\n\n"
                        "namespace {\n"
                        "class ProjectGameModule final : public Horo::Gameplay::IGameModule {\n"
                        "public:\n"
                        "    Horo::Result<void> Register(Horo::Gameplay::GameRegistrationContext&) override {\n"
                        "        return Horo::Result<void>::Success();\n"
                        "    }\n"
                        "    Horo::Result<void> Start(Horo::Gameplay::GameRuntimeContext&) override {\n"
                        "        return Horo::Result<void>::Success();\n"
                        "    }\n"
                        "    void Stop(Horo::Gameplay::GameRuntimeContext&) noexcept override {}\n"
                        "};\n"
                        "}\n\n"
                        "extern \"C\" HORO_GAME_EXPORT Horo::Gameplay::IGameModule* CreateGameModule() noexcept {\n"
                        "    return new ProjectGameModule{};\n"
                        "}\n\n"
                        "extern \"C\" HORO_GAME_EXPORT void DestroyGameModule(Horo::Gameplay::IGameModule* module) noexcept {\n"
                        "    delete module;\n"
                        "}\n";
                    if (const Result<void> written = durableFiles_.WriteDurable(issue.targetPath, std::as_bytes(std::span{moduleContent}));
                        written.HasError()) {
                        LOG_ERROR("editor.project_validator", "Failed to repair GameModule.cpp for '%s': %s", projectRoot.string().c_str(),
                                  written.ErrorValue().message.c_str());
                        return written;
                    }
                    LOG_INFO("editor.project_validator", "Repaired missing GameModule.cpp at '%s'", issue.targetPath.string().c_str());
                    break;
                }
                case ProjectIntegrityIssueKind::MissingScriptsDirectory:
                case ProjectIntegrityIssueKind::MissingScenesDirectory: {
                    std::error_code error;
                    std::filesystem::create_directories(issue.targetPath, error);
                    if (error) {
                        LOG_ERROR("editor.project_validator", "Failed to repair directory '%s': %s", issue.targetPath.string().c_str(),
                                  error.message().c_str());
                        return Result<void>::Failure(MakeError(ProjectCreationErrors::InvalidRequest, error.message()));
                    }
                    LOG_INFO("editor.project_validator", "Created missing directory '%s'", issue.targetPath.string().c_str());
                    break;
                }
            }
        }

        return Result<void>::Success();
    }

}  // namespace Horo::Editor
