#pragma once
/** @file NavigationProjectSettings.h
 * @brief Project navigation policy; definition-owned authoring data and user overlays are deliberately absent.
 */
#include "Horo/Assets/AssetId.h"
#include "Horo/Navigation/NavigationProjectProfiles.h"
#include "Horo/Navigation/NavigationSettingsErrors.h"

#include <string>
#include <string_view>

namespace Horo::Application {
    /** @brief One versioned project policy; profiles, areas and filters remain in the referenced definition asset. */
    struct NavigationProjectSettingsInput final {
        std::uint64_t revision{};
        Assets::AssetId definition;
        std::string defaultProvider; /**< Explicit host provider identity, never provider discovery or fallback. */
        Navigation::NavigationProjectProfileInput runtimeProfile;
    };

    /** @brief Immutable validated project authority, independent of developer preview/overlay preferences. */
    class NavigationProjectSettings final {
    public:
        /** @brief Validates the complete candidate. @param input Detached policy. @return Owned policy or typed failure. */
        [[nodiscard]] static Result<NavigationProjectSettings> Create(NavigationProjectSettingsInput input);

        /** @brief Returns exact authored values. @return Borrowed immutable policy. */
        [[nodiscard]] const NavigationProjectSettingsInput &Input() const noexcept {
            return input_;
        }

        /** @brief Returns sole validated runtime capacity authority. @return Borrowed profile. */
        [[nodiscard]] const Navigation::NavigationProjectProfile &RuntimeProfile() const noexcept {
            return profile_;
        }

    private:
        NavigationProjectSettings(NavigationProjectSettingsInput input, Navigation::NavigationProjectProfile profile)
            : input_(std::move(input)), profile_(std::move(profile)) {}

        NavigationProjectSettingsInput input_;
        Navigation::NavigationProjectProfile profile_;
    };

    /** @brief Encodes policy schema2 with no editor/local presentation fields. @param settings Validated policy. @return Canonical JSON. */
    [[nodiscard]] std::string EncodeNavigationProjectSettings(const NavigationProjectSettings &settings);
    /** @brief Parses bounded schema2 or explicitly migrates schema1's provider key; rejects unknown fields/versions.
     * @param source Complete UTF8 document, at most64KiB. @return Validated owned project policy; no writes/defaults/activation.
     */
    [[nodiscard]] Result<NavigationProjectSettings> DecodeNavigationProjectSettings(std::string_view source);

    /** @brief Explicit host metadata for a selected target; never a factory or live provider. */
    struct NavigationTargetProviderEvidence final {
        std::string target;
        std::string provider;
        Navigation::NavigationProviderCapabilities capabilities;
    };

    /** @brief Checks the selected exact provider/target capability snapshot without discovery, activation or fallback.
     * @param settings Project authority. @param evidence Host-supplied target metadata. @return Typed admission result.
     */
    [[nodiscard]] Result<void> CheckNavigationProjectCompatibility(const NavigationProjectSettings &settings,
                                                                   const NavigationTargetProviderEvidence &evidence);
}  // namespace Horo::Application
