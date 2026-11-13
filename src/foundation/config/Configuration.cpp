#include "Horo/Foundation/Configuration.h"

#include "../FoundationErrors.h"
#include "../json/JsonUtils.h"
#include "Horo/Foundation/Assertions.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <locale>
#include <nlohmann/json.hpp>
#include <ranges>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <unordered_set>

namespace Horo {
    namespace {
        using Json = nlohmann::json;

        constexpr std::uint32_t kConfigurationDocumentVersion = 1;
        constexpr std::string_view kUnknownKeyDiagnostic = "configuration.unknown_key";
        constexpr std::string_view kForbiddenSourceDiagnostic = "configuration.source_forbidden";
        constexpr std::string_view kInvalidValueDiagnostic = "configuration.value_invalid";
        constexpr std::string_view kInputLimitDiagnostic = "configuration.input_limit_exceeded";
        constexpr std::string_view kSecretValueDiagnostic = "configuration.secret_value_forbidden";
        constexpr std::string_view kEnvironmentBindingDiagnostic = "configuration.environment_binding_invalid";
        constexpr std::string_view kDocumentDiagnostic = "configuration.document_invalid";

        struct SourceView {
            ConfigurationSource source;
            const ConfigurationSourceMap *values;
        };

        struct TransparentStringHash {
            using is_transparent = void;

            [[nodiscard]] std::size_t operator()(const std::string_view value) const noexcept {
                return std::hash<std::string_view>{}(value);
            }
        };

        using StringSet = std::unordered_set<std::string, TransparentStringHash, std::equal_to<>>;

        /** @brief Allows host limits to tighten the canonical resource envelope. */
        [[nodiscard]] ConfigurationLimits BoundedLimits(const ConfigurationLimits &requested) noexcept {
            const ConfigurationLimits defaults;
            return {.maximumKeysPerSource = std::min(requested.maximumKeysPerSource, defaults.maximumKeysPerSource),
                    .maximumKeyBytes = std::min(requested.maximumKeyBytes, defaults.maximumKeyBytes),
                    .maximumStringValueBytes = std::min(requested.maximumStringValueBytes, defaults.maximumStringValueBytes),
                    .maximumSourceLocationBytes = std::min(requested.maximumSourceLocationBytes, defaults.maximumSourceLocationBytes),
                    .maximumDocumentBytes = std::min(requested.maximumDocumentBytes, defaults.maximumDocumentBytes),
                    .maximumEnvironmentBindings = std::min(requested.maximumEnvironmentBindings, defaults.maximumEnvironmentBindings),
                    .maximumNestingDepth = std::min(requested.maximumNestingDepth, defaults.maximumNestingDepth)};
        }

        [[nodiscard]] std::array<SourceView, 6> SourceViews(const ConfigurationResolutionRequest &request) noexcept {
            using enum ConfigurationSource;
            return {{{Invocation, &request.invocation},
                     {Environment, &request.environment},
                     {Session, &request.session},
                     {Project, &request.project},
                     {User, &request.user},
                     {PackagedProfile, &request.packagedProfile}}};
        }

        /** @brief Selects the explicitly authored source without changing source precedence. */
        [[nodiscard]] ConfigurationSourceMap *DraftSource(ConfigurationResolutionRequest &request,
                                                          const ConfigurationSource source) noexcept {
            using enum ConfigurationSource;
            switch (source) {
                case Invocation:
                    return &request.invocation;
                case Environment:
                    return &request.environment;
                case Session:
                    return &request.session;
                case Project:
                    return &request.project;
                case User:
                    return &request.user;
                case PackagedProfile:
                    return &request.packagedProfile;
                case SchemaDefault:
                    return nullptr;
            }
            return nullptr;
        }

        [[nodiscard]] std::string_view SettingTypeName(const SettingValueType type) noexcept {
            using enum SettingValueType;
            switch (type) {
                case Boolean:
                    return "boolean";
                case Integer:
                    return "integer";
                case String:
                    return "string";
                case CredentialReference:
                    return "credential-reference";
            }
            return "unknown";
        }

        [[nodiscard]] std::string_view SettingValueTypeName(const SettingValue &value) noexcept {
            if (std::holds_alternative<bool>(value))
                return "boolean";
            if (std::holds_alternative<std::int64_t>(value))
                return "integer";
            if (std::holds_alternative<CredentialReference>(value))
                return "credential-reference";
            return "string";
        }

        [[nodiscard]] std::string_view SourceName(const ConfigurationSource source) noexcept {
            using enum ConfigurationSource;
            switch (source) {
                case Invocation:
                    return "invocation";
                case Environment:
                    return "environment";
                case Session:
                    return "session";
                case Project:
                    return "project";
                case User:
                    return "user";
                case PackagedProfile:
                    return "packaged-profile";
                case SchemaDefault:
                    return "schema-default";
            }
            return "unknown";
        }

        [[nodiscard]] ConfigurationSourceMask SourceBit(const ConfigurationSource source) noexcept {
            using enum ConfigurationSource;
            switch (source) {
                case Invocation:
                    return ConfigurationSourceMask::Invocation;
                case Environment:
                    return ConfigurationSourceMask::Environment;
                case Session:
                    return ConfigurationSourceMask::Session;
                case Project:
                    return ConfigurationSourceMask::Project;
                case User:
                    return ConfigurationSourceMask::User;
                case PackagedProfile:
                    return ConfigurationSourceMask::PackagedProfile;
                case SchemaDefault:
                    return ConfigurationSourceMask::None;
            }
            return ConfigurationSourceMask::None;
        }

        [[nodiscard]] ConfigurationSourcePolicy LegacyPolicy(const SettingScope scope) noexcept {
            using enum ConfigurationSourceMask;
            switch (scope) {
                case SettingScope::Engine:
                    return {.allowedSources = Invocation | Environment | PackagedProfile};
                case SettingScope::User:
                    return {.allowedSources = Invocation | Environment | Session | User | PackagedProfile};
                case SettingScope::Project:
                    return {.allowedSources = Invocation | Environment | Session | Project | PackagedProfile};
                case SettingScope::Workspace:
                    return {.allowedSources = Invocation | Environment | Session};
                case SettingScope::Session:
                    return {.allowedSources = Invocation | Environment | Session};
                case SettingScope::Invocation:
                    return {.allowedSources = Invocation};
            }
            return {};
        }

        [[nodiscard]] ConfigurationSourcePolicy PolicyFor(const SettingDescriptor &descriptor) noexcept {
            return descriptor.sourcePolicy.value_or(LegacyPolicy(descriptor.scope));
        }

        /** @brief Rejects invalid enum and source-mask metadata before it reaches resolution or presentation. */
        [[nodiscard]] bool ValidDescriptorPolicy(const SettingDescriptor &descriptor) noexcept {
            constexpr auto allSources = ConfigurationSourceMask::Invocation | ConfigurationSourceMask::Environment |
                                        ConfigurationSourceMask::Session | ConfigurationSourceMask::Project |
                                        ConfigurationSourceMask::User | ConfigurationSourceMask::PackagedProfile;
            return static_cast<std::uint8_t>(descriptor.scope) <= static_cast<std::uint8_t>(SettingScope::Invocation) &&
                   static_cast<std::uint8_t>(descriptor.reloadPolicy) <= static_cast<std::uint8_t>(ReloadPolicy::ProcessRestart) &&
                   static_cast<std::uint8_t>(descriptor.sensitivity) <= static_cast<std::uint8_t>(SettingSensitivity::SecretReference) &&
                   (static_cast<std::uint16_t>(PolicyFor(descriptor).allowedSources) & ~static_cast<std::uint16_t>(allSources)) == 0;
        }

