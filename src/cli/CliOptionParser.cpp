#include "Horo/Cli/CliOptionParser.h"

#include "CliInputValidation.h"
#include "Horo/Cli/CliErrors.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <locale>
#include <memory>
#include <sstream>
#include <type_traits>
#include <utility>

namespace Horo::Cli {
    namespace {
        constexpr std::string_view kArgvSource{"argv"};

        class DiagnosticCollector final {
        public:
            explicit DiagnosticCollector(const std::size_t maximum) : maximum_(maximum) {
                diagnostics_.reserve(maximum);
            }

            void Add(std::string code, std::string message, const std::size_t argument) {
                if (diagnostics_.size() == maximum_)
                    return;
                diagnostics_.push_back(
                    {.code = DiagnosticCode{std::move(code)},
                     .severity = DiagnosticSeverity::Error,
                     .message = std::move(message),
                     .location = {.source = std::string{kArgvSource}, .line = 1, .column = static_cast<std::uint32_t>(argument + 1)}});
            }

            [[nodiscard]] bool Empty() const noexcept {
                return diagnostics_.empty();
            }

            [[nodiscard]] std::vector<Diagnostic> Take() {
                return std::move(diagnostics_);
            }

        private:
            std::size_t maximum_{};
            std::vector<Diagnostic> diagnostics_;
        };

        struct OptionState final {
            const CliOptionDescriptor *descriptor{};
            std::vector<CliResolvedValue> values;
            bool invocationSeen{};
        };

        [[nodiscard]] bool MatchesCommand(const CliCommandDescriptor &descriptor,
                                          const std::span<const std::string_view> arguments) noexcept {
            if (descriptor.path.segments.size() > arguments.size())
                return false;
            for (std::size_t index = 0; index < descriptor.path.segments.size(); ++index) {
                if (descriptor.path.segments[index] != arguments[index])
                    return false;
            }
            return true;
        }

        [[nodiscard]] const CliCommandDescriptor *ResolveCommand(const CliCommandRegistry &registry,
                                                                 const std::span<const std::string_view> arguments) noexcept {
            const CliCommandDescriptor *best = nullptr;
            for (const CliCommandDescriptor &descriptor : registry.Commands()) {
                if (MatchesCommand(descriptor, arguments) &&
                    (best == nullptr || best->path.segments.size() < descriptor.path.segments.size()))
                    best = &descriptor;
            }
            return best;
        }

        [[nodiscard]] const CliOptionDescriptor *FindLongOption(const std::span<const OptionState> options,
                                                                const std::string_view name) noexcept {
            const auto found = std::ranges::find(options, name, [](const OptionState &state) -> std::string_view {
                return state.descriptor->name;
            });
            return found == options.end() ? nullptr : found->descriptor;
        }

        [[nodiscard]] OptionState *FindState(const std::span<OptionState> options, const CliOptionDescriptor *descriptor) noexcept {
            const auto found = std::ranges::find(options, descriptor, &OptionState::descriptor);
            return found == options.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] const CliOptionDescriptor *FindShortOption(const std::span<const OptionState> options, const char name) noexcept {
            const auto found = std::ranges::find_if(options, [name](const OptionState &state) {
                return state.descriptor->shortName == name;
            });
            return found == options.end() ? nullptr : found->descriptor;
        }

        [[nodiscard]] bool IsValidLimits(const CliParserLimits &limits) noexcept {
            return limits.maximumArguments > 0 && limits.maximumArgumentBytes > 0 && limits.maximumDiagnostics > 0 &&
                   limits.maximumInputBytes > 0 && limits.maximumJsonLines > 0;
        }

        [[nodiscard]] bool IsIntegerInRange(const std::int64_t value, const CliNumericRange &range) noexcept {
            return (!range.minimumInteger.has_value() || value >= *range.minimumInteger) &&
                   (!range.maximumInteger.has_value() || value <= *range.maximumInteger);
        }

        [[nodiscard]] bool IsNumberInRange(const double value, const CliNumericRange &range) noexcept {
            return (!range.minimumNumber.has_value() || value >= *range.minimumNumber) &&
                   (!range.maximumNumber.has_value() || value <= *range.maximumNumber);
        }

