#include "Horo/Foundation/ModuleHost.h"
#include "foundation/FoundationErrors.h"

#include <algorithm>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace Horo {
    namespace {
        [[nodiscard]] bool Owns(const std::string_view prefix, const std::string_view key) {
            return key.size() > prefix.size() && key.starts_with(prefix) && key[prefix.size()] == '.';
        }

        [[nodiscard]] bool Overlaps(const std::string_view left, const std::string_view right) {
            return left == right || Owns(left, right) || Owns(right, left);
        }

        [[nodiscard]] bool ValidPrefix(const std::string_view prefix) {
            return !prefix.empty() && prefix.front() != '.' && prefix.back() != '.' && prefix.find("..") == std::string_view::npos &&
                   std::ranges::all_of(prefix, [](const char character) {
                return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_' ||
                       character == '.';
            });
        }

        [[nodiscard]] bool ValidBinding(const std::string_view variable) {
            return variable.starts_with("HORO_") && variable.size() > 5 &&
                   std::ranges::all_of(variable.substr(5), [](const char character) {
                return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '_';
            });
        }

        [[nodiscard]] Result<void> ContributionFailure(const ErrorCodeDescriptor &code, const std::string &detail) {
            return Result<void>::Failure(MakeError(code, detail));
        }

        /** @brief Validates settings and environment bindings without changing host state. */
        [[nodiscard]] Result<void> ValidateContributionMetadata(const ModuleConfigurationContribution &contribution) {
            ConfigurationSchema localSchema;
            std::set<std::string, std::less<>> keys;
            for (const SettingDescriptor &setting : contribution.settings) {
                const std::string &key = setting.key.Value();
                if (!Owns(contribution.ownerPrefix, key) || !setting.sourcePolicy.has_value())
                    return ContributionFailure(ModuleDescriptorErrors::InvalidSettingsContribution,
                                               "Setting '" + key + "' is outside its owner or lacks a source policy.");
                if (!keys.emplace(key).second)
                    return ContributionFailure(ModuleDescriptorErrors::DuplicateSetting, "Setting '" + key + "' is duplicated.");
                if (const Result<void> valid = localSchema.Register(setting); valid.HasError())
                    return ContributionFailure(ModuleDescriptorErrors::InvalidSettingsContribution,
                                               "Setting '" + key + "' has invalid schema metadata.");
            }
            std::set<std::string, std::less<>> variables;
            std::set<std::string, std::less<>> boundKeys;
            for (const EnvironmentVariableBinding &binding : contribution.environmentBindings) {
                if (const SettingDescriptor *setting = localSchema.FindDescriptor(binding.key);
                    !ValidBinding(binding.variable) || setting == nullptr ||
                    (static_cast<std::uint16_t>(setting->sourcePolicy->allowedSources) &
                     static_cast<std::uint16_t>(ConfigurationSourceMask::Environment)) == 0 ||
                    !boundKeys.emplace(binding.key.Value()).second)
                    return ContributionFailure(ModuleDescriptorErrors::InvalidSettingsContribution,
                                               "Environment binding '" + binding.variable + "' is invalid or names an unowned setting.");
                if (!variables.emplace(binding.variable).second)
                    return ContributionFailure(ModuleDescriptorErrors::DuplicateEnvironmentBinding,
                                               "Environment binding '" + binding.variable + "' is duplicated.");
            }
            return Result<void>::Success();
        }

        /** @brief Checks an incoming contribution against one live owner's settings. */
        [[nodiscard]] Result<void> ValidateContributionConflicts(const ModuleConfigurationContribution &existing,
                                                                 const ModuleConfigurationContribution &incoming) {
            for (const SettingDescriptor &setting : incoming.settings) {
                if (std::ranges::any_of(existing.settings, [&setting](const SettingDescriptor &other) {
                    return other.key == setting.key;
                }))
                    return ContributionFailure(ModuleDescriptorErrors::DuplicateSetting,
                                               "Setting '" + setting.key.Value() + "' is already registered.");
            }
            for (const EnvironmentVariableBinding &binding : incoming.environmentBindings) {
                if (std::ranges::any_of(existing.environmentBindings, [&binding](const EnvironmentVariableBinding &other) {
                    return other.variable == binding.variable;
                }))
                    return ContributionFailure(ModuleDescriptorErrors::DuplicateEnvironmentBinding,
                                               "Environment binding '" + binding.variable + "' is already registered.");
            }
            if (Overlaps(existing.ownerPrefix, incoming.ownerPrefix))
                return ContributionFailure(ModuleDescriptorErrors::SettingOwnerConflict,
                                           "Settings owners '" + existing.module.value + "' and '" + incoming.module.value + "' overlap.");
            return Result<void>::Success();
        }

        [[nodiscard]] bool HasRepeatedDependencies(const ModuleDescriptor &descriptor) {
            std::set<std::string, std::less<>> seen;
            return std::ranges::any_of(descriptor.dependencies, [&seen](const ModuleDependency &dep) {
                return !seen.emplace(dep.module.value).second;
            });
        }
    }  // namespace

    /** @copydoc ModuleHost::Register */
    Result<void> ModuleHost::Register(const ModuleDescriptor &descriptor) {
        // Registration is inert: local metadata is checked here, but graph-wide rules
        // (duplicates across the set, missing providers, cycles) stay in ActivateRegistered
        // so that registration never depends on the full set's state.
        if (HasRepeatedDependencies(descriptor))
            return Result<void>::Failure(MakeError(ModuleDescriptorErrors::InvalidDescriptor,
                                                   "Module '" + descriptor.id.value + "' repeats a dependency at registration."));
        if (StateOf(descriptor.id).has_value())
            return Result<void>::Failure(
                MakeError(ModuleDescriptorErrors::DuplicateModule, "Module '" + descriptor.id.value + "' already has a host lifetime."));
        m_registered.push_back(descriptor);
        m_states.push_back(ModuleStateRecord{.id = descriptor.id});
        return Result<void>::Success();
    }

    /** @copydoc ModuleHost::Register */
    Result<void> ModuleHost::Register(const ModuleDescriptor &descriptor, const ModuleConfigurationContribution &contribution) {
        if (contribution.module != descriptor.id || !ValidPrefix(contribution.ownerPrefix))
            return ContributionFailure(ModuleDescriptorErrors::InvalidSettingsContribution,
                                       "Settings contribution owner does not match module '" + descriptor.id.value + "'.");
        if (const Result<void> valid = ValidateContributionMetadata(contribution); valid.HasError())
            return valid;
        for (const ModuleConfigurationContribution &existing : m_configurationContributions) {
            if (StateOf(existing.module) == ModuleLifecycleState::Stopped || StateOf(existing.module) == ModuleLifecycleState::Failed)
                continue;
            if (const Result<void> conflicts = ValidateContributionConflicts(existing, contribution); conflicts.HasError())
                return conflicts;
        }
        std::size_t settingCount = contribution.settings.size();
        std::size_t bindingCount = contribution.environmentBindings.size();
        for (const ModuleConfigurationContribution &existing : m_configurationContributions) {
            if (StateOf(existing.module) == ModuleLifecycleState::Active || StateOf(existing.module) == ModuleLifecycleState::Registered) {
                settingCount += existing.settings.size();
                bindingCount += existing.environmentBindings.size();
            }
        }
        if (const ConfigurationLimits limits;
            settingCount > limits.maximumKeysPerSource || bindingCount > limits.maximumEnvironmentBindings)
            return ContributionFailure(ModuleDescriptorErrors::InvalidSettingsContribution,
                                       "Module settings exceed the shared configuration registration limits.");
        if (const Result<void> registered = Register(descriptor); registered.HasError())
            return registered;
        m_configurationContributions.push_back(contribution);
        return Result<void>::Success();
    }

    /** @copydoc ModuleHost::BuildConfigurationSchema */
    Result<ConfigurationSchema> ModuleHost::BuildConfigurationSchema() const {
        ConfigurationSchema schema;
        std::vector<const ModuleConfigurationContribution *> active;
        for (const ModuleConfigurationContribution &contribution : m_configurationContributions) {
            if (StateOf(contribution.module) == ModuleLifecycleState::Active)
                active.push_back(&contribution);
        }
        std::ranges::sort(active, {}, [](const ModuleConfigurationContribution *contribution) {
            return contribution->module.value;
        });
        for (const ModuleConfigurationContribution *contribution : active) {
            std::vector<const SettingDescriptor *> settings;
            for (const SettingDescriptor &setting : contribution->settings)
                settings.push_back(&setting);
            std::ranges::sort(settings, {}, [](const SettingDescriptor *setting) {
                return setting->key.Value();
            });
            for (const SettingDescriptor *setting : settings) {
                if (const Result<void> registered = schema.Register(*setting); registered.HasError())
                    return Result<ConfigurationSchema>::Failure(registered.ErrorValue());
            }
        }
        return Result<ConfigurationSchema>::Success(std::move(schema));
    }

    /** @copydoc ModuleHost::ConfigurationEnvironmentBindings */
    std::vector<EnvironmentVariableBinding> ModuleHost::ConfigurationEnvironmentBindings() const {
        std::vector<EnvironmentVariableBinding> bindings;
        for (const ModuleConfigurationContribution &contribution : m_configurationContributions) {
            if (StateOf(contribution.module) == ModuleLifecycleState::Active)
                bindings.insert(bindings.end(), contribution.environmentBindings.begin(), contribution.environmentBindings.end());
        }
        std::ranges::sort(bindings, {}, &EnvironmentVariableBinding::variable);
        return bindings;
    }
}  // namespace Horo
