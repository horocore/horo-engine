#include "Horo/Foundation/Configuration.h"

#include "../FoundationErrors.h"
#include "Horo/Foundation/Assertions.h"
#include "Horo/Foundation/Platform.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
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

        [[nodiscard]] std::array<SourceView, 6> SourceViews(const ConfigurationResolutionRequest &request) noexcept {
            using enum ConfigurationSource;
            return {{{Invocation, &request.invocation},
                     {Environment, &request.environment},
                     {Session, &request.session},
                     {Project, &request.project},
                     {User, &request.user},
                     {PackagedProfile, &request.packagedProfile}}};
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
            }
            return "unknown";
        }

        [[nodiscard]] std::string_view SettingValueTypeName(const SettingValue &value) noexcept {
            if (std::holds_alternative<bool>(value))
                return "boolean";
            if (std::holds_alternative<std::int64_t>(value))
                return "integer";
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

        [[nodiscard]] bool Allows(const ConfigurationSourcePolicy &policy, const ConfigurationSource source) noexcept {
            return (static_cast<std::uint16_t>(policy.allowedSources) & static_cast<std::uint16_t>(SourceBit(source))) != 0U;
        }

        [[nodiscard]] Error InvalidConfigurationValue(const SettingKey &key, const SettingValue &value,
                                                      const SettingDescriptor *descriptor) {
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
            if (key.Value().size() > limits.maximumKeyBytes)
                return Finding(kInputLimitDiagnostic, "Configuration key exceeds the permitted byte length.", source.source, &input);
            if (input.location.has_value() && input.location->source.size() > limits.maximumSourceLocationBytes)
                return Finding(kInputLimitDiagnostic, "Configuration source location exceeds the permitted byte length.", source.source);
            const SettingDescriptor *descriptor = schema.FindDescriptor(key);
            if (descriptor == nullptr)
                return Finding(kUnknownKeyDiagnostic, "Configuration key '" + key.Value() + "' is not declared.", source.source, &input);
            if (!Allows(PolicyFor(*descriptor), source.source))
                return Finding(kForbiddenSourceDiagnostic,
                               "Configuration key '" + key.Value() + "' is not legal in " + std::string{SourceName(source.source)} + ".",
                               source.source, &input);
            if (descriptor->sensitivity == SettingSensitivity::SecretReference)
                return Finding(kSecretValueDiagnostic,
                               "Configuration key '" + key.Value() +
                                   "' cannot accept values until typed credential references are available.",
                               source.source, &input);
            if (!ConfigurationSchema::MatchesType(descriptor->type, input.value))
                return Finding(kInvalidValueDiagnostic,
                               "Configuration key '" + key.Value() + "' expects " + std::string{SettingTypeName(descriptor->type)} + ".",
                               source.source, &input);
            if (!StringWithinLimit(input.value, limits))
                return Finding(kInputLimitDiagnostic, "Configuration string value exceeds the permitted byte length.", source.source,
                               &input);
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

        [[nodiscard]] ResolvedSetting ResolveSetting(const SettingDescriptor &descriptor, const ConfigurationResolutionRequest &request) {
            const ConfigurationSourcePolicy policy = PolicyFor(descriptor);
            std::array<SourceView, 6> sources = SourceViews(request);
            if (policy.sessionOverridesEnvironment)
                std::swap(sources[1], sources[2]);
            for (const SourceView source : sources) {
                if (!Allows(policy, source.source))
                    continue;
                if (const ConfigurationInputValue *input = FindInput(*source.values, descriptor.key); input != nullptr)
                    return {.value = input->value,
                            .source = source.source,
                            .location = input->location,
                            .sensitivity = descriptor.sensitivity};
            }
            return {.value = descriptor.defaultValue,
                    .source = ConfigurationSource::SchemaDefault,
                    .location = std::nullopt,
                    .sensitivity = descriptor.sensitivity};
        }

        [[nodiscard]] Result<SettingValue> ParseEnvironmentValue(const SettingDescriptor &descriptor, const std::string_view raw) {
            if (raw.empty())
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

        [[nodiscard]] Json ParseJsonRejectingDuplicateKeys(const std::string_view document, bool &duplicateKey) {
            std::vector<StringSet> objectKeys;
            const auto callback = [&objectKeys, &duplicateKey](const int depth, const Json::parse_event_t event, const Json &parsed) {
                static_cast<void>(depth);
                using enum Json::parse_event_t;
                if (event == object_start) {
                    objectKeys.emplace_back();
                } else if (event == key) {
                    if (objectKeys.empty() || !objectKeys.back().emplace(parsed.get<std::string>()).second)
                        duplicateKey = true;
                } else if (event == object_end && !objectKeys.empty()) {
                    objectKeys.pop_back();
                }
                return true;
            };
            return Json::parse(document, callback, false, true);
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
            Result<SettingValue> value = ValueFromJson(jsonValue);
            if (value.HasError())
                return Result<ConfigurationInputValue>::Failure(value.ErrorValue());
            if (!StringWithinLimit(value.Value(), limits))
                return Result<ConfigurationInputValue>::Failure(MakeError(ConfigurationErrors::InputTooLarge));
            return Result<ConfigurationInputValue>::Success(
                {.value = std::move(value).Value(), .location = SourceLocation{.source = sourceName}});
        }

        [[nodiscard]] bool IsValidEnvironmentBinding(const EnvironmentVariableBinding &binding, const SettingDescriptor *descriptor,
                                                     const ConfigurationSourceMap &captured, StringSet &variables,
                                                     const ConfigurationLimits &limits) {
            return binding.variable.size() <= limits.maximumSourceLocationBytes && binding.variable.starts_with("HORO_") &&
                   descriptor != nullptr && variables.emplace(binding.variable).second && !captured.contains(binding.key);
        }

        enum class EnvironmentCaptureFailure : std::uint8_t {
            None,
            ForbiddenOrOversized,
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
            if (descriptor.sensitivity == SettingSensitivity::SecretReference || raw->size() > limits.maximumStringValueBytes)
                return {.failure = EnvironmentCaptureFailure::ForbiddenOrOversized};
            Result<SettingValue> parsed = ParseEnvironmentValue(descriptor, *raw);
            if (parsed.HasError())
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
                   left.sensitivity == right.sensitivity;
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
            for (const auto &[key, value] : draft.proposedValues) {
                const SettingDescriptor *descriptor = schema.FindDescriptor(key);
                if (descriptor == nullptr || !ConfigurationSchema::MatchesType(descriptor->type, value) ||
                    descriptor->sensitivity == SettingSensitivity::SecretReference)
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
            if (entry.second.sensitivity == SettingSensitivity::Public)
                sorted.push_back(&entry);
        }
        std::ranges::sort(sorted, {}, [](const SnapshotEntry *entry) -> const std::string & {
            return entry->first.Value();
        });

        std::ostringstream json;
        json << "{\n  \"schemaVersion\": " << kConfigurationDocumentVersion << ",\n  \"values\": {\n";
        for (std::size_t index = 0; index < sorted.size(); ++index) {
            const auto &[key, resolved] = *sorted[index];
            json << "    \"" << EscapeJsonString(key.Value()) << "\": ";
            std::visit([&json]<typename Value>(const Value &value) {
                if constexpr (std::is_same_v<Value, bool>)
                    json << (value ? "true" : "false");
                else if constexpr (std::is_same_v<Value, std::int64_t>)
                    json << value;
                else
                    json << "\"" << EscapeJsonString(value) << "\"";
            }, resolved.value);
            json << (index + 1 == sorted.size() ? "\n" : ",\n");
        }
        json << "  }\n}\n";
        return json.str();
    }

    /** @copydoc ConfigurationSchema::MatchesType */
    bool ConfigurationSchema::MatchesType(const SettingValueType type, const SettingValue &value) {
        using enum SettingValueType;
        return (type == Boolean && std::holds_alternative<bool>(value)) ||
               (type == Integer && std::holds_alternative<std::int64_t>(value)) ||
               (type == String && std::holds_alternative<std::string>(value));
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
            !MatchesType(descriptor.type, descriptor.defaultValue) || !StringWithinLimit(descriptor.defaultValue, limits))
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

    /** @copydoc ConfigurationResolver::Resolve */
    Result<ConfigurationSnapshot> ConfigurationResolver::Resolve(const ConfigurationSchema &schema,
                                                                 const ConfigurationResolutionRequest &request,
                                                                 const ConfigurationRevision revision, const ConfigurationLimits &limits) {
        if (!schema.m_sealed)
            return Result<ConfigurationSnapshot>::Failure(ConfigurationSchema::ErrorFor(ConfigurationErrors::SchemaInvalid));
        std::vector<Diagnostic> findings;
        for (const SourceView source : SourceViews(request))
            ValidateSource(schema, source, limits, findings);
        if (!findings.empty())
            return Result<ConfigurationSnapshot>::Failure(ResolutionFailure(std::move(findings)));

        auto data = std::make_shared<ConfigurationSnapshot::Data>();
        data->revision = revision;
        data->values.reserve(schema.m_descriptors.size());
        for (const auto &[key, descriptor] : schema.m_descriptors)
            data->values.try_emplace(key, ResolveSetting(descriptor, request));
        return Result<ConfigurationSnapshot>::Success(ConfigurationSnapshot{std::move(data)});
    }

    /** @copydoc ConfigurationResolver::CaptureEnvironment */
    Result<ConfigurationSourceMap> ConfigurationResolver::CaptureEnvironment(const ConfigurationSchema &schema,
                                                                             const std::span<const EnvironmentVariableBinding> bindings,
                                                                             const ProcessService &processes,
                                                                             const ConfigurationLimits &limits) {
        if (!schema.m_sealed || bindings.size() > limits.maximumEnvironmentBindings)
            return DocumentFailure("Environment binding set is invalid or exceeds its limit.", "environment",
                                   kEnvironmentBindingDiagnostic);

        ConfigurationSourceMap captured;
        captured.reserve(bindings.size());
        StringSet variables;
        variables.reserve(bindings.size());
        for (const EnvironmentVariableBinding &binding : bindings) {
            const SettingDescriptor *descriptor = schema.FindDescriptor(binding.key);
            if (!IsValidEnvironmentBinding(binding, descriptor, captured, variables, limits)) {
                return DocumentFailure("Environment binding is unknown, duplicate, unbounded, or lacks the HORO_ prefix.", binding.variable,
                                       kEnvironmentBindingDiagnostic);
            }
            const EnvironmentCaptureOutcome outcome = CaptureEnvironmentValue(*descriptor, binding, processes, limits);
            if (outcome.failure == EnvironmentCaptureFailure::ForbiddenOrOversized)
                return DocumentFailure("Environment value is forbidden or exceeds its limit.", binding.variable, kSecretValueDiagnostic);
            if (outcome.failure == EnvironmentCaptureFailure::InvalidType)
                return DocumentFailure("Environment value does not match the setting type.", binding.variable, kInvalidValueDiagnostic);
            if (outcome.input.has_value())
                captured.try_emplace(binding.key, *outcome.input);
        }
        return Result<ConfigurationSourceMap>::Success(std::move(captured));
    }

    /** @copydoc ConfigurationResolver::ParseDocument */
    Result<ConfigurationSourceMap> ConfigurationResolver::ParseDocument(const ConfigurationSchema &schema, const std::string_view document,
                                                                        std::string sourceName, const ConfigurationLimits &limits) {
        if (!DocumentInputWithinLimits(schema, document, sourceName, limits))
            return DocumentFailure("Configuration document or source name exceeds its limit.", std::move(sourceName),
                                   kInputLimitDiagnostic);

        bool duplicateKey = false;
        const Json parsed = ParseJsonRejectingDuplicateKeys(document, duplicateKey);
        if (duplicateKey || parsed.is_discarded() || !HasExpectedDocumentShape(parsed) || !HasSupportedDocumentVersion(parsed))
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
        : m_schema(std::move(schema)), m_events(events) {
        HORO_INVARIANT_MSG(m_schema.m_sealed, "ConfigurationService requires a sealed schema.");
        Result<ConfigurationSnapshot> initial = ConfigurationResolver::Resolve(m_schema, {});
        HORO_INVARIANT_MSG(initial.HasValue(), "A sealed configuration schema must resolve its initial snapshot.");
        m_active = std::move(initial).Value().m_data;
    }

    /** @copydoc ConfigurationService::Snapshot */
    ConfigurationSnapshot ConfigurationService::Snapshot() const {
        std::lock_guard lock(m_mutex);
        return ConfigurationSnapshot(m_active);
    }

    /** @copydoc ConfigurationService::Validate */
    Result<void> ConfigurationService::Validate(const ConfigurationDraft &draft) const {
        std::lock_guard lock(m_mutex);
        return ValidateDraftCandidate(m_schema, draft, m_active->revision);
    }

    /** @copydoc ConfigurationService::Commit */
    Result<void> ConfigurationService::Commit(const ConfigurationDraft &draft) {
        ConfigurationRevision revision{};
        {
            std::lock_guard lock(m_mutex);
            if (Result<void> validation = ValidateDraftCandidate(m_schema, draft, m_active->revision); validation.HasError())
                return validation;
            auto candidate = std::make_shared<ConfigurationSnapshot::Data>(*m_active);
            for (const auto &[key, value] : draft.proposedValues) {
                const SettingDescriptor *descriptor = m_schema.FindDescriptor(key);
                HORO_INVARIANT_MSG(descriptor != nullptr, "Validated configuration drafts must reference registered settings.");
                candidate->values[key] = {.value = value,
                                          .source = ConfigurationSource::Session,
                                          .location = std::nullopt,
                                          .sensitivity = descriptor->sensitivity};
            }
            candidate->revision = m_active->revision + 1;
            revision = candidate->revision;
            m_active = std::move(candidate);
        }
        if (m_events != nullptr) {
            ConfigurationChangedEvent event{.revision = revision};
            event.changedKeys.reserve(draft.proposedValues.size());
            for (const auto &[key, unused] : draft.proposedValues) {
                static_cast<void>(unused);
                event.changedKeys.push_back(key);
            }
            std::ranges::sort(event.changedKeys, {}, [](const SettingKey &key) -> const std::string & {
                return key.Value();
            });
            m_events->Publish(event);
        }
        return Result<void>::Success();
    }

    /** @copydoc ConfigurationService::ResolveAndCommit */
    Result<void> ConfigurationService::ResolveAndCommit(const ConfigurationResolutionRequest &request, const ConfigurationLimits &limits) {
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
        {
            std::lock_guard lock(m_mutex);
            if (m_active->revision != before.Revision())
                return Result<void>::Failure(ConfigurationSchema::ErrorFor(ConfigurationErrors::DraftStale));
            m_active = resolved.Value().m_data;
        }
        if (m_events != nullptr) {
            std::ranges::sort(changed, {}, [](const SettingKey &key) -> const std::string & {
                return key.Value();
            });
            m_events->Publish(ConfigurationChangedEvent{.revision = before.Revision() + 1,
                                                        .domain = ConfigurationDomain::All,
                                                        .changedKeys = std::move(changed)});
        }
        return Result<void>::Success();
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
