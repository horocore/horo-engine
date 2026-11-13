#include "Horo/PlatformServices/PlatformProjectSettings.h"

namespace Horo::PlatformServices {
    namespace {
        constexpr const char *ProviderModeKey = "platform.services.provider.mode";
        constexpr const char *ProviderKey = "platform.services.provider.key";
        constexpr std::array RequirementKeys{"platform.services.achievements", "platform.services.leaderboards_and_stats",
                                             "platform.services.cloud",        "platform.services.presence",
                                             "platform.services.friends",      "platform.services.session"};
        static_assert(RequirementKeys.size() == static_cast<std::size_t>(PlatformServiceKind::Count));

        /** @brief Declares one inert portable setting with no ambient source or lifecycle authority. */
        SettingDescriptor Descriptor(const char *key, const SettingValueType type, SettingValue value) {
            return {.key = SettingKey{key},
                    .type = type,
                    .defaultValue = std::move(value),
                    .scope = SettingScope::Project,
                    .reloadPolicy = ReloadPolicy::ProjectReopen,
                    .sensitivity = SettingSensitivity::Public,
                    .sourcePolicy = ConfigurationSourcePolicy{.allowedSources = ConfigurationSourceMask::Project}};
        }

        /** @brief Reports an exact contributed key rather than echoing untrusted values. */
        Error InvalidSetting(const char *key) {
            auto error = MakeError(PlatformProjectConfigurationErrors::InvalidConfiguration);
            error.diagnostics.push_back({.code = DiagnosticCode{error.code.Value()},
                                         .message = "Provide a complete bounded Platform Services project setting.",
                                         .location = {.source = ".horo/project.json"},
                                         .path = key});
            return error;
        }

        /** @brief Reads an enum representation before any narrowing conversion. */
        Result<std::int64_t> Integer(const ConfigurationSnapshot &snapshot, const char *key, const std::int64_t maximum) {
            const auto value = snapshot.Find(SettingKey{key});
            if (!value)
                return Result<std::int64_t>::Failure(InvalidSetting(key));
            const auto *number = std::get_if<std::int64_t>(&*value);
            if (!number || *number < 0 || *number > maximum)
                return Result<std::int64_t>::Failure(InvalidSetting(key));
            return Result<std::int64_t>::Success(*number);
        }
    }  // namespace

    /** @copydoc MakePlatformProjectSettingsContribution */
    ModuleConfigurationContribution MakePlatformProjectSettingsContribution() {
        ModuleConfigurationContribution contribution{.module = ModuleId{"horo.platform.services"},
                                                     .ownerPrefix = "platform.services",
                                                     .settings = {Descriptor(ProviderModeKey, SettingValueType::Integer, std::int64_t{0}),
                                                                  Descriptor(ProviderKey, SettingValueType::String, std::string{})}};
        for (const auto *key : RequirementKeys)
            contribution.settings.push_back(Descriptor(key, SettingValueType::Integer, std::int64_t{0}));
        return contribution;
    }

    /** @copydoc PlatformProjectSettingsValues */
    Result<ConfigurationSourceMap> PlatformProjectSettingsValues(const PlatformProjectConfiguration &configuration) {
        if (configuration.ProjectId().empty())
            return Result<ConfigurationSourceMap>::Failure(InvalidSetting("projectId"));
        ConfigurationSourceMap values;
        values.emplace(SettingKey{ProviderModeKey},
                       ConfigurationInputValue{.value = std::int64_t{configuration.UsesNullProvider() ? 0 : 1}});
        values.emplace(SettingKey{ProviderKey}, ConfigurationInputValue{.value = std::string{configuration.SelectedProviderKey()}});
        const auto requirements = configuration.ServiceRequirements();
        for (std::size_t index = 0; index < RequirementKeys.size(); ++index)
            values.emplace(SettingKey{RequirementKeys[index]},
                           ConfigurationInputValue{.value = static_cast<std::int64_t>(requirements[index])});
        return Result<ConfigurationSourceMap>::Success(std::move(values));
    }

    /** @copydoc ResolvePlatformProjectSettings */
    Result<PlatformProjectConfiguration> ResolvePlatformProjectSettings(
        const ConfigurationSnapshot &snapshot, const std::string_view projectId, const PlatformServicesHostProfile profile,
        const std::span<const PlatformProviderModuleContribution> contributions, const std::span<const ModuleId> trustedModules,
        const PlatformProjectConfigurationLimits &limits) {
        using Output = Result<PlatformProjectConfiguration>;
        const auto mode = Integer(snapshot, ProviderModeKey, 1);
        if (mode.HasError())
            return Output::Failure(mode.ErrorValue());
        const auto keyValue = snapshot.Find(SettingKey{ProviderKey});
        const auto *providerKey = keyValue ? std::get_if<std::string>(&*keyValue) : nullptr;
        if (!providerKey)
            return Output::Failure(InvalidSetting(ProviderKey));
        PlatformProjectConfigurationCandidate candidate{.projectId = std::string{projectId},
                                                        .profile = profile,
                                                        .provider = {.mode = static_cast<PlatformProviderSelectionMode>(mode.Value()),
                                                                     .providerKey = *providerKey}};
        for (std::size_t index = 0; index < RequirementKeys.size(); ++index) {
            const auto requirement = Integer(snapshot, RequirementKeys[index], 2);
            if (requirement.HasError())
                return Output::Failure(requirement.ErrorValue());
            candidate.services[index] = static_cast<PlatformServiceRequirement>(requirement.Value());
        }
        return BuildPlatformProjectConfiguration(candidate, contributions, trustedModules, limits);
    }
}  // namespace Horo::PlatformServices
