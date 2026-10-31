/** @file RccppMacCompiler.cpp
 * @brief Private macOS RCC++ compiler transport; GUI-owner polling never waits for compiler output.
 */
#include "Compiler.h"
#include "ICompilerLogger.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {
    /** @brief Quotes a path for RCC++'s trusted development compiler command. */
    std::string Quote(const std::string &text) {
        std::string result = "'";
        for (const char character : text)
            result += character == '\'' ? "'\\''" : std::string(1, character);
        return result + "'";
    }

    /** @brief Reaps only the child owned by this transport, retrying interrupted waits. */
    pid_t Wait(const pid_t child, int &status, const int flags) {
        pid_t result;
        do {
            result = waitpid(child, &status, flags);
        } while (result < 0 && errno == EINTR);
        return result;
    }
}  // namespace

/** @brief Owner-thread state; the child process group exclusively owns compilation and linking. */
class PlatformCompilerImplData {
public:
    ICompilerLogger *logger{};
    pid_t child{};
    bool complete{};
    std::FILE *diagnostics{};

    ~PlatformCompilerImplData() {
        if (child > 0) {
            // No compiler or descendant may outlive its runtime/logger during shutdown.
            kill(-child, SIGKILL);
            int status{};
            Wait(child, status, 0);
        }
        if (diagnostics)
            std::fclose(diagnostics);
    }

    /** @brief Drains bounded completed diagnostics as data, never as a printf format. */
    void Finish() {
        if (diagnostics) {
            std::rewind(diagnostics);
            std::array<char, 4096> line{};
            constexpr std::size_t maximumLines = 1024;
            std::size_t count = 0;
            while (count++ < maximumLines && std::fgets(line.data(), static_cast<int>(line.size()), diagnostics)) {
                const auto end = std::string_view{line.data()}.find_first_of("\r\n");
                if (end != std::string_view::npos)
                    line[end] = '\0';
                if (logger)
                    logger->LogInfo("%s", line.data());
            }
            if (!std::feof(diagnostics) && logger)
                logger->LogWarning("RCC++ compiler diagnostics truncated.");
            std::fclose(diagnostics);
            diagnostics = nullptr;
        }
        complete = true;
    }
};

/** @copydoc Compiler::Compiler */
Compiler::Compiler() : m_pImplData(nullptr), m_bFastCompileMode(false) {}

/** @copydoc Compiler::~Compiler */
Compiler::~Compiler() {
    delete m_pImplData;
}

/** @copydoc Compiler::Initialise */
void Compiler::Initialise(ICompilerLogger *logger) {
    delete m_pImplData;
    m_pImplData = new PlatformCompilerImplData;
    m_pImplData->logger = logger;
}

/** @copydoc Compiler::GetObjectFileExtension */
std::string Compiler::GetObjectFileExtension() const {
    return ".o";
}

/** @copydoc Compiler::GetIsComplete */
bool Compiler::GetIsComplete() const {
    if (!m_pImplData)
        return false;
    auto &state = *m_pImplData;
    if (state.child > 0) {
        int status{};
        const auto result = Wait(state.child, status, WNOHANG);
        if (result == state.child || (result < 0 && errno == ECHILD)) {
            state.child = 0;
            state.Finish();
            if (state.logger && (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0))
                state.logger->LogError("RCC++ compiler failed; retaining the installed GUI module.");
        }
    }
    return state.complete;
}