        [[nodiscard]] bool Allows(const ConfigurationSourcePolicy &policy, const ConfigurationSource source) noexcept {
            return (static_cast<std::uint16_t>(policy.allowedSources) & static_cast<std::uint16_t>(SourceBit(source))) != 0U;
        }

        /** @brief Keeps identities portable and unambiguous in diagnostics and native control IDs. */
        [[nodiscard]] bool ValidSettingKey(const std::string_view key) noexcept {
            return !key.empty() && key.front() != '.' && key.back() != '.' && key.find("..") == std::string_view::npos &&
                   std::ranges::all_of(key, [](const char character) {
                return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '.';
            });
        }

        [[nodiscard]] Error InvalidConfigurationValue(const SettingKey &key, const SettingValue &value,
                                                      const SettingDescriptor *descriptor) {
            if (!ValidSettingKey(key.Value()))
                return MakeError(ConfigurationErrors::ValueInvalid, "Configuration key identity is malformed.");
            if (descriptor == nullptr)
                return MakeError(ConfigurationErrors::ValueInvalid,
                                 "Configuration key '" + key.Value() + "' is not registered in the active schema.");
            return MakeError(ConfigurationErrors::ValueInvalid, "Configuration key '" + key.Value() + "' expects " +
                                                                    std::string{SettingTypeName(descriptor->type)} + " but received " +
                                                                    std::string{SettingValueTypeName(value)} + ".");
        }

        [[nodiscard]] std::string EscapeJsonString(const std::string_view value) {
            std::ostringstream escaped;
            for (const unsigned char character : value) {
                switch (character) {
                    case '"':
                        escaped << R"(\")";
                        break;
                    case '\\':
                        escaped << R"(\\)";
                        break;
                    case '\b':
                        escaped << "\\b";
                        break;
                    case '\f':
                        escaped << "\\f";
                        break;
                    case '\n':
                        escaped << "\\n";
                        break;
                    case '\r':
                        escaped << "\\r";
                        break;
                    case '\t':
                        escaped << "\\t";
                        break;
                    default:
                        if (character < 0x20) {
                            static constexpr char hex[] = "0123456789abcdef";
                            escaped << "\\u00" << hex[static_cast<std::size_t>(character) >> 4]
                                    << hex[static_cast<std::size_t>(character) & 0x0f];
                        } else {
                            escaped << static_cast<char>(character);
                        }
                }
            }
            return escaped.str();
        }

        [[nodiscard]] Diagnostic Finding(const std::string_view code, std::string message, const ConfigurationSource source,
                                         const ConfigurationInputValue *input = nullptr) {
            SourceLocation location{.source = std::string{SourceName(source)}};
            if (input != nullptr && input->location.has_value())
                location = *input->location;
            return {.code = DiagnosticCode{std::string{code}},
                    .severity = DiagnosticSeverity::Error,
                    .message = std::move(message),
                    .location = std::move(location)};
        }

        [[nodiscard]] Error ResolutionFailure(std::vector<Diagnostic> findings) {
            Error error = MakeError(ConfigurationErrors::ResolutionFailed);
            error.diagnostics = std::move(findings);
            return error;
        }

        [[nodiscard]] bool StringWithinLimit(const SettingValue &value, const ConfigurationLimits &limits) noexcept {
            const auto *text = std::get_if<std::string>(&value);
            return text == nullptr || text->size() <= limits.maximumStringValueBytes;
        }

        /** @brief Rejects invalid scalar encoding before strings can enter a snapshot or JSON serialization. */
        [[nodiscard]] bool ValidStringEncoding(const SettingValue &value) noexcept {
            const auto *text = std::get_if<std::string>(&value);
            return text == nullptr || (text->find('\0') == std::string::npos && IsValidUtf8ScalarSequence(*text));
        }

        /** @brief Applies inert owner-declared validation equally to defaults, shadowed sources and draft previews. */
        [[nodiscard]] bool MatchesValidation(const SettingDescriptor &descriptor, const SettingValue &value) {
            if (const auto *range = std::get_if<SettingIntegerRange>(&descriptor.validation)) {
                const auto *number = std::get_if<std::int64_t>(&value);
                return number != nullptr && *number >= range->minimum && *number <= range->maximum;
            }
            if (const auto *rules = std::get_if<SettingStringRules>(&descriptor.validation)) {
                const auto *text = std::get_if<std::string>(&value);
                return text != nullptr && text->size() >= rules->minimumBytes && text->size() <= rules->maximumBytes &&
                       text->starts_with(rules->requiredPrefix) &&
                       (rules->allowedCharacters.empty() ||
                        std::string_view{*text}.substr(rules->requiredPrefix.size()).find_first_not_of(rules->allowedCharacters) ==
                            std::string::npos) &&
                       (rules->choices.empty() || std::ranges::find(rules->choices, *text) != rules->choices.end());
            }
            return true;
        }

        /** @brief Bounds rule metadata before any descriptor is retained. */
        [[nodiscard]] bool ValidValidation(const SettingDescriptor &descriptor) {
            if (const auto *range = std::get_if<SettingIntegerRange>(&descriptor.validation))
                return descriptor.type == SettingValueType::Integer && range->minimum <= range->maximum;
            const auto *rules = std::get_if<SettingStringRules>(&descriptor.validation);
            if (rules == nullptr)
                return true;
            return descriptor.type == SettingValueType::String && rules->minimumBytes <= rules->maximumBytes &&
                   rules->maximumBytes <= ConfigurationLimits{}.maximumStringValueBytes && rules->requiredPrefix.size() <= 256 &&
                   rules->allowedCharacters.size() <= 256 && rules->choices.size() <= 64 &&
                   IsValidUtf8ScalarSequence(rules->requiredPrefix) && IsValidUtf8ScalarSequence(rules->allowedCharacters) &&
                   std::ranges::all_of(rules->choices, [&](const std::string &choice) {
                return choice.size() <= rules->maximumBytes && IsValidUtf8ScalarSequence(choice);
            });
        }

        using SourceEntry = ConfigurationSourceMap::value_type;

        [[nodiscard]] std::vector<const SourceEntry *> SortedEntries(const ConfigurationSourceMap &values, const std::size_t maximum) {
            std::vector<const SourceEntry *> sorted;
            sorted.reserve(std::min(values.size(), maximum));
            for (const auto &entry : values) {
                if (sorted.size() == maximum)
                    break;
                sorted.push_back(&entry);
            }
            std::ranges::sort(sorted, {}, [](const SourceEntry *entry) -> const std::string & {
                return entry->first.Value();
            });
            return sorted;
        }

