#pragma once

/**
 * @file XRProjectSettings.h
 * @brief Versioned project XR policy projected from the shared immutable configuration authority.
 */

#include "Horo/Foundation/Configuration.h"
#include "Horo/XR/XRFeatureNegotiation.h"
#include "Horo/XR/XRLoaderPreflight.h"
#include "Horo/XR/XRLocomotionComfort.h"

namespace Horo::XR {
    /** @brief Portable XR project-settings schema version, independent of the native API version. */
    inline constexpr std::int64_t XRProjectSettingsSchemaVersion = 1;
    /** @brief Explicit extra-feature policy; unrequested features remain outside the admitted plan. */
    enum class XRProjectFeaturePolicy : std::uint8_t {
        Unrequested,
        Required,
        OptionalDisable,
        OptionalProjection,
        Disabled,
        Count
    };
    /** @brief Exact selected renderer/runtime interoperability evidence supplied by application composition. */
    enum class XRRendererCompatibility : std::uint8_t {
        Compatible,
        Unsupported,
        Incompatible,
        Count
    };

    /**
     * @brief Immutable project policy retaining its source snapshot for diagnostics and revision fencing.
     * @details Owns all policy storage. It remains readable after configuration replacement or shutdown but must be admitted
     * against the current host revision before use. Projection is load-time work; it does not discover or activate native resources.
     */
    class XRProjectSettings final {
    public:
        /** @brief Returns the captured configuration revision. @return Exact shared-authority revision. */
        [[nodiscard]] ConfigurationRevision Revision() const noexcept;
        /** @brief Reports explicitly authored XR enablement. @return True when the product requests XR. */
        [[nodiscard]] bool Enabled() const noexcept;
        /** @brief Returns runtime-selection intent. @return Closed policy; never an approval grant. */
        [[nodiscard]] XRRuntimeSelectionPolicy RuntimeSelection() const noexcept;
        /** @brief Returns the product requirement set. @return One of the two independent version-one profiles. */
        [[nodiscard]] XRFeatureProfile Profile() const noexcept;
        /** @brief Returns requested finite capacities. @return Immutable validated budget. */
        [[nodiscard]] const XRSystemLimits &Limits() const noexcept;
        /** @brief Returns the immutable preferences for the admitted locomotion owner's profile. */
        [[nodiscard]] const XRLocomotionComfortSettings &LocomotionComfort() const noexcept;
        /** @brief Returns the authored inward Stage warning margin in metres. */
        [[nodiscard]] float StageBoundaryWarningMeters() const noexcept;
        /** @brief Returns the minimum projected world-canvas span required in each eye.
         * @return Positive pixel count captured at project reopen.
         */
        [[nodiscard]] std::uint32_t WorldCanvasMinimumSpanPixels() const noexcept;
        /** @brief Reports optional policy for the independent eye-pointing preview.
         * @return True when product policy permits requesting consent; this is never a grant. */
        [[nodiscard]] bool AllowsEyeGazePreview() const noexcept;
        /** @brief Reports policy for the independent plane-capture preview.
         * @return True when the product permits a separate operator request for consent. */
        [[nodiscard]] bool AllowsPlaneCapturePreview() const noexcept;

        /** @brief Reports whether the selected host may offer live spatial anchor preview consent. */
        [[nodiscard]] bool AllowsSpatialAnchorPreview() const noexcept {
            return spatialAnchorPreview_;
        }

        /** @brief Reports whether the selected host may offer separate anchor storage consent. */
        [[nodiscard]] bool AllowsSpatialAnchorPersistence() const noexcept {
            return spatialAnchorPersistence_;
        }

        /** @brief Returns version-one extra requests. @return Snapshot-owned span valid for this object's lifetime. */
        [[nodiscard]] std::span<const XRFeatureRequest> Features() const noexcept;
        /** @brief Returns safe configuration provenance. @return Retained immutable source snapshot. */
        [[nodiscard]] const ConfigurationSnapshot &Source() const noexcept;

    private:
        friend Result<XRProjectSettings> ResolveXRProjectSettings(const ConfigurationSnapshot &);
        /** @brief Retains the immutable shared authority without exposing a mutable policy constructor. */
        explicit XRProjectSettings(ConfigurationSnapshot source);
        ConfigurationSnapshot source_;
        bool enabled_{};
        XRRuntimeSelectionPolicy runtimeSelection_{XRRuntimeSelectionPolicy::SystemDefault};
        XRFeatureProfile profile_{XRFeatureProfile::Projection1_0};
        XRSystemLimits limits_{};
        std::uint32_t worldCanvasMinimumSpanPixels_{};
        float stageBoundaryWarningMeters_{0.30F};
        XRLocomotionComfortSettings locomotionComfort_;
        std::array<XRFeatureRequest, static_cast<std::size_t>(XRCapability::Count)> features_{};
        std::size_t featureCount_{};
        bool planeCapturePreview_{};
        bool spatialAnchorPreview_{};
        bool spatialAnchorPersistence_{};
        bool eyeGazePreview_{};
    };

    /**
     * @brief Supplies inert schema descriptors for explicit composition-root registration.
     * @return Owned descriptors with project/profile/invocation sources and ProjectReopen reload policy.
     * @post Does not register settings or inspect ambient state.
     */
    [[nodiscard]] std::vector<SettingDescriptor> XRProjectSettingDescriptors();
    /**
     * @brief Projects resolved settings and validates all version, policy and budget fields atomically.
     * @param source Immutable snapshot produced by Foundation after host registration of XR descriptors.
     * @return Complete immutable policy or typed error containing the winning source and setting path.
     */
    [[nodiscard]] Result<XRProjectSettings> ResolveXRProjectSettings(const ConfigurationSnapshot &source);

    /** @brief Borrowed exact composition evidence consumed synchronously and never retained. */
    struct XRProjectSettingsAdmission final {
        ConfigurationRevision activeConfigurationRevision{};                    /**< Zero after owner shutdown. */
        const XRLoaderPreflightSnapshot &loader;                                /**< Successful selected-runtime evidence. */
        const XRLoaderPreflightRequest &currentLoaderRequest;                   /**< Current host policy/attempt and override approval. */
        XRRendererCompatibility renderer{XRRendererCompatibility::Unsupported}; /**< Selected tuple's explicit interoperability state. */
        const XRCapabilitySnapshot &capabilities;                               /**< Selected exact-system capability publication. */
        XRSystemId activeSystem;                                                /**< Current system, invalid after shutdown. */
        XRCapabilityRevision expectedCapabilityRevision;                        /**< Current publication revision. */
    };

    /**
     * @brief Admits exact current settings, runtime, renderer and feature evidence before session preparation.
     * @param settings Retained validated project policy.
     * @param admission Current application-owned composition evidence; runtime approval cannot come from project settings.
     * @return Complete feature plan or actionable source diagnostics; no fallback, publication or native resource is created.
     * @note Load-time boundary. Errors may allocate diagnostic storage; frame consumers use AdmitXRPlannedFeature instead.
     */
    [[nodiscard]] Result<XRFeaturePlan> AdmitXRProjectSettings(const XRProjectSettings &settings,
                                                               const XRProjectSettingsAdmission &admission);
}  // namespace Horo::XR
