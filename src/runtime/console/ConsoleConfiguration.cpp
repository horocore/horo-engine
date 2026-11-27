#include "Horo/Debug/ConsoleConfiguration.h"

namespace Horo::Debug {
    namespace {
        constexpr auto PreferenceSources = ConfigurationSourceMask::Invocation | ConfigurationSourceMask::Environment |
                                           ConfigurationSourceMask::Session | ConfigurationSourceMask::Project |
                                           ConfigurationSourceMask::User | ConfigurationSourceMask::PackagedProfile;

        /** @brief Reads primitive public values directly from the canonical snapshot. */
        template <typename T> Result<T> Read(const ConfigurationSnapshotRef &snapshot, const SettingKey &key) {
            const auto *entry = snapshot.FindResolved(key);
            if (!entry)
                return Result<T>::Failure(MakeCommandError(CommandError::MissingCapability));
            if (entry->sensitivity != SettingSensitivity::Public)
                return Result<T>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
            const auto *value = std::get_if<T>(&entry->value);
            if (!value)
                return Result<T>::Failure(MakeCommandError(CommandError::Usage));
            return Result<T>::Success(*value);
        }

        /** @brief Constructs an inert primitive setting with explicit legal sources and activation semantics. */
        SettingDescriptor Descriptor(SettingKey key, SettingValue value, SettingScope scope, ReloadPolicy reload,
                                     ConfigurationSourceMask sources = PreferenceSources) {
            auto type = SettingValueType::String;
            if (std::holds_alternative<bool>(value))
                type = SettingValueType::Boolean;
            else if (std::holds_alternative<std::int64_t>(value))
                type = SettingValueType::Integer;
            return {std::move(key),
                    type,
                    std::move(value),
                    scope,
                    reload,
                    SettingSensitivity::Public,
                    ConfigurationSourcePolicy{sources, false}};
        }
    }  // namespace

    /** @copydoc ConsoleOverlayVisibilityKey */
    std::optional<SettingKey> ConsoleOverlayVisibilityKey(OverlaySource source) {
        const auto name = OverlaySourceName(source);
        if (name.empty())
            return std::nullopt;
        return SettingKey("runtime.overlays." + std::string(name) + ".visible");
    }

    /** @copydoc ConsoleConfigurationDescriptors */
    Result<std::vector<SettingDescriptor>> ConsoleConfigurationDescriptors(ProductProfile profile) {
        if (static_cast<unsigned>(profile) > static_cast<unsigned>(ProductProfile::Server))
            return Result<std::vector<SettingDescriptor>>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        std::vector<SettingDescriptor> descriptors;
        descriptors.reserve(4 + static_cast<std::size_t>(OverlaySource::Count));
        descriptors.push_back(Descriptor(SettingKey(std::string(ConsoleEnabledSettingName)), profile == ProductProfile::Development,
                                         SettingScope::Project, ReloadPolicy::ProcessRestart));
        descriptors.push_back(
            Descriptor(SettingKey(std::string(ConsoleHistorySettingName)), std::int64_t{500}, SettingScope::User, ReloadPolicy::NextFrame));
        descriptors.push_back(Descriptor(SettingKey(std::string(ConsoleStartupSettingName)), true, SettingScope::Project,
                                         ReloadPolicy::ProcessRestart,
                                         ConfigurationSourceMask::Project | ConfigurationSourceMask::PackagedProfile));
        descriptors.push_back(
            Descriptor(SettingKey(std::string(OverlaysEnabledSettingName)), false, SettingScope::User, ReloadPolicy::ProcessRestart));
        for (unsigned i = 0; i < static_cast<unsigned>(OverlaySource::Count); ++i) {
            const auto source = static_cast<OverlaySource>(i);
            const bool visible = source == OverlaySource::Logs || source == OverlaySource::Frame || source == OverlaySource::Memory ||
                                 source == OverlaySource::Jobs;
            descriptors.push_back(Descriptor(*ConsoleOverlayVisibilityKey(source), visible, SettingScope::User, ReloadPolicy::NextFrame));
        }
        return Result<std::vector<SettingDescriptor>>::Success(std::move(descriptors));
    }

    /** @copydoc ReadConsoleHistoryLimit */
    Result<std::size_t> ReadConsoleHistoryLimit(const ConfigurationSnapshotRef &snapshot) {
        const auto history = Read<std::int64_t>(snapshot, SettingKey(std::string(ConsoleHistorySettingName)));
        if (history.HasError())
            return Result<std::size_t>::Failure(history.ErrorValue());
        if (history.Value() < 1 || history.Value() > 10000)
            return Result<std::size_t>::Failure(MakeCommandError(CommandError::Usage));
        return Result<std::size_t>::Success(static_cast<std::size_t>(history.Value()));
    }

    /** @copydoc ReadConsoleConfiguration */
    Result<ConsoleConfigurationDefaults> ReadConsoleConfiguration(const ConfigurationSnapshotRef &snapshot) {
        const auto console = Read<bool>(snapshot, SettingKey(std::string(ConsoleEnabledSettingName)));
        const auto history = ReadConsoleHistoryLimit(snapshot);
        const auto startup = Read<bool>(snapshot, SettingKey(std::string(ConsoleStartupSettingName)));
        const auto overlays = Read<bool>(snapshot, SettingKey(std::string(OverlaysEnabledSettingName)));
        if (console.HasError())
            return Result<ConsoleConfigurationDefaults>::Failure(console.ErrorValue());
        if (history.HasError())
            return Result<ConsoleConfigurationDefaults>::Failure(history.ErrorValue());
        if (startup.HasError())
            return Result<ConsoleConfigurationDefaults>::Failure(startup.ErrorValue());
        if (overlays.HasError())
            return Result<ConsoleConfigurationDefaults>::Failure(overlays.ErrorValue());
        ConsoleConfigurationDefaults result{snapshot.Revision(), console.Value(), overlays.Value(), startup.Value(), history.Value(), {}};
        for (unsigned i = 0; i < static_cast<unsigned>(OverlaySource::Count); ++i) {
            const auto source = static_cast<OverlaySource>(i);
            const auto visible = Read<bool>(snapshot, *ConsoleOverlayVisibilityKey(source));
            if (visible.HasError())
                return Result<ConsoleConfigurationDefaults>::Failure(visible.ErrorValue());
            if (visible.Value())
                result.panels.push_back(source);
        }
        return Result<ConsoleConfigurationDefaults>::Success(std::move(result));
    }
}  // namespace Horo::Debug
