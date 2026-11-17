#include "CliInputValidation.h"

#include "Horo/Cli/CliErrors.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace Horo::Cli::Detail {
    namespace {
        [[nodiscard]] std::string InputText(const std::span<const std::byte> bytes) {
            std::string text(bytes.size(), '\0');
            std::ranges::transform(bytes, text.begin(), [](const std::byte value) {
                return static_cast<char>(std::to_integer<unsigned char>(value));
            });
            return text;
        }

        [[nodiscard]] bool ValidJsonDocument(const std::span<const std::byte> bytes) {
            const std::string text = InputText(bytes);
            return text.find('\0') == std::string::npos && !nlohmann::json::parse(text, nullptr, false).is_discarded();
        }

        [[nodiscard]] bool ValidJsonLines(const std::span<const std::byte> bytes, const std::size_t maximumLines) {
            const std::string text = InputText(bytes);
            const std::string_view input{text};
            std::size_t begin = 0;
            std::size_t count = 0;
            while (begin < input.size()) {
                const std::size_t end = input.find('\n', begin);
                std::string_view line = input.substr(begin, end == std::string_view::npos ? input.size() - begin : end - begin);
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);
                if (line.empty() || line.find('\0') != std::string_view::npos)
                    return false;
                ++count;
                if (count > maximumLines || nlohmann::json::parse(line, nullptr, false).is_discarded())
                    return false;
                if (end == std::string_view::npos)
                    break;
                begin = end + 1;
            }
            return count > 0;
        }

        [[nodiscard]] Error InputParseFailure() {
            Error error = MakeError(CliErrors::ParseFailed, "CLI stdin payload is malformed.");
            error.diagnostics.push_back({.code = DiagnosticCode{"cli.stdin_malformed"},
                                         .severity = DiagnosticSeverity::Error,
                                         .message = "Selected stdin does not match the command's declared grammar.",
                                         .location = {.source = "stdin", .line = 1, .column = 1}});
            return error;
        }

        [[nodiscard]] bool IsInputModeAdmitted(const CliCommandDescriptor &command, const CliParserContext &context) noexcept {
            using enum CliStdinPolicy;
            return command.stdinPolicy != None && context.selectedStdinPolicy != None &&
                   context.selectedStdinPolicy == command.stdinPolicy && context.inputReader != nullptr;
        }

        [[nodiscard]] bool IsValidInputPayload(const CliStdinPolicy policy, const std::span<const std::byte> bytes,
                                               const CliParserLimits &limits) {
            using enum CliStdinPolicy;
            switch (policy) {
                case BinaryStream:
                    return true;
                case JsonDocument:
                    return ValidJsonDocument(bytes);
                case JsonLines:
                    return ValidJsonLines(bytes, limits.maximumJsonLines);
                case None:
                    return false;
            }
            return false;
        }
    }  // namespace

    /** @copydoc ReadInput */
    Result<std::optional<CliParsedInput>> ReadInput(const CliCommandDescriptor &command, const CliParserContext &context,
                                                    const CliParserLimits &limits) {
        if (!context.stdinSelected)
            return Result<std::optional<CliParsedInput>>::Success(std::nullopt);
        if (!IsInputModeAdmitted(command, context))
            return Result<std::optional<CliParsedInput>>::Failure(MakeError(CliErrors::InputModeUnsupported));
        Result<std::vector<std::byte>> read = context.inputReader->Read(limits.maximumInputBytes);
        if (read.HasError())
            return Result<std::optional<CliParsedInput>>::Failure(
                WrapError(CliErrors::ParseFailed, read.ErrorValue(), "CLI stdin capture failed."));
        std::vector<std::byte> bytes = std::move(read).Value();
        if (bytes.size() > limits.maximumInputBytes)
            return Result<std::optional<CliParsedInput>>::Failure(MakeError(CliErrors::InputCapacityExceeded));
        if (!IsValidInputPayload(command.stdinPolicy, bytes, limits))
            return Result<std::optional<CliParsedInput>>::Failure(InputParseFailure());
        return Result<std::optional<CliParsedInput>>::Success(CliParsedInput{.policy = command.stdinPolicy, .bytes = std::move(bytes)});
    }
}  // namespace Horo::Cli::Detail
