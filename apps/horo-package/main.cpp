#include "PackageCommand.h"

#include <array>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>

namespace {
    using Horo::PackageCommand::Outcome;

    struct Options final {
        std::string_view packageId;
        std::string_view trust;
        std::string_view signature;
        std::string_view publisher;
        std::string_view keyId;
        std::string_view key;
        std::string_view output;
    };

    [[nodiscard]] std::optional<Options> ParseNamed(const std::span<char *> arguments) {
        Options options;
        for (std::size_t index = 0; index < arguments.size(); index += 2) {
            if (index + 1 >= arguments.size())
                return std::nullopt;
            const std::string_view flag{arguments[index]};
            const std::string_view value{arguments[index + 1]};
            if (value.empty() || value.starts_with("--"))
                return std::nullopt;
            std::string_view *field{};
            if (flag == "--package-id")
                field = &options.packageId;
            else if (flag == "--trust")
                field = &options.trust;
            else if (flag == "--signature")
                field = &options.signature;
            else if (flag == "--publisher")
                field = &options.publisher;
            else if (flag == "--key-id")
                field = &options.keyId;
            else if (flag == "--key")
                field = &options.key;
            else if (flag == "--output")
                field = &options.output;
            else
                return std::nullopt;
            if (!field->empty())
                return std::nullopt;
            *field = value;
        }
        return options;
    }

    void Usage() {
        std::cerr << "Usage: horo-package --version\n"
                     "       horo-package pack <root> <output.horopkg>\n"
                     "       horo-package inspect <archive.horopkg>\n"
                     "       horo-package verify <archive.horopkg> --package-id <id> --trust <trust.json> [--signature <signature.json>]\n"
                     "       horo-package sign <archive.horopkg> --publisher <id> --key-id <id> --key <unencrypted-p256.pem> "
                     "--output <signature.json>\n";
    }

    [[nodiscard]] std::optional<Outcome> Run(const std::span<char *> arguments) {
        using namespace Horo::PackageCommand;
        if (arguments.size() == 2 && std::string_view{arguments[1]} == "--version")
            return Success(std::string{ToolVersion});
        if (arguments.size() < 3)
            return std::nullopt;
        const std::string_view command{arguments[1]};
        if (command == "pack" && arguments.size() == 4)
            return Pack(arguments[2], arguments[3]);
        if (command == "inspect" && arguments.size() == 3)
            return Inspect(arguments[2]);
        if (command == "verify") {
            const auto options = ParseNamed(arguments.subspan(3));
            if (!options || options->packageId.empty() || options->trust.empty() || !options->publisher.empty() ||
                !options->keyId.empty() || !options->key.empty() || !options->output.empty())
                return std::nullopt;
            return Verify(arguments[2], options->signature, options->trust, options->packageId);
        }
        if (command == "sign") {
            const auto options = ParseNamed(arguments.subspan(3));
            if (!options || options->publisher.empty() || options->keyId.empty() || options->key.empty() || options->output.empty() ||
                !options->packageId.empty() || !options->trust.empty() || !options->signature.empty())
                return std::nullopt;
            return Sign(arguments[2], options->key, options->publisher, options->keyId, options->output);
        }
        return std::nullopt;
    }
}  // namespace

int main(const int argc, char **argv) {
    const auto result = Run({argv, static_cast<std::size_t>(argc)});
    if (!result) {
        Usage();
        return 2;
    }
    if (!result->success) {
        std::cerr << result->code << ": " << result->detail << '\n';
        return 1;
    }
    std::cout << result->detail << '\n';
    return 0;
}