        [[nodiscard]] std::optional<Diagnostic> ValidateSourceEntry(const ConfigurationSchema &schema, const SourceView source,
                                                                    const SourceEntry &entry, const ConfigurationLimits &limits) {
            const auto &[key, input] = entry;
            if (input.location.has_value() && input.location->source.size() > limits.maximumSourceLocationBytes)
                return Finding(kInputLimitDiagnostic, "Configuration source location exceeds the permitted byte length.", source.source);
            if (key.Value().size() > limits.maximumKeyBytes)
                return Finding(kInputLimitDiagnostic, "Configuration key exceeds the permitted byte length.", source.source, &input);
            if (!ValidSettingKey(key.Value()))
                return Finding(kInvalidValueDiagnostic, "Configuration key identity is malformed.", source.source);
            const SettingDescriptor *descriptor = schema.FindDescriptor(key);
            if (descriptor == nullptr)
                return Finding(kUnknownKeyDiagnostic, "Configuration key '" + key.Value() + "' is not declared.", source.source, &input);
            if (!Allows(PolicyFor(*descriptor), source.source))
                return Finding(kForbiddenSourceDiagnostic,
                               "Configuration key '" + key.Value() + "' is not legal in " + std::string{SourceName(source.source)} + ".",
                               source.source, &input);
            if (descriptor->sensitivity == SettingSensitivity::SecretReference && !std::holds_alternative<CredentialReference>(input.value))
                return Finding(kSecretValueDiagnostic, "Configuration key '" + key.Value() + "' requires a typed credential reference.",
                               source.source, &input);
            if (!ConfigurationSchema::MatchesType(descriptor->type, input.value))
                return Finding(kInvalidValueDiagnostic,
                               "Configuration key '" + key.Value() + "' expects " + std::string{SettingTypeName(descriptor->type)} + ".",
                               source.source, &input);
            if (!StringWithinLimit(input.value, limits))
                return Finding(kInputLimitDiagnostic, "Configuration string value exceeds the permitted byte length.", source.source,
                               &input);
            if (!ValidStringEncoding(input.value))
                return Finding(kInvalidValueDiagnostic, "Configuration string has invalid UTF-8 encoding.", source.source, &input);
            if (!MatchesValidation(*descriptor, input.value))
                return Finding(kInvalidValueDiagnostic, "Configuration key '" + key.Value() + "' violates its declared validation rule.",
                               source.source, &input);
            return std::nullopt;
        }

        void ValidateSource(const ConfigurationSchema &schema, const SourceView source, const ConfigurationLimits &limits,
                            std::vector<Diagnostic> &findings) {
            if (source.values->size() > limits.maximumKeysPerSource) {
                findings.push_back(Finding(kInputLimitDiagnostic,
                                           std::string{SourceName(source.source)} + " contains more than the permitted number of keys.",
                                           source.source));
                return;
            }
            for (const SourceEntry *entry : SortedEntries(*source.values, limits.maximumKeysPerSource)) {
                if (std::optional<Diagnostic> finding = ValidateSourceEntry(schema, source, *entry, limits); finding.has_value())
                    findings.push_back(std::move(*finding));
            }
        }

        [[nodiscard]] const ConfigurationInputValue *FindInput(const ConfigurationSourceMap &values, const SettingKey &key) noexcept {
            const auto found = values.find(key);
            return found == values.end() ? nullptr : &found->second;
        }

        [[nodiscard]] ResolvedSetting ResolveSetting(const SettingDescriptor &descriptor,
                                                     const std::shared_ptr<const ConfigurationResolutionRequest> &request,
                                                     const std::shared_ptr<const ConfigurationSourceMap> &defaults) {
            const ConfigurationSourcePolicy policy = PolicyFor(descriptor);
            std::array<SourceView, 6> sources = SourceViews(*request);
            if (policy.sessionOverridesEnvironment)
                std::swap(sources[1], sources[2]);
            std::vector<ConfigurationCandidate> candidates;
            for (const SourceView source : sources) {
                if (!Allows(policy, source.source))
                    continue;
                if (const ConfigurationInputValue *input = FindInput(*source.values, descriptor.key); input != nullptr)
                    candidates.emplace_back(source.source, std::shared_ptr<const ConfigurationInputValue>{request, input});
            }
            candidates.emplace_back(ConfigurationSource::SchemaDefault,
                                    std::shared_ptr<const ConfigurationInputValue>{defaults, &defaults->at(descriptor.key)});
            return {.value = candidates.front().Value(),
                    .source = candidates.front().Source(),
                    .location = candidates.front().Location(),
                    .sensitivity = descriptor.sensitivity,
                    .candidates = std::move(candidates)};
        }

        [[nodiscard]] Result<SettingValue> ParseEnvironmentValue(const SettingDescriptor &descriptor, const std::string_view raw) {
            if (descriptor.type == SettingValueType::CredentialReference) {
                auto reference = CredentialReference::Parse(raw);
                if (reference.HasError())
                    return Result<SettingValue>::Failure(reference.ErrorValue());
                return Result<SettingValue>::Success(std::move(reference).Value());
            }
            if (raw.empty() || raw.find('\0') != std::string_view::npos)
                return Result<SettingValue>::Failure(MakeError(ConfigurationErrors::ValueInvalid));
            if (descriptor.type == SettingValueType::Boolean) {
                if (raw == "true")
                    return Result<SettingValue>::Success(true);
                if (raw == "false")
                    return Result<SettingValue>::Success(false);
                return Result<SettingValue>::Failure(MakeError(ConfigurationErrors::ValueInvalid));
            }
            if (descriptor.type == SettingValueType::Integer) {
                std::int64_t value{};
                const auto [position, error] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
                if (error != std::errc{} || position != raw.data() + raw.size())
                    return Result<SettingValue>::Failure(MakeError(ConfigurationErrors::ValueInvalid));
                return Result<SettingValue>::Success(value);
            }
            if (!IsValidUtf8ScalarSequence(raw))
                return Result<SettingValue>::Failure(MakeError(ConfigurationErrors::ValueInvalid));
            return Result<SettingValue>::Success(std::string{raw});
        }

        [[nodiscard]] Result<ConfigurationSourceMap> DocumentFailure(const std::string_view message, std::string sourceName,
                                                                     const std::string_view code = kDocumentDiagnostic) {
            Error error = MakeError(ConfigurationErrors::JsonParseError, std::string{message});
            error.diagnostics.push_back({.code = DiagnosticCode{std::string{code}},
                                         .severity = DiagnosticSeverity::Error,
                                         .message = std::string{message},
                                         .location = {.source = std::move(sourceName)}});
            return Result<ConfigurationSourceMap>::Failure(std::move(error));
        }