        [[nodiscard]] std::optional<CliParsedValue> InvalidValue(DiagnosticCollector &diagnostics, const std::string_view message,
                                                                 const std::size_t argument) {
            diagnostics.Add("cli.option_value_invalid", std::string{message}, argument);
            return std::nullopt;
        }

        [[nodiscard]] std::optional<CliParsedValue> ParseEnumeration(const std::string_view text, const std::span<const std::string> values,
                                                                     DiagnosticCollector &diagnostics, const std::size_t argument) {
            if (std::ranges::find(values, text) != values.end())
                return CliParsedValue{std::string{text}};
            return InvalidValue(diagnostics, "Option value is not an admitted enumeration alternative.", argument);
        }

        [[nodiscard]] std::optional<CliParsedValue> ParseInteger(const std::string_view text, const CliNumericRange &range,
                                                                 DiagnosticCollector &diagnostics, const std::size_t argument) {
            std::int64_t value{};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error == std::errc{} && end == text.data() + text.size() && IsIntegerInRange(value, range))
                return CliParsedValue{value};
            return InvalidValue(diagnostics, "Option value is not an integer within the declared range.", argument);
        }

        [[nodiscard]] std::optional<CliParsedValue> ParseNumber(const std::string_view text, const CliNumericRange &range,
                                                                DiagnosticCollector &diagnostics, const std::size_t argument) {
            std::istringstream stream{std::string{text}};
            stream.imbue(std::locale::classic());
            double value{};
            stream >> value;
            if (stream && stream.peek() == std::char_traits<char>::eof() && std::isfinite(value) && IsNumberInRange(value, range))
                return CliParsedValue{value};
            return InvalidValue(diagnostics, "Option value is not a finite number within the declared range.", argument);
        }

        [[nodiscard]] std::optional<CliParsedValue> ParsePath(const std::string_view text, const ICliPathNormalizer *normalizer,
                                                              DiagnosticCollector &diagnostics, const std::size_t argument) {
            if (normalizer == nullptr) {
                diagnostics.Add("cli.path_normalizer_unavailable", "Path input requires the host path-normalization adapter.", argument);
                return std::nullopt;
            }
            if (Result<std::string> normalized = normalizer->Normalize(text); normalized.HasValue() && !normalized.Value().empty())
                return CliParsedValue{CliPathValue{std::move(normalized).Value()}};
            diagnostics.Add("cli.path_normalization_failed", "Path input could not be normalized safely.", argument);
            return std::nullopt;
        }

