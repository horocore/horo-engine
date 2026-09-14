#include "Horo/Extensions/ExtensionAbiConformance.h"

#include <iostream>
#include <optional>
#include <span>
#include <string_view>

namespace {
    struct Options final {
        std::string_view modulePath;
        bool json{};
    };

    void PrintUsage() {
        std::cerr << "Usage: horo-extension-conformance [--json] <native-module>\n";
    }

    [[nodiscard]] std::optional<Options> ParseOptions(const std::span<char *> arguments) {
        Options options;
        for (std::size_t index = 1; index < arguments.size(); ++index) {
            const std::string_view argument{arguments[index]};
            if (argument == "--json") {
                if (options.json)
                    return std::nullopt;
                options.json = true;
            } else if (argument.starts_with('-') || !options.modulePath.empty()) {
                return std::nullopt;
            } else {
                options.modulePath = argument;
            }
        }
        return options.modulePath.empty() ? std::nullopt : std::optional{options};
    }

    void PrintReport(const Horo::Extensions::ExtensionAbiConformanceReport &report, const bool json) {
        const std::string_view code = Horo::Extensions::ExtensionAbiConformanceCodeName(report.code);
        if (!json) {
            std::cout << (report.Passed() ? "passed: " : "failed: ") << code << " status=" << report.status
                      << " registrations=" << report.registrationCount << " query=" << report.queryPresent
                      << " unload=" << report.unloadPresent << " unloadInvoked=" << report.unloadInvoked << '\n';
            return;
        }
        std::cout << "{\"passed\":" << (report.Passed() ? "true" : "false") << ",\"code\":\"" << code << "\",\"status\":" << report.status
                  << ",\"registrations\":" << report.registrationCount << ",\"queryPresent\":" << (report.queryPresent ? "true" : "false")
                  << ",\"unloadPresent\":" << (report.unloadPresent ? "true" : "false")
                  << ",\"unloadInvoked\":" << (report.unloadInvoked ? "true" : "false") << "}\n";
    }

    [[nodiscard]] int RunConformance(const Options &options) {
        const auto report = Horo::Extensions::RunExtensionAbiConformance(options.modulePath);
        PrintReport(report, options.json);
        return report.Passed() ? 0 : 1;
    }
}  // namespace

int main(const int argc, char **argv) {
    if (const auto options = ParseOptions(std::span{argv, static_cast<std::size_t>(argc)}); options.has_value())
        return RunConformance(*options);
    PrintUsage();
    return 2;
}
