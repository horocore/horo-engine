#include "Horo/Application/PlatformServicesAuthoring.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <vector>

namespace Horo::Application {
    using namespace PlatformServices;

    namespace {
        using Json = nlohmann::ordered_json;
        constexpr std::string_view Algorithm = "horo.platform-services.stable-id.v1";
        constexpr std::size_t MaximumPolicyDocumentBytes = 64U * 1024U;

        /** @brief Adds safe source/field context without echoing private or malformed authoring values. */
        Error AuthoringError(const ErrorCodeDescriptor &descriptor, const char *source, const char *path, const char *reason) {
            auto error = MakeError(descriptor);
            error.diagnostics.push_back({.code = DiagnosticCode{descriptor.code.Value()},
                                         .message = std::string{reason} + " " + std::string{descriptor.remediationHint},
                                         .location = {.source = source},
                                         .path = path});
            return error;
        }

        /** @brief Preserves the identity validator's failure and augments missing document context. */
        Result<PlatformStableIdRegistry> LedgerResult(Result<PlatformStableIdRegistry> result) {
            if (result.HasValue() || !result.ErrorValue().diagnostics.empty())
                return result;
            auto error = std::move(result).ErrorValue();
            error.diagnostics.push_back(
                {.code = DiagnosticCode{error.code.Value()},
                 .message = "Restore the matching committed root salt and ledger; do not regenerate IDs as a repair.",
                 .location = {.source = ".horo/platform_services.ids.json"}});
            return Result<PlatformStableIdRegistry>::Failure(std::move(error));
        }

        /** @brief Requires every member of a closed schema before accessing any field. */
        bool Fields(const Json &value, const std::initializer_list<std::string_view> fields) {
            return value.is_object() && value.size() == fields.size() && std::ranges::all_of(fields, [&](const auto field) {
                return value.contains(field);
            });
        }

        /** @brief Reads only bounded string values; numbers never stand in for stable IDs. */
        std::string Text(const Json &value, const std::size_t maximum) {
            if (!value.is_string() || value.get_ref<const std::string &>().size() > maximum)
                throw std::invalid_argument("bounded text required");
            return value.get<std::string>();
        }

        /** @brief Rejects negative, fractional and unknown integer kind/version representations. */
        std::uint32_t Number(const Json &value, const std::uint32_t maximum) {
            if (!value.is_number_unsigned())
                throw std::invalid_argument("unsigned integer required");
            const auto number = value.get<std::uint64_t>();
            if (number > maximum)
                throw std::invalid_argument("integer capacity exceeded");
            return static_cast<std::uint32_t>(number);
        }

        /** @brief Parses bounded JSON while rejecting duplicate members and deep nesting before projection. */
        Json Document(const std::string_view source) {
            std::vector<std::set<std::string>> keys;
            std::size_t keyCount{};
            const auto callback = [&](const int depth, const Json::parse_event_t event, const Json &value) {
                if (depth > 8)
                    throw std::invalid_argument("depth exceeded");
                if (event == Json::parse_event_t::object_start)
                    keys.emplace_back();
                if (event == Json::parse_event_t::key) {
                    const auto &key = value.get_ref<const std::string &>();
                    if (key.size() > 64 || ++keyCount > 32768)
                        throw std::invalid_argument("member capacity exceeded");
                    if (!keys.back().insert(key).second)
                        throw std::invalid_argument("duplicate member");
                }
                if (event == Json::parse_event_t::object_end)
                    keys.pop_back();
                return true;
            };
            return Json::parse(source, callback);
        }

        /** @brief Reads one complete declaration while preserving tombstone and alias history. */
        PlatformStableIdDeclaration Entry(const Json &value) {
            if (!Fields(value, {"kind", "canonicalKey", "id", "aliases", "state", "removalProvenance"}) || !value["aliases"].is_array() ||
                value["aliases"].size() > 16)
                throw std::invalid_argument("complete bounded entry required");
            const auto id = ParsePlatformServiceStableId(Text(value["id"], 21));
            if (id.HasError())
                throw std::invalid_argument("canonical stable ID required");
            const auto state = Text(value["state"], 10);
            if (state != "active" && state != "tombstoned")
                throw std::invalid_argument("known identity state required");
            PlatformStableIdDeclaration result{.kind = static_cast<PlatformServiceIdKind>(Number(value["kind"], 4)),
                                               .canonicalKey = Text(value["canonicalKey"], 96),
                                               .storedId = id.Value(),
                                               .state =
                                                   state == "active" ? PlatformStableIdState::Active : PlatformStableIdState::Tombstoned,
                                               .removalProvenance = Text(value["removalProvenance"], 256)};
            for (const auto &alias : value["aliases"])
                result.aliases.push_back(Text(alias, 96));
            return result;
        }
    }  // namespace

