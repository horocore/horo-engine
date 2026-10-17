#include "Horo/Application/NavigationProjectSettings.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace Horo::Application {
    using namespace Navigation;

    /** @copydoc NavigationProjectSettings::Create */
    Result<NavigationProjectSettings> NavigationProjectSettings::Create(NavigationProjectSettingsInput input) {
        if (!input.revision || !input.definition.IsValid() || input.defaultProvider.empty() || input.defaultProvider.size() > 128 ||
            !std::ranges::all_of(input.defaultProvider, [](const unsigned char value) {
            return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '.' || value == '_' || value == '-';
        }))
            return Result<NavigationProjectSettings>::Failure(MakeError(NavigationSettingsError(NavigationSettingsFailure::Invalid)));
        auto profile = NavigationProjectProfile::Create(input.runtimeProfile);
        if (profile.HasError())
            return Result<NavigationProjectSettings>::Failure(profile.ErrorValue());
        return Result<NavigationProjectSettings>::Success({std::move(input), std::move(profile).Value()});
    }

    /** @copydoc CheckNavigationProjectCompatibility */
    Result<void> CheckNavigationProjectCompatibility(const NavigationProjectSettings &settings,
                                                     const NavigationTargetProviderEvidence &evidence) {
        if (evidence.target.empty() || evidence.target.size() > 128 || evidence.provider != settings.Input().defaultProvider)
            return Result<void>::Failure(MakeError(NavigationSettingsError(NavigationSettingsFailure::Invalid)));
        return AdmitNavigationProjectProfile(settings.RuntimeProfile(), evidence.capabilities, evidence.capabilities.revision);
    }
}  // namespace Horo::Application