/** @copydoc Compiler::RunCompile */
void Compiler::RunCompile(const std::vector<FileSystemUtils::Path> &files, const CompilerOptions &options,
                          const std::vector<FileSystemUtils::Path> &libraries, const FileSystemUtils::Path &module) {
    auto &state = *m_pImplData;
    if (state.child > 0)
        return;
    state.complete = false;
    std::error_code error;
    const std::filesystem::path intermediate{options.intermediatePath.c_str()};
    std::filesystem::create_directories(intermediate, error);
    if (error) {
        if (state.logger)
            state.logger->LogError("Unable to create RCC++ intermediate directory: %s", error.message().c_str());
        state.complete = true;
        return;
    }

    const auto compiler = Quote(options.compilerLocation.m_string.empty() ? "/usr/bin/clang++" : options.compilerLocation.m_string);
    std::string flags = " -g -fPIC -fvisibility=hidden ";
    switch (GetActualOptimizationLevel(options.optimizationLevel)) {
        case RCCPPOPTIMIZATIONLEVEL_PERF:
            flags += "-O2 ";
            break;
        case RCCPPOPTIMIZATIONLEVEL_NOT_SET:
            break;
        default:
            flags += "-O0 ";
            break;
    }
    for (const auto &include : options.includeDirList)
        flags += "-I" + Quote(include.m_string) + " ";
    flags += options.compileOptions + " ";

    // Compile separately so cached RCC++ objects retain the expected basename.
    // Every step must succeed before the unique candidate module is published.
    std::string script = "set -e\n";
    std::string objects;
    for (const auto &file : files) {
        std::filesystem::path object{file.c_str()};
        if (object.extension() != ".o") {
            object = intermediate / object.filename().replace_extension(".o");
            script += compiler + flags + "-c " + Quote(file.m_string) + " -o " + Quote(object.string()) + "\n";
        }
        objects += " " + Quote(object.string());
    }
    script += compiler + flags + "-bundle " + options.linkOptions + objects;
    for (const auto &directory : options.libraryDirList)
        script += " -L" + Quote(directory.m_string) + " -F" + Quote(directory.m_string);
    for (const auto &library : libraries)
        script += " " + Quote(library.m_string);
    script += " -o " + Quote(module.m_string) + "\n";

    // A temporary regular file cannot fill a pipe and deadlock a verbose compiler.
    // posix_spawn avoids running allocating C++ code after fork in the GUI process.
    state.diagnostics = std::tmpfile();
    if (!state.diagnostics) {
        if (state.logger)
            state.logger->LogError("Unable to open RCC++ compiler diagnostics.");
        state.complete = true;
        return;
    }
    const std::unique_ptr<std::FILE, decltype(&std::fclose)> command{std::tmpfile(), &std::fclose};
    if (!command || std::fwrite(script.data(), 1, script.size(), command.get()) != script.size() || std::fflush(command.get()) != 0) {
        state.Finish();
        if (state.logger)
            state.logger->LogError("Unable to prepare RCC++ compiler command file.");
        return;
    }
    std::rewind(command.get());
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int result = posix_spawn_file_actions_init(&actions);
    if (result == 0) {
        result = posix_spawnattr_init(&attributes);
        if (result == 0) {
            result = posix_spawn_file_actions_adddup2(&actions, fileno(command.get()), STDIN_FILENO);
            if (result == 0)
                result = posix_spawn_file_actions_adddup2(&actions, fileno(state.diagnostics), STDOUT_FILENO);
            if (result == 0)
                result = posix_spawn_file_actions_adddup2(&actions, fileno(state.diagnostics), STDERR_FILENO);
            if (result == 0)
                result = posix_spawn_file_actions_addclose(&actions, fileno(state.diagnostics));
            if (result == 0)
                result = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_CLOEXEC_DEFAULT);
            if (result == 0)
                result = posix_spawnattr_setpgroup(&attributes, 0);
            // GUI registrations can produce scripts larger than macOS ARG_MAX.
            // The shell reads its owned stdin file, so command size is independent of argv limits.
            std::array<char *, 2> arguments{const_cast<char *>("sh"), nullptr};
            if (result == 0)
                result = posix_spawn(&state.child, "/bin/sh", &actions, &attributes, arguments.data(), environ);
            posix_spawnattr_destroy(&attributes);
        }
        posix_spawn_file_actions_destroy(&actions);
    }
    if (result != 0) {
        state.child = 0;
        state.Finish();
        if (state.logger)
            state.logger->LogError("Unable to spawn RCC++ compiler: %s (%d)", std::strerror(result), result);
    }
}
