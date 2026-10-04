#include "NetworkProductLaunch.h"
#include "cli_host/CliHost.h"

#include <iostream>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include "Horo/Cli/CliErrors.h"

#include <filesystem>
#include <io.h>
#include <string>
#else
#include <unistd.h>
#endif

namespace {
    /** @brief Borrows invocation storage only for the synchronous CLI or existing network host call. */
    int RunProcess(const std::span<char *> argv, std::optional<Horo::Error> admissionFailure = {}) {
        if (!admissionFailure && argv.size() >= 2 && std::string_view{argv[1]} == "--run-network-product")
            return Horo::Application::Internal::RunNetworkProduct(argv.subspan(2));
        std::vector<std::string_view> arguments;
        for (std::size_t index = 1; index < argv.size(); ++index)
            arguments.emplace_back(argv[index]);
#if defined(_WIN32)
        const bool terminal = _isatty(_fileno(stderr)) != 0;
#else
        const bool terminal = isatty(STDERR_FILENO) != 0;
#endif
        return Horo::Application::Internal::RunCli(arguments,
                                                   {.processRole = "cli",
                                                    .engineVersion = HORO_ENGINE_VERSION_STRING,
                                                    .buildConfiguration = HORO_BUILD_CONFIGURATION,
                                                    .sourceRevision = HORO_SOURCE_REVISION},
                                                   std::cout, std::cerr, terminal, std::move(admissionFailure));
    }
}  // namespace

#if defined(_WIN32)
/** @brief Receives native UTF-16 argv without depending on the process ANSI code page. */
int wmain(const int argc, wchar_t **argv) {
    std::vector<std::string> owned;
    std::optional<Horo::Error> admissionFailure;
    for (int index = 0; index < argc; ++index) {
        try {
            const auto text = std::filesystem::path{argv[index]}.u8string();
            owned.emplace_back(text.begin(), text.end());
        } catch (const std::filesystem::filesystem_error &) {
            owned.emplace_back();
            admissionFailure = Horo::MakeError(Horo::Cli::CliErrors::ParseFailed, "Native command argument encoding is invalid.");
        }
    }
    std::vector<char *> arguments;
    for (auto &argument : owned)
        arguments.push_back(argument.data());
    return RunProcess(arguments, std::move(admissionFailure));
}
#else
/** @brief Preserves native UTF-8 argv for the synchronous terminal host. */
int main(const int argc, char **argv) {
    return RunProcess(std::span{argv, static_cast<std::size_t>(argc)});
}
#endif