        [[nodiscard]] Result<SettingValue> ValueFromJson(const Json &value) {
            if (value.is_object() && value.size() == 1 && value.contains("credentialReference") &&
                value["credentialReference"].is_string()) {
                auto reference = CredentialReference::Parse(value["credentialReference"].get_ref<const std::string &>());
                if (reference.HasError())
                    return Result<SettingValue>::Failure(reference.ErrorValue());
                return Result<SettingValue>::Success(std::move(reference).Value());
            }
            if (value.is_boolean())
                return Result<SettingValue>::Success(value.get<bool>());
            if (value.is_number_unsigned()) {
                const std::uint64_t unsignedValue = value.get<std::uint64_t>();
                if (unsignedValue <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                    return Result<SettingValue>::Success(static_cast<std::int64_t>(unsignedValue));
            } else if (value.is_number_integer()) {
                return Result<SettingValue>::Success(value.get<std::int64_t>());
            } else if (value.is_string()) {
                return Result<SettingValue>::Success(value.get<std::string>());
            }
            return Result<SettingValue>::Failure(MakeError(ConfigurationErrors::JsonParseError));
        }

        [[nodiscard]] bool DocumentInputWithinLimits(const ConfigurationSchema &schema, const std::string_view document,
                                                     const std::string_view sourceName, const ConfigurationLimits &limits) noexcept {
            return schema.IsSealed() && document.size() <= limits.maximumDocumentBytes &&
                   sourceName.size() <= limits.maximumSourceLocationBytes;
        }

        [[nodiscard]] bool HasExpectedDocumentShape(const Json &parsed) {
            return parsed.is_object() && parsed.size() == 2 && parsed.contains("schemaVersion") && parsed.contains("values") &&
                   parsed["values"].is_object();
        }

        [[nodiscard]] bool HasSupportedDocumentVersion(const Json &parsed) {
            return parsed["schemaVersion"].is_number_unsigned() &&
                   parsed["schemaVersion"].get<std::uint64_t>() == kConfigurationDocumentVersion;
        }

        [[nodiscard]] Result<ConfigurationInputValue> ParseDocumentEntry(const std::string_view key, const Json &jsonValue,
                                                                         const std::string &sourceName, const ConfigurationLimits &limits) {
            if (key.size() > limits.maximumKeyBytes)
                return Result<ConfigurationInputValue>::Failure(MakeError(ConfigurationErrors::InputTooLarge));
            if (!ValidSettingKey(key))
                return Result<ConfigurationInputValue>::Failure(MakeError(ConfigurationErrors::ValueInvalid));
            Result<SettingValue> value = ValueFromJson(jsonValue);
            if (value.HasError())
                return Result<ConfigurationInputValue>::Failure(value.ErrorValue());
            if (!StringWithinLimit(value.Value(), limits))
                return Result<ConfigurationInputValue>::Failure(MakeError(ConfigurationErrors::InputTooLarge));
            if (!ValidStringEncoding(value.Value()))
                return Result<ConfigurationInputValue>::Failure(MakeError(ConfigurationErrors::ValueInvalid));
            return Result<ConfigurationInputValue>::Success(
                {.value = std::move(value).Value(), .location = SourceLocation{.source = sourceName}});
        }

        [[nodiscard]] bool IsValidEnvironmentBinding(const EnvironmentVariableBinding &binding, const SettingDescriptor *descriptor,
                                                     StringSet &keys, StringSet &variables, const ConfigurationLimits &limits) {
            return binding.variable.size() <= limits.maximumSourceLocationBytes && binding.variable.starts_with("HORO_") &&
                   binding.variable.size() > 5 &&
                   std::ranges::all_of(std::string_view{binding.variable}.substr(5),
                                       [](const char character) {
                return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '_';
            }) && descriptor != nullptr &&
                   Allows(PolicyFor(*descriptor), ConfigurationSource::Environment) && variables.emplace(binding.variable).second &&
                   keys.emplace(binding.key.Value()).second;
        }

        enum class EnvironmentCaptureFailure : std::uint8_t {
            None,
            Oversized,
            InvalidType
        };

        struct EnvironmentCaptureOutcome {
            std::optional<ConfigurationInputValue> input;
            EnvironmentCaptureFailure failure = EnvironmentCaptureFailure::None;
        };

        [[nodiscard]] EnvironmentCaptureOutcome CaptureEnvironmentValue(const SettingDescriptor &descriptor,
                                                                        const EnvironmentVariableBinding &binding,
                                                                        const ProcessService &processes,
                                                                        const ConfigurationLimits &limits) {
            const std::optional<std::string> raw = processes.EnvironmentValue(binding.variable);
            if (!raw.has_value())
                return {};
            if (raw->size() > limits.maximumStringValueBytes)
                return {.failure = EnvironmentCaptureFailure::Oversized};
            Result<SettingValue> parsed = ParseEnvironmentValue(descriptor, *raw);
            if (parsed.HasError() || !MatchesValidation(descriptor, parsed.Value()))
                return {.failure = EnvironmentCaptureFailure::InvalidType};
            return {.input = ConfigurationInputValue{.value = std::move(parsed).Value(),
                                                     .location = SourceLocation{.source = binding.variable}}};
        }

        [[nodiscard]] bool SameLocation(const std::optional<SourceLocation> &left, const std::optional<SourceLocation> &right) noexcept {
            if (left.has_value() != right.has_value())
                return false;
            return !left.has_value() || (left->source == right->source && left->line == right->line && left->column == right->column);
        }

        [[nodiscard]] bool SameResolved(const ResolvedSetting &left, const ResolvedSetting &right) noexcept {
            return left.value == right.value && left.source == right.source && SameLocation(left.location, right.location) &&
                   left.sensitivity == right.sensitivity &&
                   std::ranges::equal(left.candidates, right.candidates,
                                      [](const ConfigurationCandidate &first, const ConfigurationCandidate &second) {
                return first.Source() == second.Source() && first.Value() == second.Value() &&
                       SameLocation(first.Location(), second.Location());
            });
        }

        [[nodiscard]] bool PermitsReload(const ReloadPolicy policy, const ConfigurationReloadPoint point) noexcept {
            using enum ConfigurationReloadPoint;
            switch (policy) {
                case ReloadPolicy::Immediate:
                    return true;
                case ReloadPolicy::NextFrame:
                    return point == NextFrame || point == NextFrameAndOperation || point == ProjectReopen || point == ProcessRestart;
                case ReloadPolicy::NextOperation:
                    return point == NextOperation || point == NextFrameAndOperation || point == ProjectReopen || point == ProcessRestart;
                case ReloadPolicy::ProjectReopen:
                    return point == ProjectReopen || point == ProcessRestart;
                case ReloadPolicy::ProcessRestart:
                    return point == ProcessRestart;
            }
            return false;
        }

        [[nodiscard]] bool DraftWithinLimits(const ConfigurationDraft &draft, const ConfigurationLimits &limits = {}) noexcept {
            if (draft.proposedValues.size() > limits.maximumKeysPerSource)
                return false;
            return std::ranges::all_of(draft.proposedValues, [&limits](const auto &entry) {
                return entry.first.Value().size() <= limits.maximumKeyBytes && StringWithinLimit(entry.second, limits);
            });
        }

        [[nodiscard]] Result<void> ValidateDraftCandidate(const ConfigurationSchema &schema, const ConfigurationDraft &draft,
                                                          const ConfigurationRevision activeRevision) {
            if (draft.baseRevision != activeRevision)
                return Result<void>::Failure(MakeError(ConfigurationErrors::DraftStale));
            if (!DraftWithinLimits(draft))
                return Result<void>::Failure(MakeError(ConfigurationErrors::InputTooLarge));
            using DraftEntry = decltype(draft.proposedValues)::value_type;
            std::vector<const DraftEntry *> entries;
            entries.reserve(draft.proposedValues.size());
            for (const auto &entry : draft.proposedValues)
                entries.push_back(&entry);
            std::ranges::sort(entries, {}, [](const DraftEntry *entry) {
                return entry->first.Value();
            });
            for (const auto *entry : entries) {
                const auto &[key, value] = *entry;
                const SettingDescriptor *descriptor = schema.FindDescriptor(key);
                if (descriptor == nullptr || !ConfigurationSchema::MatchesType(descriptor->type, value) ||
                    descriptor->sensitivity == SettingSensitivity::SecretReference || descriptor->readOnly ||
                    !Allows(PolicyFor(*descriptor), draft.source))
                    return Result<void>::Failure(InvalidConfigurationValue(key, value, descriptor));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<ConfigurationSourceMap> DocumentEntryFailure(const Error &error, std::string sourceName) {
            const bool tooLarge = error.code.Value() == ConfigurationErrors::InputTooLarge.code.Value();
            return DocumentFailure(tooLarge ? "Configuration document contains an oversized key or value."
                                            : "Configuration document contains an unsupported value.",
                                   std::move(sourceName), tooLarge ? kInputLimitDiagnostic : kDocumentDiagnostic);
        }
    }  // namespace

    /** @copydoc CredentialReference::Parse */
    Result<CredentialReference> CredentialReference::Parse(const std::string_view value) {
        constexpr std::string_view prefix = "cred:v1:";
        if (value.size() != prefix.size() + 48 || !value.starts_with(prefix) ||
            !std::ranges::all_of(value.substr(prefix.size()), [](const char digit) {
            return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
        }))
            return Result<CredentialReference>::Failure(MakeError(ConfigurationErrors::ValueInvalid));
        return Result<CredentialReference>::Success(CredentialReference{std::string{value}});
    }

    CredentialReference::CredentialReference(std::string value) : value_(std::move(value)) {}

    /** @copydoc CredentialReference::Value */
    std::string_view CredentialReference::Value() const noexcept {
        return value_;
    }

    /** @copydoc ConfigurationSnapshot::Revision */
    ConfigurationRevision ConfigurationSnapshot::Revision() const noexcept {
        return m_data->revision;
    }

    /** @copydoc ConfigurationSnapshot::Get */
    const SettingValue &ConfigurationSnapshot::Get(const SettingKey &key) const {
        const auto found = m_data->values.find(key);
        HORO_INVARIANT_MSG(found != m_data->values.end(), "ConfigurationSnapshot::Get requires a registered key.");
        return found->second.value;
    }

    /** @copydoc ConfigurationSnapshot::Find */
    std::optional<SettingValue> ConfigurationSnapshot::Find(const SettingKey &key) const {
        const ResolvedSetting *resolved = FindResolved(key);
        return resolved == nullptr ? std::nullopt : std::optional<SettingValue>{resolved->value};
    }

    /** @copydoc ConfigurationSnapshot::FindResolved */
    const ResolvedSetting *ConfigurationSnapshot::FindResolved(const SettingKey &key) const noexcept {
        const auto found = m_data->values.find(key);
        return found == m_data->values.end() ? nullptr : &found->second;
    }

    /** @copydoc ConfigurationSnapshot::ToJson */
    std::string ConfigurationSnapshot::ToJson() const {
        using SnapshotEntry = std::unordered_map<SettingKey, ResolvedSetting, SettingKeyHash>::value_type;
        std::vector<const SnapshotEntry *> sorted;
        sorted.reserve(m_data->values.size());
        for (const auto &entry : m_data->values) {
            const auto *reference = std::get_if<CredentialReference>(&entry.second.value);
            if (entry.second.sensitivity == SettingSensitivity::Public || (reference != nullptr && !reference->Value().empty()))
                sorted.push_back(&entry);
        }
        std::ranges::sort(sorted, {}, [](const SnapshotEntry *entry) -> const std::string & {
            return entry->first.Value();
        });

        std::ostringstream json;
        json.imbue(std::locale::classic());
        json << "{\n  \"schemaVersion\": " << kConfigurationDocumentVersion << ",\n  \"values\": {\n";
        for (std::size_t index = 0; index < sorted.size(); ++index) {
            const auto &[key, resolved] = *sorted[index];
            json << "    \"" << EscapeJsonString(key.Value()) << "\": ";
            std::visit([&json]<typename Value>(const Value &value) {
                if constexpr (std::is_same_v<Value, bool>)
                    json << (value ? "true" : "false");
                else if constexpr (std::is_same_v<Value, std::int64_t>)
                    json << value;
                else if constexpr (std::is_same_v<Value, CredentialReference>)
                    json << "{\"credentialReference\":\"" << value.Value() << "\"}";
                else
                    json << "\"" << EscapeJsonString(value) << "\"";
            }, resolved.value);
            json << (index + 1 == sorted.size() ? "\n" : ",\n");
        }
        json << "  }\n}\n";
        return json.str();
    }

    /** @copydoc ConfigurationSnapshot::SourceToJson */
    std::string ConfigurationSnapshot::SourceToJson(const ConfigurationSource source) const {
        auto data = std::make_shared<Data>();
        for (const auto &[key, resolved] : m_data->values) {
            const auto candidate = std::ranges::find(resolved.candidates, source, &ConfigurationCandidate::Source);
            if (candidate != resolved.candidates.end())
                data->values.emplace(key, ResolvedSetting{.value = candidate->Value(),
                                                          .source = source,
                                                          .location = candidate->Location(),
                                                          .sensitivity = resolved.sensitivity});
        }
        return ConfigurationSnapshot{std::move(data)}.ToJson();
    }

    /** @copydoc ConfigurationSchema::MatchesType */
    bool ConfigurationSchema::MatchesType(const SettingValueType type, const SettingValue &value) {
        using enum SettingValueType;
        return (type == Boolean && std::holds_alternative<bool>(value)) ||
               (type == Integer && std::holds_alternative<std::int64_t>(value)) ||
               (type == String && std::holds_alternative<std::string>(value)) ||
               (type == SettingValueType::CredentialReference && std::holds_alternative<Horo::CredentialReference>(value));
    }

    /** @copydoc ConfigurationSchema::AllowsSource */
    bool ConfigurationSchema::AllowsSource(const SettingDescriptor &descriptor, const ConfigurationSource source) noexcept {
        return Allows(PolicyFor(descriptor), source);
    }

    /** @copydoc ConfigurationSchema::ErrorFor */
    Error ConfigurationSchema::ErrorFor(const ErrorCodeDescriptor &descriptor) {
        return MakeError(descriptor);
    }

    /** @copydoc ConfigurationSchema::Register */
    Result<void> ConfigurationSchema::Register(const SettingDescriptor &descriptor) {
        if (const ConfigurationLimits limits;
            m_sealed || m_descriptors.size() >= limits.maximumKeysPerSource || descriptor.key.Value().empty() ||
            descriptor.key.Value().size() > limits.maximumKeyBytes || m_descriptors.contains(descriptor.key) ||
            !MatchesType(descriptor.type, descriptor.defaultValue) || !StringWithinLimit(descriptor.defaultValue, limits) ||
            !ValidSettingKey(descriptor.key.Value()) || !ValidStringEncoding(descriptor.defaultValue) ||
            (descriptor.label &&
             (descriptor.label->namespaceId.size() > limits.maximumKeyBytes || descriptor.label->key.size() > limits.maximumKeyBytes ||
              !ValidSettingKey(descriptor.label->namespaceId) || !ValidSettingKey(descriptor.label->key))) ||
            !ValidDescriptorPolicy(descriptor) || !ValidValidation(descriptor) || !MatchesValidation(descriptor, descriptor.defaultValue) ||
            ((descriptor.type == SettingValueType::CredentialReference) != (descriptor.sensitivity == SettingSensitivity::SecretReference)))
            return Result<void>::Failure(ErrorFor(ConfigurationErrors::SchemaInvalid));
        m_descriptors.try_emplace(descriptor.key, descriptor);
        return Result<void>::Success();
    }

    /** @copydoc ConfigurationSchema::Seal */
    Result<void> ConfigurationSchema::Seal() {
        if (m_sealed)
            return Result<void>::Failure(ErrorFor(ConfigurationErrors::SchemaSealed));
        m_sealed = true;
        return Result<void>::Success();
    }

    /** @copydoc ConfigurationSchema::FindDescriptor */
    const SettingDescriptor *ConfigurationSchema::FindDescriptor(const SettingKey &key) const noexcept {
        const auto found = m_descriptors.find(key);
        return found == m_descriptors.end() ? nullptr : &found->second;
    }

    /** @copydoc ConfigurationSchema::IsSealed */
    bool ConfigurationSchema::IsSealed() const noexcept {
        return m_sealed;
    }

    /** @copydoc ConfigurationSchema::Descriptors */
    std::vector<SettingDescriptor> ConfigurationSchema::Descriptors() const {
        std::vector<SettingDescriptor> descriptors;
        descriptors.reserve(m_descriptors.size());
        for (const auto &[key, descriptor] : m_descriptors) {
            static_cast<void>(key);
            descriptors.push_back(descriptor);
        }
        std::ranges::sort(descriptors, {}, [](const SettingDescriptor &descriptor) {
            return descriptor.key.Value();
        });
        return descriptors;
    }

    /** @copydoc ConfigurationResolver::Resolve */
    Result<ConfigurationSnapshot> ConfigurationResolver::Resolve(const ConfigurationSchema &schema,
                                                                 const ConfigurationResolutionRequest &request,
                                                                 const ConfigurationRevision revision,
                                                                 const ConfigurationLimits &requestedLimits) {
        const auto limits = BoundedLimits(requestedLimits);
        if (!schema.m_sealed)
            return Result<ConfigurationSnapshot>::Failure(ConfigurationSchema::ErrorFor(ConfigurationErrors::SchemaInvalid));
        std::vector<Diagnostic> findings;
        for (const SourceView source : SourceViews(request))
            ValidateSource(schema, source, limits, findings);
        if (!findings.empty())
            return Result<ConfigurationSnapshot>::Failure(ResolutionFailure(std::move(findings)));

        auto data = std::make_shared<ConfigurationSnapshot::Data>();
        data->revision = revision;
        data->inputs = std::make_shared<const ConfigurationResolutionRequest>(request);
        auto defaults = std::make_shared<ConfigurationSourceMap>();
        defaults->reserve(schema.m_descriptors.size());
        for (const auto &[key, descriptor] : schema.m_descriptors)
            defaults->emplace(key, ConfigurationInputValue{.value = descriptor.defaultValue});
        data->values.reserve(schema.m_descriptors.size());
        for (const auto &[key, descriptor] : schema.m_descriptors)
            data->values.try_emplace(key, ResolveSetting(descriptor, data->inputs, defaults));
        return Result<ConfigurationSnapshot>::Success(ConfigurationSnapshot{std::move(data)});
    }

    /** @copydoc ConfigurationResolver::CaptureEnvironment */
    Result<ConfigurationSourceMap> ConfigurationResolver::CaptureEnvironment(const ConfigurationSchema &schema,
                                                                             const std::span<const EnvironmentVariableBinding> bindings,
                                                                             const ProcessService &processes,
                                                                             const ConfigurationLimits &requestedLimits) {
        const auto limits = BoundedLimits(requestedLimits);
        if (!schema.m_sealed || bindings.size() > limits.maximumEnvironmentBindings)
            return DocumentFailure("Environment binding set is invalid or exceeds its limit.", "environment",
                                   kEnvironmentBindingDiagnostic);

        ConfigurationSourceMap captured;
        captured.reserve(bindings.size());
        StringSet variables;
        StringSet keys;
        variables.reserve(bindings.size());
        keys.reserve(bindings.size());
        std::vector<const EnvironmentVariableBinding *> ordered;
        ordered.reserve(bindings.size());
        for (const auto &binding : bindings) {
            if (binding.variable.size() > limits.maximumSourceLocationBytes || binding.key.Value().size() > limits.maximumKeyBytes)
                return DocumentFailure("Environment binding exceeds its metadata limits.", "environment", kInputLimitDiagnostic);
            ordered.push_back(&binding);
        }
        std::ranges::sort(ordered, {}, [](const EnvironmentVariableBinding *binding) {
            return std::pair<std::string_view, std::string_view>{binding->variable, binding->key.Value()};
        });
        for (const auto *entry : ordered) {
            const auto &binding = *entry;
            const SettingDescriptor *descriptor = schema.FindDescriptor(binding.key);
            if (!IsValidEnvironmentBinding(binding, descriptor, keys, variables, limits)) {
                return DocumentFailure("Environment binding is unknown, duplicate, forbidden, unbounded, or malformed.", "environment",
                                       kEnvironmentBindingDiagnostic);
            }
            EnvironmentCaptureOutcome outcome = CaptureEnvironmentValue(*descriptor, binding, processes, limits);
            if (outcome.failure == EnvironmentCaptureFailure::Oversized)
                return DocumentFailure("Environment value exceeds its byte limit.", binding.variable, kInputLimitDiagnostic);
            if (outcome.failure == EnvironmentCaptureFailure::InvalidType)
                return DocumentFailure("Environment value violates the declared setting type or validation rule.", binding.variable,
                                       kInvalidValueDiagnostic);
            if (outcome.input.has_value())
                captured.try_emplace(binding.key, std::move(*outcome.input));
        }
        return Result<ConfigurationSourceMap>::Success(std::move(captured));
    }

    /** @copydoc ConfigurationResolver::ParseDocument */
    Result<ConfigurationSourceMap> ConfigurationResolver::ParseDocument(const ConfigurationSchema &schema, const std::string_view document,
                                                                        std::string sourceName,
                                                                        const ConfigurationLimits &requestedLimits) {
        const auto limits = BoundedLimits(requestedLimits);
        if (sourceName.size() > limits.maximumSourceLocationBytes)
            return DocumentFailure("Configuration source name exceeds its byte limit.", "configuration-document", kInputLimitDiagnostic);
        if (!DocumentInputWithinLimits(schema, document, sourceName, limits))
            return DocumentFailure("Configuration document or source name exceeds its limit.", std::move(sourceName),
                                   kInputLimitDiagnostic);

        Foundation::JsonParseGuard guard{std::min(limits.maximumNestingDepth, ConfigurationLimits{}.maximumNestingDepth)};
        if (!guard.Validate(document.begin(), document.end()))
            return DocumentFailure("Configuration document has invalid syntax, duplicate keys, encoding, or nesting.",
                                   std::move(sourceName), guard.IsTooDeep() ? kInputLimitDiagnostic : kDocumentDiagnostic);
        const Json parsed = Json::parse(document, nullptr, false);
        if (parsed.is_discarded() || !HasExpectedDocumentShape(parsed) || !HasSupportedDocumentVersion(parsed))
            return DocumentFailure("Configuration document must contain only schemaVersion 1 and an object-valued values member.",
                                   std::move(sourceName));

        const Json &values = parsed["values"];
        if (values.size() > limits.maximumKeysPerSource)
            return DocumentFailure("Configuration document contains too many keys.", std::move(sourceName), kInputLimitDiagnostic);

        ConfigurationSourceMap result;
        result.reserve(values.size());
        for (auto iterator = values.begin(); iterator != values.end(); ++iterator) {
            Result<ConfigurationInputValue> input = ParseDocumentEntry(iterator.key(), iterator.value(), sourceName, limits);
            if (input.HasError())
                return DocumentEntryFailure(input.ErrorValue(), std::move(sourceName));
            result.try_emplace(SettingKey{iterator.key()}, std::move(input).Value());
        }
        return Result<ConfigurationSourceMap>::Success(std::move(result));
    }

    /** @copydoc ConfigurationService::ConfigurationService */
    ConfigurationService::ConfigurationService(ConfigurationSchema schema, EngineDataBus *events)
        : ConfigurationService(std::move(schema), events, {}) {}

    /** @copydoc ConfigurationService::ConfigurationService */
    ConfigurationService::ConfigurationService(ConfigurationSchema schema, EngineDataBus *events,
                                               const ConfigurationResolutionRequest &initial)
        : m_schema(std::move(schema)), m_events(events) {
        HORO_INVARIANT_MSG(m_schema.m_sealed, "ConfigurationService requires a sealed schema.");
        Result<ConfigurationSnapshot> resolved = ConfigurationResolver::Resolve(m_schema, initial);
        HORO_INVARIANT_MSG(resolved.HasValue(), "A sealed configuration schema and validated sources must resolve their initial snapshot.");
        m_active = std::move(resolved).Value().m_data;
    }

    /** @copydoc ConfigurationService::Snapshot */
    ConfigurationSnapshot ConfigurationService::Snapshot() const {
        std::lock_guard lock(m_mutex);
        return ConfigurationSnapshot(m_active);
    }

    /** @copydoc ConfigurationService::Schema */
    const ConfigurationSchema &ConfigurationService::Schema() const noexcept {
        return m_schema;
    }

    /** @copydoc ConfigurationService::Preview */
    Result<ConfigurationSnapshot> ConfigurationService::Preview(const ConfigurationDraft &draft) const {
        const auto before = Snapshot();
        if (auto validation = ValidateDraftCandidate(m_schema, draft, before.Revision()); validation.HasError())
            return Result<ConfigurationSnapshot>::Failure(validation.ErrorValue());
        ConfigurationResolutionRequest inputs = *before.m_data->inputs;
        auto *source = DraftSource(inputs, draft.source);
        if (source == nullptr)
            return Result<ConfigurationSnapshot>::Failure(MakeError(ConfigurationErrors::ValueInvalid));
        for (const auto &[key, value] : draft.proposedValues) {
            const auto previous = source->find(key);
            const auto location = previous == source->end() ? std::optional<SourceLocation>{} : previous->second.location;
            source->insert_or_assign(key, ConfigurationInputValue{.value = value, .location = location});
        }
        return ConfigurationResolver::Resolve(m_schema, inputs, before.Revision() + 1);
    }

    /** @copydoc ConfigurationService::Validate */
    Result<void> ConfigurationService::Validate(const ConfigurationDraft &draft) const {
        const auto preview = Preview(draft);
        if (preview.HasError())
            return Result<void>::Failure(preview.ErrorValue());
        return Result<void>::Success();
    }

    ConfigurationService::PreparedCommit::PreparedCommit(ConfigurationService &owner, std::unique_lock<std::mutex> lock,
                                                         ConfigurationSnapshot candidate, ConfigurationChangedEvent event)
        : owner_(&owner), lock_(std::move(lock)), candidate_(std::move(candidate)), event_(std::move(event)) {}

    /** @copydoc ConfigurationService::PreparedCommit::Candidate */
    ConfigurationSnapshot ConfigurationService::PreparedCommit::Candidate() const {
        return candidate_;
    }

    /** @copydoc ConfigurationService::PreparedCommit::Commit */
    Result<void> ConfigurationService::PreparedCommit::Commit() {
        if (!lock_.owns_lock())
            return Result<void>::Failure(MakeError(ConfigurationErrors::ValueInvalid));
        std::shared_ptr<const ConfigurationSnapshot::Data> retired;
        std::optional<PendingReload> discarded;
        {
            std::lock_guard registryLock(owner_->m_mutex);
            retired = std::move(owner_->m_active);
            owner_->m_active = candidate_.m_data;
            discarded = std::move(owner_->m_pendingReload);
            owner_->m_pendingReload.reset();
            ++owner_->m_reloadSequence;
        }
        lock_.unlock();
        if (owner_->m_events != nullptr && !event_.changedKeys.empty())
            owner_->m_events->Publish(event_);
        return Result<void>::Success();
    }

    /** @copydoc ConfigurationService::Prepare */
    Result<ConfigurationService::PreparedCommit> ConfigurationService::Prepare(const ConfigurationDraft &draft) {
        std::unique_lock writerLock(m_writerMutex);
        auto candidate = Preview(draft);
        if (candidate.HasError())
            return Result<PreparedCommit>::Failure(candidate.ErrorValue());
        ConfigurationChangedEvent event{.revision = candidate.Value().Revision()};
        const auto before = Snapshot();
        for (const auto &[key, value] : candidate.Value().m_data->values) {
            if (!SameResolved(before.m_data->values.at(key), value))
                event.changedKeys.push_back(key);
        }
        std::ranges::sort(event.changedKeys, {}, [](const SettingKey &key) -> const std::string & {
            return key.Value();
        });
        auto preparedCandidate = std::move(candidate).Value();
        if (event.changedKeys.empty()) {
            preparedCandidate = Snapshot();
            event.revision = preparedCandidate.Revision();
        }
        return Result<PreparedCommit>::Success(
            PreparedCommit{*this, std::move(writerLock), std::move(preparedCandidate), std::move(event)});
    }

    /** @copydoc ConfigurationService::Commit */
    Result<void> ConfigurationService::Commit(const ConfigurationDraft &draft) {
        auto prepared = Prepare(draft);
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        auto transaction = std::move(prepared).Value();
        return transaction.Commit();
    }

    /** @copydoc ConfigurationService::ResolveAndCommit */
    Result<void> ConfigurationService::ResolveAndCommit(const ConfigurationResolutionRequest &request, const ConfigurationLimits &limits) {
        std::unique_lock writerLock(m_writerMutex);
        const ConfigurationSnapshot before = Snapshot();
        Result<ConfigurationSnapshot> resolved = ConfigurationResolver::Resolve(m_schema, request, before.Revision() + 1, limits);
        if (resolved.HasError())
            return Result<void>::Failure(resolved.ErrorValue());

        std::vector<SettingKey> changed;
        for (const auto &[key, value] : resolved.Value().m_data->values) {
            const auto previous = before.m_data->values.find(key);
            if (previous == before.m_data->values.end() || !SameResolved(previous->second, value))
                changed.push_back(key);
        }
        std::shared_ptr<const ConfigurationSnapshot::Data> retired;
        std::optional<PendingReload> discarded;
        {
            std::lock_guard lock(m_mutex);
            if (m_active->revision != before.Revision())
                return Result<void>::Failure(ConfigurationSchema::ErrorFor(ConfigurationErrors::DraftStale));
            if (!changed.empty()) {
                retired = std::move(m_active);
                m_active = resolved.Value().m_data;
            }
            discarded = std::move(m_pendingReload);
            m_pendingReload.reset();
            ++m_reloadSequence;
        }
        writerLock.unlock();
        if (m_events != nullptr && !changed.empty()) {
            std::ranges::sort(changed, {}, [](const SettingKey &key) -> const std::string & {
                return key.Value();
            });
            m_events->Publish(ConfigurationChangedEvent{.revision = before.Revision() + 1,
                                                        .domain = ConfigurationDomain::All,
                                                        .changedKeys = std::move(changed)});
        }
        return Result<void>::Success();
    }

    /** @copydoc ConfigurationService::StageReload */
    Result<void> ConfigurationService::StageReload(const ConfigurationResolutionRequest &request, const ConfigurationLimits &limits) {
        ConfigurationRevision baseRevision{};
        std::uint64_t sequence{};
        std::shared_ptr<const ConfigurationSnapshot::Data> active;
        std::optional<PendingReload> discarded;
        {
            std::lock_guard lock(m_mutex);
            baseRevision = m_active->revision;
            active = m_active;
            sequence = ++m_reloadSequence;
            discarded = std::move(m_pendingReload);
            m_pendingReload.reset();
        }
        discarded.reset();

        Result<ConfigurationSnapshot> resolved = ConfigurationResolver::Resolve(m_schema, request, baseRevision + 1, limits);
        if (resolved.HasError()) {
            std::lock_guard lock(m_mutex);
            if (sequence == m_reloadSequence)
                m_pendingReload.reset();
            return Result<void>::Failure(resolved.ErrorValue());
        }
        auto candidate = std::move(resolved).Value().m_data;
        std::vector<SettingKey> changed;
        for (const auto &[key, value] : candidate->values) {
            const auto previous = active->values.find(key);
            if (previous == active->values.end() || !SameResolved(previous->second, value))
                changed.push_back(key);
        }
        std::ranges::sort(changed, {}, [](const SettingKey &key) -> const std::string & {
            return key.Value();
        });
        std::lock_guard lock(m_mutex);
        if (sequence != m_reloadSequence || baseRevision != m_active->revision)
            return Result<void>::Failure(ConfigurationSchema::ErrorFor(ConfigurationErrors::DraftStale));
        if (changed.empty())
            return Result<void>::Success();
        m_pendingReload = PendingReload{.snapshot = std::move(candidate), .changedKeys = std::move(changed), .baseRevision = baseRevision};
        return Result<void>::Success();
    }

    /** @copydoc ConfigurationService::CancelPendingReload */
    void ConfigurationService::CancelPendingReload() {
        std::optional<PendingReload> discarded;
        {
            std::lock_guard lock(m_mutex);
            ++m_reloadSequence;
            discarded = std::move(m_pendingReload);
            m_pendingReload.reset();
        }
    }

    /** @copydoc ConfigurationService::ActivateReload */
    Result<bool> ConfigurationService::ActivateReload(const ConfigurationReloadPoint point) {
        if (static_cast<std::uint8_t>(point) > static_cast<std::uint8_t>(ConfigurationReloadPoint::ProcessRestart))
            return Result<bool>::Failure(MakeError(ConfigurationErrors::ValueInvalid));
        std::unique_lock writerLock(m_writerMutex, std::try_to_lock);
        if (!writerLock.owns_lock())
            return Result<bool>::Success(false);
        std::optional<ConfigurationChangedEvent> event;
        std::shared_ptr<const ConfigurationSnapshot::Data> retired;
        {
            std::lock_guard lock(m_mutex);
            if (!m_pendingReload.has_value())
                return Result<bool>::Success(false);
            if (m_pendingReload->baseRevision != m_active->revision) {
                m_pendingReload.reset();
                return Result<bool>::Failure(ConfigurationSchema::ErrorFor(ConfigurationErrors::DraftStale));
            }

            for (const SettingKey &key : m_pendingReload->changedKeys) {
                const SettingDescriptor *descriptor = m_schema.FindDescriptor(key);
                HORO_INVARIANT_MSG(descriptor != nullptr, "Resolved reload candidates contain only registered settings.");
                if (!PermitsReload(descriptor->reloadPolicy, point))
                    return Result<bool>::Success(false);
            }
            PendingReload pending = std::move(*m_pendingReload);
            m_pendingReload.reset();
            if (pending.changedKeys.empty())
                return Result<bool>::Success(false);

            retired = std::move(m_active);
            m_active = std::move(pending.snapshot);
            if (m_events != nullptr)
                event = ConfigurationChangedEvent{.revision = m_active->revision,
                                                  .domain = ConfigurationDomain::All,
                                                  .changedKeys = std::move(pending.changedKeys)};
        }
        writerLock.unlock();
        if (event.has_value())
            m_events->PublishAsync(std::move(*event));
        return Result<bool>::Success(true);
    }

    /** @copydoc ConfigurationService::LoadJson */
    Result<void> ConfigurationService::LoadJson(const std::string &jsonString) {
        Result<ConfigurationSourceMap> parsed = ConfigurationResolver::ParseDocument(m_schema, jsonString, "inline-user");
        if (parsed.HasError())
            return Result<void>::Failure(parsed.ErrorValue());
        ConfigurationResolutionRequest request;
        request.user = std::move(parsed).Value();
        return ResolveAndCommit(request);
    }

    /** @copydoc ConfigurationService::LoadFile */
    Result<void> ConfigurationService::LoadFile(const std::string &path) const {
        static_cast<void>(path);
        return Result<void>::Failure(ConfigurationSchema::ErrorFor(ConfigurationErrors::PersistenceUnavailable));
    }

    /** @copydoc ConfigurationService::SaveFile */
    Result<void> ConfigurationService::SaveFile(const std::string &path) const {
        static_cast<void>(path);
        return Result<void>::Failure(ConfigurationSchema::ErrorFor(ConfigurationErrors::PersistenceUnavailable));
    }
}  // namespace Horo
