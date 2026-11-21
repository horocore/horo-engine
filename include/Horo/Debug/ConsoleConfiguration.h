#pragma once

/** @file ConsoleConfiguration.h @brief Inert console settings and typed projection of the canonical immutable configuration. */
#include "Horo/Debug/CommandRegistry.h"
#include "Horo/Debug/ConsoleVariable.h"
#include "Horo/Debug/OverlaySource.h"
#include "Horo/Foundation/Configuration.h"

namespace Horo::Debug {
    /** @brief Stable public session control identity; distinct from the launch-only default visibility preference. */
    inline constexpr std::uint64_t ConsoleVisibleVariableId = 0xDB620001;
    /** @brief Canonical Public session control name used by metadata and host input submission. */
    inline constexpr std::string_view ConsoleVisibleVariableName = "console.visible";
    /** @brief Canonical Developer session controls for the two existing process-store panel adapters. */
    inline constexpr std::string_view ConsoleLogOverlayVariableName = "overlay.logs.visible";
    /** @brief Requested operation-panel visibility, independent of the overlay feature's lifecycle/focus eligibility. */
    inline constexpr std::string_view ConsoleJobsOverlayVariableName = "overlay.jobs.visible";
    /** @brief Requested read-only Scene-panel visibility; meaningful only in a host with a real Scene inspection capability. */
    inline constexpr std::string_view ConsoleSceneOverlayVariableName = "overlay.scene.visible";
    /** @brief Requested immutable Network diagnostic-panel visibility; requires an explicitly supplied application query. */
    inline constexpr std::string_view ConsoleNetworkOverlayVariableName = "overlay.network.visible";
    /** @brief Startup preference, independent of current focus and trusted Shipping admission. */
    inline constexpr std::string_view ConsoleEnabledSettingName = "runtime.console.enabled_by_default";
    /** @brief Canonical retained-entry preference, activated at the host's next frame boundary. */
    inline constexpr std::string_view ConsoleHistorySettingName = "runtime.console.history_limit";
    /** @brief Stable variable identity shared by the product catalog and explicit host control composition. */
    inline constexpr std::uint64_t ConsoleHistoryLimitVariableId = 0xDB620002;
    /** @brief Project/packaged startup permission; user, environment, session and Invocation sources are excluded. */
    inline constexpr std::string_view ConsoleStartupSettingName = "runtime.console.allow_startup_exec";
    /** @brief Startup overlay preference; source requests still require profile admission and actual host capabilities. */
    inline constexpr std::string_view OverlaysEnabledSettingName = "runtime.overlays.enabled_by_default";

    /** @brief Owned launch defaults projected from one snapshot, never an effective configuration authority. */
    struct ConsoleConfigurationDefaults final {
        ConfigurationRevision revision{};
        bool console{};
        bool overlays{};
        bool startupAllowed{};
        std::size_t historyLimit{500};
        std::vector<OverlaySource> panels;
    };

    /** @brief Constructs metadata for explicit host registration before the shared schema is sealed.
     * @param profile Fixed artifact identity; only Development defaults its local console on.
     * @return Owned typed descriptors or an invalid-profile failure. Registers/reads/activates nothing.
     * No permissions, shipping allowlists, authentication or remote endpoints are granted through these settings.
     */
    [[nodiscard]] Result<std::vector<SettingDescriptor>> ConsoleConfigurationDescriptors(ProductProfile profile);
    /** @brief Constructs the one canonical visibility setting identity for a source.
     * @param source Typed snapshot family. @return Owned key or no value for an invalid/sentinel identity.
     */
    [[nodiscard]] std::optional<SettingKey> ConsoleOverlayVisibilityKey(OverlaySource source);
    /** @brief Reads the canonical public history-entry preference without projecting unrelated panel settings.
     * @param snapshot Existing immutable host configuration.
     * @return Validated 1 through 10000 entry budget, or typed missing/type/range/sensitivity failure without fallback.
     * Hosts use this at explicit configuration/frame boundaries, never to infer input or permission grants.
     */
    [[nodiscard]] Result<std::size_t> ReadConsoleHistoryLimit(const ConfigurationSnapshotRef &snapshot);
    /** @brief Constructs the inert NonShipping history variable bridge without creating state or querying configuration.
     * @return Developer-only session request metadata with NextFrame activation through the canonical history setting.
     * The separately linked factory is referenced only by hosts/catalogs that compose this control; Shipping catalogs omit it.
     */
    [[nodiscard]] ConsoleVariableDescriptor ConsoleHistoryVariableDescriptor();
    /** @brief Constructs inert Public/ShippingAllowlist local-console visibility metadata.
     * @return Session Boolean metadata; actual local UI eligibility and Shipping allowlist approval remain host capabilities.
     * Declaring this control does not create a surface, acquire input, or register it into a product catalog.
     */
    [[nodiscard]] ConsoleVariableDescriptor ConsoleVisibilityVariableDescriptor();
    /** @brief Constructs inert Developer/NonShipping log-panel request metadata. @return Session Boolean; creates no log producer/store. */
    [[nodiscard]] ConsoleVariableDescriptor ConsoleLogOverlayVariableDescriptor();
    /** @brief Constructs inert Developer/NonShipping operation-panel request metadata. @return Session Boolean; creates no job/store. */
    [[nodiscard]] ConsoleVariableDescriptor ConsoleJobsOverlayVariableDescriptor();
    /** @brief Constructs inert Developer/DevelopmentOnly Scene-panel metadata. @return NextFrame session Boolean; starts no Scene. */
    [[nodiscard]] ConsoleVariableDescriptor ConsoleSceneOverlayVariableDescriptor();
    /** @brief Constructs inert Developer/NonShipping network-panel metadata. @return Session Boolean; starts no networking. */
    [[nodiscard]] ConsoleVariableDescriptor ConsoleNetworkOverlayVariableDescriptor();
    /** @brief Reads every required setting from the existing resolved snapshot without a second resolver or source precedence.
     * @param snapshot Canonical immutable captured configuration.
     * @return Typed defaults carrying the exact revision, or missing/type/range/sensitivity failure. No ambient reads or fallback.
     * Panel preferences become active requests only through profile-gated launch composition.
     * @note Load-time/explicit recomposition projection, not a per-frame configuration polling API.
     */
    [[nodiscard]] Result<ConsoleConfigurationDefaults> ReadConsoleConfiguration(const ConfigurationSnapshotRef &snapshot);
}  // namespace Horo::Debug