        [[nodiscard]] std::optional<CliParsedValue> ParseValue(const CliOptionValueKind kind, const std::string_view text,
                                                               const std::span<const std::string> enumerationValues,
                                                               const CliNumericRange &range, const ICliPathNormalizer *normalizer,
                                                               DiagnosticCollector &diagnostics, const std::size_t argument) {
            using enum CliOptionValueKind;
            switch (kind) {
                case String:
                    return CliParsedValue{std::string{text}};
                case Enumeration:
                    return ParseEnumeration(text, enumerationValues, diagnostics, argument);
                case SignedInteger:
                    return ParseInteger(text, range, diagnostics, argument);
                case FloatingPoint:
                    return ParseNumber(text, range, diagnostics, argument);
                case Path:
                    return ParsePath(text, normalizer, diagnostics, argument);
                case Flag:
                    return InvalidValue(diagnostics, "A flag cannot consume an explicit value.", argument);
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<CliParsedValue> ParseConfiguredBool(const CliOptionDescriptor &option, const bool value) {
            using enum CliOptionValueKind;
            return option.valueKind == Flag ? std::optional<CliParsedValue>{CliParsedValue{value}} : std::nullopt;
        }

        [[nodiscard]] std::optional<CliParsedValue> ParseConfiguredInteger(const CliOptionDescriptor &option, const std::int64_t value) {
            using enum CliOptionValueKind;
            if (option.valueKind == SignedInteger && IsIntegerInRange(value, option.numericRange))
                return CliParsedValue{value};
            if (const auto number = static_cast<double>(value);
                option.valueKind == FloatingPoint && IsNumberInRange(number, option.numericRange))
                return CliParsedValue{number};
            return std::nullopt;
        }

        [[nodiscard]] std::optional<CliParsedValue> ParseConfiguredString(const CliOptionDescriptor &option, const std::string &value,
                                                                          const ICliPathNormalizer *normalizer,
                                                                          DiagnosticCollector &diagnostics) {
            return ParseValue(option.valueKind, value, option.enumerationValues, option.numericRange, normalizer, diagnostics, 0);
        }

        struct ConfiguredValueParse final {
            std::optional<CliParsedValue> value;
            bool incompatible{};
        };

        [[nodiscard]] std::optional<CliParsedValue> ParseConfigurationValue(const CliOptionDescriptor &option, const SettingValue &value,
                                                                            const ICliPathNormalizer *normalizer,
                                                                            DiagnosticCollector &diagnostics) {
            using enum CliOptionValueKind;
            const ConfiguredValueParse parsed =
                std::visit([&option, normalizer, &diagnostics]<typename Value>(const Value &configured) -> ConfiguredValueParse {
                if constexpr (std::is_same_v<Value, bool>) {
                    auto result = ParseConfiguredBool(option, configured);
                    const bool incompatible = !result.has_value();
                    return {.value = std::move(result), .incompatible = incompatible};
                } else if constexpr (std::is_same_v<Value, std::int64_t>) {
                    auto result = ParseConfiguredInteger(option, configured);
                    const bool incompatible = !result.has_value();
                    return {.value = std::move(result), .incompatible = incompatible};
                } else if constexpr (std::is_same_v<Value, std::string>) {
                    const bool incompatible = option.valueKind == Flag;
                    return {.value = incompatible ? std::nullopt : ParseConfiguredString(option, configured, normalizer, diagnostics),
                            .incompatible = incompatible};
                } else {
                    return {.incompatible = true};
                }
            }, value);
            if (parsed.incompatible)
                diagnostics.Add("cli.configuration_value_incompatible",
                                "Resolved configuration value is incompatible with the command option schema.", 0);
            return parsed.value;
        }

        [[nodiscard]] Error ParseFailure(DiagnosticCollector &diagnostics) {
            Error error = MakeError(CliErrors::ParseFailed);
            error.diagnostics = diagnostics.Take();
            return error;
        }

        [[nodiscard]] Error InputParseFailure() {
            Error error = MakeError(CliErrors::ParseFailed, "CLI stdin payload is malformed.");
            error.diagnostics.push_back({.code = DiagnosticCode{"cli.stdin_malformed"},
                                         .severity = DiagnosticSeverity::Error,
                                         .message = "Selected stdin does not match the command's declared grammar.",
                                         .location = {.source = "stdin", .line = 1, .column = 1}});
            return error;
        }

        [[nodiscard]] Result<std::vector<OptionState>> BuildOptionStates(const CliCommandDescriptor &command,
                                                                         DiagnosticCollector &diagnostics) {
            std::vector<OptionState> states;
            const std::span common = CliOptionParser::CommonOptions();
            states.reserve(common.size() + command.options.size());
            for (const CliOptionDescriptor &option : common)
                states.push_back({.descriptor = &option});
            for (const CliOptionDescriptor &option : command.options) {
                const bool collision = std::ranges::any_of(states, [&option](const OptionState &state) {
                    return state.descriptor->name == option.name ||
                           (state.descriptor->shortName.has_value() && state.descriptor->shortName == option.shortName);
                });
                if (collision)
                    diagnostics.Add("cli.option_source_conflict", "Command option conflicts with a shared option identity.", 0);
                else
                    states.push_back({.descriptor = &option});
            }
            if (!diagnostics.Empty())
                return Result<std::vector<OptionState>>::Failure(ParseFailure(diagnostics));
            std::ranges::sort(states, {}, [](const OptionState &state) -> const std::string & {
                return state.descriptor->name;
            });
            return Result<std::vector<OptionState>>::Success(std::move(states));
        }

        void RecordOption(OptionState &state, const std::optional<CliParsedValue> &value, DiagnosticCollector &diagnostics,
                          const std::size_t argument) {
            if (state.invocationSeen && !state.descriptor->repeatable) {
                diagnostics.Add("cli.option_source_conflict", "Non-repeatable option was provided more than once.", argument);
                return;
            }
            state.invocationSeen = true;
            if (value.has_value())
                state.values.push_back({.value = *value, .source = CliValueSource::Invocation});
        }

        void ParseOptionValue(const std::span<const std::string_view> arguments, std::size_t &index,
                              const std::optional<std::string_view> inlineValue, OptionState &state, DiagnosticCollector &diagnostics,
                              const ICliPathNormalizer *normalizer) {
            using enum CliOptionValueKind;
            const CliOptionDescriptor &descriptor = *state.descriptor;
            if (descriptor.valueKind == Flag) {
                const std::optional<CliParsedValue> value =
                    inlineValue.has_value() ? ParseValue(descriptor.valueKind, *inlineValue, {}, {}, normalizer, diagnostics, index)
                                            : std::optional<CliParsedValue>{CliParsedValue{true}};
                RecordOption(state, value, diagnostics, index);
                return;
            }
            std::size_t valueIndex = index;
            std::optional<std::string_view> value = inlineValue;
            if (!value.has_value() && index + 1 < arguments.size() && arguments[index + 1] != "--") {
                value = arguments[++index];
                valueIndex = index;
            }
            if (!value.has_value()) {
                diagnostics.Add("cli.option_value_missing", "Option requires a value.", index);
                return;
            }
            RecordOption(state,
                         ParseValue(descriptor.valueKind, *value, descriptor.enumerationValues, descriptor.numericRange, normalizer,
                                    diagnostics, valueIndex),
                         diagnostics, valueIndex);
        }

        void ParseResolvedOption(const std::span<const std::string_view> arguments, std::size_t &index,
                                 const std::optional<std::string_view> inlineValue, const CliOptionDescriptor *descriptor,
                                 const std::span<OptionState> states, DiagnosticCollector &diagnostics,
                                 const ICliPathNormalizer *normalizer) {
            if (descriptor == nullptr) {
                diagnostics.Add("cli.option_unknown", "Unknown option.", index);
                return;
            }
            ParseOptionValue(arguments, index, inlineValue, *FindState(states, descriptor), diagnostics, normalizer);
        }

        [[nodiscard]] std::optional<std::string_view> InlineOptionValue(const std::string_view token,
                                                                        const std::size_t separator) noexcept {
            return separator == std::string_view::npos ? std::nullopt : std::optional{token.substr(separator + 1)};
        }

        void ParseLongOption(const std::span<const std::string_view> arguments, std::size_t &index, const std::string_view token,
                             const std::span<OptionState> states, DiagnosticCollector &diagnostics, const ICliPathNormalizer *normalizer) {
            const std::size_t separator = token.find('=');
            const std::string_view name = token.substr(2, separator == std::string_view::npos ? token.size() - 2 : separator - 2);
            const CliOptionDescriptor *descriptor = FindLongOption(states, name);
            ParseResolvedOption(arguments, index, InlineOptionValue(token, separator), descriptor, states, diagnostics, normalizer);
        }

        void ParseShortOption(const std::span<const std::string_view> arguments, std::size_t &index, const std::string_view token,
                              const std::span<OptionState> states, DiagnosticCollector &diagnostics, const ICliPathNormalizer *normalizer) {
            const std::size_t separator = token.find('=');
            if (token.size() < 2 || (separator == std::string_view::npos && token.size() != 2) ||
                (separator != std::string_view::npos && separator != 2)) {
                diagnostics.Add("cli.option_unknown", "Short options cannot be grouped.", index);
                return;
            }
            const CliOptionDescriptor *descriptor = FindShortOption(states, token[1]);
            ParseResolvedOption(arguments, index, InlineOptionValue(token, separator), descriptor, states, diagnostics, normalizer);
        }

        void AppendResolvedValue(OptionState &state, std::optional<CliParsedValue> parsed, const CliValueSource source,
                                 const std::optional<ConfigurationSource> configurationSource = std::nullopt) {
            if (parsed.has_value())
                state.values.push_back({.value = std::move(*parsed), .source = source, .configurationSource = configurationSource});
        }

        void ResolveFallbacks(const std::span<OptionState> states, const ConfigurationSnapshot *configuration,
                              DiagnosticCollector &diagnostics, const ICliPathNormalizer *normalizer) {
            for (OptionState &state : states) {
                const CliOptionDescriptor &option = *state.descriptor;
                if (!state.values.empty())
                    continue;
                if (configuration != nullptr && option.configurationKey.has_value()) {
                    const ResolvedSetting *resolved = configuration->FindResolved(SettingKey{*option.configurationKey});
                    if (resolved != nullptr) {
                        AppendResolvedValue(state, ParseConfigurationValue(option, resolved->value, normalizer, diagnostics),
                                            CliValueSource::Configuration, resolved->source);
                        continue;
                    }
                }
                if (option.defaultValue.has_value()) {
                    AppendResolvedValue(state,
                                        ParseValue(option.valueKind, *option.defaultValue, option.enumerationValues, option.numericRange,
                                                   normalizer, diagnostics, 0),
                                        CliValueSource::DescriptorDefault);
                } else if (option.required) {
                    diagnostics.Add("cli.option_required", "Required option is missing.", 0);
                }
            }
        }

        void AppendPositionalValue(const std::span<const std::string_view> values, const std::size_t current,
                                   const CliPositionalDescriptor &descriptor, CliCommandRequest &request, DiagnosticCollector &diagnostics,
                                   const ICliPathNormalizer *normalizer) {
            if (auto parsed = ParseValue(descriptor.valueKind, values[current], descriptor.enumerationValues, descriptor.numericRange,
                                         normalizer, diagnostics, current);
                parsed.has_value())
                request.positionals.push_back({.name = descriptor.name, .value = std::move(*parsed)});
        }

        void ParsePositionals(const std::span<const std::string_view> values, const CliCommandDescriptor &command,
                              CliCommandRequest &request, DiagnosticCollector &diagnostics, const ICliPathNormalizer *normalizer) {
            std::size_t valueIndex = 0;
            for (const CliPositionalDescriptor &descriptor : command.positionals) {
                const std::size_t remaining = values.size() - valueIndex;
                const std::size_t count = descriptor.repeatable ? remaining : std::min<std::size_t>(remaining, 1);
                if (count == 0 && descriptor.required) {
                    diagnostics.Add("cli.positional_required", "Required positional input is missing.", valueIndex);
                    continue;
                }
                for (std::size_t occurrence = 0; occurrence < count; ++occurrence) {
                    const std::size_t current = valueIndex++;
                    AppendPositionalValue(values, current, descriptor, request, diagnostics, normalizer);
                }
            }
            if (valueIndex != values.size())
                diagnostics.Add("cli.positional_unexpected", "Unexpected positional input.", valueIndex);
        }

        [[nodiscard]] const OptionState *FindOptionState(const std::span<const OptionState> states, const std::string_view name) noexcept {
            const auto found = std::ranges::find(states, name, [](const OptionState &state) -> std::string_view {
                return state.descriptor->name;
            });
            return found == states.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] bool OptionIsTrue(const std::span<const OptionState> states, const std::string_view name) noexcept {
            const OptionState *state = FindOptionState(states, name);
            return state != nullptr && !state->values.empty() && std::holds_alternative<bool>(state->values.back().value) &&
                   std::get<bool>(state->values.back().value);
        }

        [[nodiscard]] bool OptionHasValue(const std::span<const OptionState> states, const std::string_view name) noexcept {
            const OptionState *state = FindOptionState(states, name);
            return state != nullptr && !state->values.empty();
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
                    return Detail::ValidJsonDocument(bytes);
                case JsonLines:
                    return Detail::ValidJsonLines(bytes, limits.maximumJsonLines);
                case None:
                    return false;
            }
            return false;
        }

        [[nodiscard]] Result<void> ValidateArgumentBounds(const std::span<const std::string_view> arguments,
                                                          const CliParserLimits &limits) {
            if (!IsValidLimits(limits))
                return Result<void>::Failure(MakeError(CliErrors::ParserPolicyInvalid));
            const bool oversized =
                arguments.size() > limits.maximumArguments || std::ranges::any_of(arguments, [&limits](const std::string_view argument) {
                return argument.size() > limits.maximumArgumentBytes;
            });
            return oversized ? Result<void>::Failure(MakeError(CliErrors::InputCapacityExceeded)) : Result<void>::Success();
        }

        void ParseArguments(const std::span<const std::string_view> arguments, const std::size_t firstArgument,
                            const std::span<OptionState> states, std::vector<std::string_view> &positionals,
                            DiagnosticCollector &diagnostics, const ICliPathNormalizer *normalizer) {
            bool optionsTerminated = false;
            std::size_t index = firstArgument;
            bool consumedAsOptionValue = false;
            for (const std::string_view token : arguments.subspan(firstArgument)) {
                if (consumedAsOptionValue) {
                    consumedAsOptionValue = false;
                } else {
                    std::size_t parsedIndex = index;
                    if (!optionsTerminated && token == "--") {
                        optionsTerminated = true;
                    } else if (!optionsTerminated && token.starts_with("--"))
                        ParseLongOption(arguments, parsedIndex, token, states, diagnostics, normalizer);
                    else if (!optionsTerminated && token.starts_with('-') && token != "-")
                        ParseShortOption(arguments, parsedIndex, token, states, diagnostics, normalizer);
                    else
                        positionals.push_back(token);
                    consumedAsOptionValue = parsedIndex != index;
                }
                ++index;
            }
        }

        [[nodiscard]] Result<void> ApplyInteractionPolicy(const CliCommandDescriptor &command, const std::span<const OptionState> states,
                                                          const CliParserContext &context, CliCommandRequest &request) {
            const bool nonInteractive = OptionIsTrue(states, "non-interactive") || !context.terminalAvailable;
            request.interactive = command.interactive != CliInteractivePolicy::Forbidden && !nonInteractive;
            if (command.interactive == CliInteractivePolicy::Required && nonInteractive &&
                !OptionHasValue(states, *command.interactiveAlternativeOption))
                return Result<void>::Failure(MakeError(CliErrors::InteractiveInputUnavailable));
            return Result<void>::Success();
        }

        void AppendParsedOptions(const std::span<OptionState> states, CliCommandRequest &request) {
            request.options.reserve(states.size());
            for (OptionState &state : states) {
                if (!state.values.empty())
                    request.options.push_back({.name = state.descriptor->name, .values = std::move(state.values)});
            }
        }

        [[nodiscard]] Result<std::optional<CliParsedInput>> ReadInput(const CliCommandDescriptor &command, const CliParserContext &context,
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
    }  // namespace

    /** @copydoc CliOptionParser::Parse */
    Result<CliCommandRequest> CliOptionParser::Parse(const std::span<const std::string_view> arguments, const CliCommandRegistry &registry,
                                                     const CliParserContext &context, const CliParserLimits &limits) {
        if (const Result<void> bounds = ValidateArgumentBounds(arguments, limits); bounds.HasError())
            return Result<CliCommandRequest>::Failure(bounds.ErrorValue());
        const CliCommandDescriptor *command = ResolveCommand(registry, arguments);
        if (command == nullptr)
            return Result<CliCommandRequest>::Failure(MakeError(CliErrors::CommandUnknown));
        DiagnosticCollector diagnostics{limits.maximumDiagnostics};
        Result<std::vector<OptionState>> built = BuildOptionStates(*command, diagnostics);
        if (built.HasError())
            return Result<CliCommandRequest>::Failure(built.ErrorValue());
        std::vector<OptionState> states = std::move(built).Value();
        std::vector<std::string_view> positionals;
        positionals.reserve(command->positionals.size());
        ParseArguments(arguments, command->path.segments.size(), states, positionals, diagnostics, context.pathNormalizer);
        CliCommandRequest request{.command = command->path};
        ParsePositionals(positionals, *command, request, diagnostics, context.pathNormalizer);
        ResolveFallbacks(states, context.configuration, diagnostics, context.pathNormalizer);
        if (const Result<void> interaction = ApplyInteractionPolicy(*command, states, context, request); interaction.HasError())
            return Result<CliCommandRequest>::Failure(interaction.ErrorValue());
        if (!diagnostics.Empty())
            return Result<CliCommandRequest>::Failure(ParseFailure(diagnostics));
        Result<std::optional<CliParsedInput>> input = ReadInput(*command, context, limits);
        if (input.HasError())
            return Result<CliCommandRequest>::Failure(input.ErrorValue());
        request.input = std::move(input).Value();
        AppendParsedOptions(states, request);
        return Result<CliCommandRequest>::Success(std::move(request));
    }
}  // namespace Horo::Cli