    /** @copydoc DecodePlatformServicesLedger */
    Result<PlatformStableIdRegistry> DecodePlatformServicesLedger(const std::string_view source, const std::string_view projectId,
                                                                  const PlatformServicesIdSalt &salt) {
        using Output = Result<PlatformStableIdRegistry>;
        if (!salt.IsValid())
            return Output::Failure(AuthoringError(StableIdErrors::InvalidSalt, ".horo/project.json", "platformServicesIdSalt",
                                                  "The captured root salt is missing or zero."));
        if (source.empty() || source.size() > PlatformStableIdRegistryMaximumDocumentBytes || projectId.empty() || projectId.size() > 128)
            return Output::Failure(AuthoringError(StableIdErrors::InvalidLedger, ".horo/platform_services.ids.json", "",
                                                  "A nonempty ledger of at most 4 MiB and a bounded root owner are required."));
        try {
            const auto root = Document(source);
            if (!Fields(root, {"schemaVersion", "projectId", "algorithm", "entries"}) || Number(root["schemaVersion"], 1) != 1 ||
                Text(root["projectId"], 128) != projectId || Text(root["algorithm"], 64) != Algorithm || !root["entries"].is_array() ||
                root["entries"].size() > 4096)
                return Output::Failure(AuthoringError(StableIdErrors::InvalidLedger, ".horo/platform_services.ids.json", "",
                                                      "Check required members, owner, version, algorithm and bounded entries."));
            PlatformStableIdRegistryCandidate candidate{.projectId = std::string{projectId}, .salt = salt};
            candidate.entries.reserve(root["entries"].size());
            for (const auto &entry : root["entries"])
                candidate.entries.push_back(Entry(entry));
            return LedgerResult(BuildPlatformStableIdRegistry(candidate));
        } catch (const Json::exception &) {
            return Output::Failure(AuthoringError(StableIdErrors::InvalidLedger, ".horo/platform_services.ids.json", "",
                                                  "Use one complete UTF-8 JSON document without trailing data."));
        } catch (const std::invalid_argument &) {
            return Output::Failure(AuthoringError(StableIdErrors::InvalidLedger, ".horo/platform_services.ids.json", "",
                                                  "Check unique members, nesting bounds, canonical ID text and complete entry fields."));
        }
    }

    /** @copydoc EncodePlatformServicesLedger */
    Result<std::string> EncodePlatformServicesLedger(const PlatformStableIdRegistry &registry) {
        return registry.CanonicalDocument();
    }

    /** @copydoc ResolveProjectPlatformServicesSalt */
    Result<PlatformServicesIdSalt> ResolveProjectPlatformServicesSalt(const ProjectSourceDocument &source) {
        using Output = Result<PlatformServicesIdSalt>;
        const auto invalid = [] {
            return Output::Failure(AuthoringError(StableIdErrors::InvalidSalt, ".horo/project.json", "platformServicesIdSalt",
                                                  "Use canonical psid1 text from the committed root authority; loading never repairs it."));
        };
        if (!source.platformServicesIdSaltSource || source.platformServicesIdSaltSource->size() > 256)
            return Output::Failure(AuthoringError(StableIdErrors::InvalidSalt, ".horo/project.json", "platformServicesIdSalt",
                                                  "Required root metadata is absent or oversized; use the explicit project migration."));
        try {
            auto parsed = ParsePlatformServicesIdSalt(Text(Document(*source.platformServicesIdSaltSource), 38));
            if (parsed.HasValue())
                return parsed;
            return invalid();
        } catch (const Json::exception &) {
            return invalid();
        } catch (const std::invalid_argument &) {
            return invalid();
        }
    }

    /** @copydoc ResolveProjectPlatformServicesConfiguration */
    Result<PlatformProjectConfiguration> ResolveProjectPlatformServicesConfiguration(
        const ProjectSourceDocument &source, const PlatformServicesHostProfile profile,
        const std::span<const PlatformProviderModuleContribution> contributions, const std::span<const ModuleId> trustedModules,
        const PlatformProjectConfigurationLimits &limits) {
        using Output = Result<PlatformProjectConfiguration>;
        if (const auto salt = ResolveProjectPlatformServicesSalt(source); salt.HasError())
            return Output::Failure(salt.ErrorValue());
        if (!source.platformServicesSettingsSource || source.platformServicesSettingsSource->empty() ||
            source.platformServicesSettingsSource->size() > MaximumPolicyDocumentBytes)
            return Output::Failure(AuthoringError(PlatformProjectConfigurationErrors::InvalidConfiguration, ".horo/project.json",
                                                  "settings.platformServices", "An explicit bounded project policy is required."));
        try {
            const auto root = Document(*source.platformServicesSettingsSource);
            if (!Fields(root, {"schemaVersion", "provider", "services"}) || !Fields(root["provider"], {"mode", "key"}) ||
                !root["services"].is_array() || root["services"].size() != static_cast<std::size_t>(PlatformServiceKind::Count))
                return Output::Failure(AuthoringError(PlatformProjectConfigurationErrors::InvalidConfiguration, ".horo/project.json",
                                                      "settings.platformServices",
                                                      "Provide the complete closed provider and service policy."));
            if (Number(root["schemaVersion"], UINT32_MAX) != PlatformProjectConfigurationSchemaVersion)
                return Output::Failure(AuthoringError(PlatformProjectConfigurationErrors::UnsupportedVersion, ".horo/project.json",
                                                      "settings.platformServices.schemaVersion",
                                                      "Use an explicit supported project migration."));
            const auto mode = Text(root["provider"]["mode"], 5);
            if (mode != "null" && mode != "exact")
                return Output::Failure(AuthoringError(PlatformProjectConfigurationErrors::InvalidConfiguration, ".horo/project.json",
                                                      "settings.platformServices.provider.mode",
                                                      "Choose explicit null or exact provider selection."));
            PlatformProjectConfigurationCandidate candidate{.projectId = source.metadata.projectId, .profile = profile};
            candidate.provider = {.mode =
                                      mode == "null" ? PlatformProviderSelectionMode::Null : PlatformProviderSelectionMode::ExactProvider,
                                  .providerKey = Text(root["provider"]["key"], MaximumPolicyDocumentBytes)};
            for (std::size_t index = 0; index < candidate.services.size(); ++index)
                candidate.services[index] = static_cast<PlatformServiceRequirement>(Number(root["services"][index], 2));
            return BuildPlatformProjectConfiguration(candidate, contributions, trustedModules, limits);
        } catch (const Json::exception &) {
            return Output::Failure(AuthoringError(PlatformProjectConfigurationErrors::InvalidConfiguration, ".horo/project.json",
                                                  "settings.platformServices", "Use one complete UTF-8 project policy JSON value."));
        } catch (const std::invalid_argument &) {
            return Output::Failure(AuthoringError(PlatformProjectConfigurationErrors::InvalidConfiguration, ".horo/project.json",
                                                  "settings.platformServices",
                                                  "Check unique members, enum ranges, text and nesting bounds."));
        }
    }

    /** @copydoc EncodeProjectPlatformServicesConfiguration */
    Result<std::string> EncodeProjectPlatformServicesConfiguration(const PlatformProjectConfiguration &configuration) {
        if (configuration.ProjectId().empty())
            return Result<std::string>::Failure(AuthoringError(PlatformProjectConfigurationErrors::InvalidConfiguration,
                                                               ".horo/project.json", "settings.platformServices",
                                                               "Validate complete project policy before persistence."));
        auto services = Json::array();
        for (const auto requirement : configuration.ServiceRequirements())
            services.push_back(static_cast<std::uint8_t>(requirement));
        return Result<std::string>::Success(
            Json{{"schemaVersion", 1},
                 {"provider",
                  {{"mode", configuration.UsesNullProvider() ? "null" : "exact"}, {"key", configuration.SelectedProviderKey()}}},
                 {"services", std::move(services)}}
                .dump(2));
    }
}  // namespace Horo::Application
