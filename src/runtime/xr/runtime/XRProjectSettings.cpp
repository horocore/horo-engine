#include "Horo/XR/XRProjectSettings.h"

#include <format>
#include <string_view>
#include <utility>

namespace Horo::XR {
    namespace {
        struct FeatureSetting final {
            std::string_view key;
            XRCapability capability;
        };

        constexpr std::array FeatureSettings{
            FeatureSetting{"xr.features.stage_space", XRCapability::StageSpace},
            FeatureSetting{"xr.features.depth_composition", XRCapability::DepthComposition},
            FeatureSetting{"xr.features.fixed_foveation", XRCapability::FixedFoveation},
            FeatureSetting{"xr.features.refresh_rate", XRCapability::RefreshRateSelection},
            FeatureSetting{"xr.features.visibility_mask", XRCapability::VisibilityMask},
            FeatureSetting{"xr.features.controller_presentation", XRCapability::ControllerPresentation},
            FeatureSetting{"xr.features.android_standalone", XRCapability::AndroidStandalone},
        };

        /** @brief Adds winning-source context while preserving the original typed error identity and cause. */
        Error WithSource(Error error, const ConfigurationSnapshot &source, const std::string_view key) {
            SourceLocation location;
            if (const auto *entry = source.FindResolved(SettingKey{std::string(key)}); entry && entry->location)
                location = *entry->location;
            error.diagnostics.emplace_back(DiagnosticCode{error.code.Value()},
                                           DiagnosticSeverityForError(error.severity).value_or(DiagnosticSeverity::Error), error.message,
                                           std::move(location), std::string(key));
            return error;
        }

        /** @brief Forms a declared project-policy error with source provenance. */
        Error Failure(const ConfigurationSnapshot &source, const std::string_view key, const ErrorCodeDescriptor &descriptor,
                      const std::string_view reason) {
            return WithSource(MakeError(descriptor, std::string(reason)), source, key);
        }

        /** @brief Reads one required integer without interpreting missing or wrong-typed values as defaults. */
        Result<std::int64_t> Integer(const ConfigurationSnapshot &source, const std::string_view key, const std::int64_t minimum,
                                     const std::int64_t maximum) {
            const auto *entry = source.FindResolved(SettingKey{std::string(key)});
            const auto *value = entry ? std::get_if<std::int64_t>(&entry->value) : nullptr;
            if (!value || *value < minimum || *value > maximum)
                return Result<std::int64_t>::Failure(Failure(source, key, XRErrors::OperationInvalid,
                                                             "Register the XR schema and supply an integer within the declared range."));
            return Result<std::int64_t>::Success(*value);
        }

        /** @brief Validates one explicit extra-feature policy without publishing a partial settings object. */
        Result<std::optional<XRFeatureRequest>> ResolveFeature(const ConfigurationSnapshot &source, const FeatureSetting &feature,
                                                               const XRFeatureProfile profile) {
            const auto value = Integer(source, feature.key, 0, static_cast<std::int64_t>(XRProjectFeaturePolicy::Count) - 1);
            if (value.HasError())
                return Result<std::optional<XRFeatureRequest>>::Failure(value.ErrorValue());
            const auto policy = static_cast<XRProjectFeaturePolicy>(value.Value());
            if (policy == XRProjectFeaturePolicy::Unrequested)
                return Result<std::optional<XRFeatureRequest>>::Success(std::nullopt);
            if (const bool projectionFallback =
                    feature.capability == XRCapability::DepthComposition || feature.capability == XRCapability::FixedFoveation ||
                    feature.capability == XRCapability::RefreshRateSelection || feature.capability == XRCapability::VisibilityMask;
                (policy == XRProjectFeaturePolicy::OptionalProjection && !projectionFallback) ||
                (feature.capability == XRCapability::ControllerPresentation && profile != XRFeatureProfile::TrackedInteraction1_0))
                return Result<std::optional<XRFeatureRequest>>::Failure(
                    Failure(source, feature.key, XRErrors::OperationIncompatible,
                            "Select an extra-feature policy compatible with the exact product profile."));
            XRFeatureRequest request{feature.capability, XRFeatureRequirementKind::Optional, XRFeatureFallback::Disable};
            if (policy == XRProjectFeaturePolicy::Required || policy == XRProjectFeaturePolicy::Disabled) {
                request.kind =
                    policy == XRProjectFeaturePolicy::Required ? XRFeatureRequirementKind::Required : XRFeatureRequirementKind::Disabled;
                request.fallback = XRFeatureFallback::None;
            } else if (policy == XRProjectFeaturePolicy::OptionalProjection) {
                request.fallback = XRFeatureFallback::BaselineProjection;
            }

            return Result<std::optional<XRFeatureRequest>>::Success(request);
        }

        /** @brief Revalidates retained loader evidence against the complete current host request. */
        Result<void> ValidateLoader(const XRProjectSettings &settings, const XRProjectSettingsAdmission &admission) {
            const auto &request = admission.currentLoaderRequest;
            if (const auto loader = ValidateXRLoaderPreflight(admission.loader, request.attempt, request.backend, request.installRecord,
                                                              request.productProfile);
                loader.HasError()) {
                return Result<void>::Failure(WithSource(loader.ErrorValue(), settings.Source(), "xr.runtime_selection"));
            }
            if (const auto current = CreateXRLoaderPreflightSnapshot(request,
                                                                     {
                                                                         .attempt = admission.loader.Attempt(),
                                                                         .loader = XRLoaderAvailability::Available,
                                                                         .runtime = XRRuntimeAvailability::Available,
                                                                         .system = XRSystemAvailability::Supported,
                                                                         .loaderApiVersion = admission.loader.LoaderApiVersion(),
                                                                         .runtimeGeneration = admission.loader.RuntimeGeneration(),
                                                                         .consumedProbeSteps = admission.loader.ConsumedProbeSteps(),
                                                                     });
                current.HasError()) {
                return Result<void>::Failure(WithSource(current.ErrorValue(), settings.Source(), "xr.runtime_selection"));
            }
            if (admission.loader.RuntimeGeneration() != admission.activeSystem.runtime)
                return Result<void>::Failure(Failure(settings.Source(), "xr.runtime_selection", XRErrors::IdentityStale,
                                                     "Selected runtime evidence belongs to a replaced XR system."));
            return Result<void>::Success();
        }

        /** @brief Maps feature-resolution failure categories without discarding the failing capability state. */
        const ErrorCodeDescriptor &NegotiationError(const XRFeatureNegotiationStatus status) noexcept {
            using enum XRFeatureNegotiationStatus;
            switch (status) {
                case StaleSystem:
                    return XRErrors::IdentityStale;
                case StaleRevision:
                    return XRErrors::CapabilityStale;
                case CapacityExceeded:
                    return XRErrors::CapacityExceeded;
                case RequiredUnsupported:
                case UnsupportedPath:
                    return XRErrors::OperationUnsupported;
                case RequiredIncompatible:
                    return XRErrors::OperationIncompatible;
                case RequiredUnavailable:
                case Unavailable:
                    return XRErrors::OperationUnavailable;
                default:
                    return XRErrors::OperationInvalid;
            }
        }
    }  // namespace

    /** @copydoc XRProjectSettingDescriptors */
    std::vector<SettingDescriptor> XRProjectSettingDescriptors() {
        const ConfigurationSourcePolicy policy{ConfigurationSourceMask::Project | ConfigurationSourceMask::PackagedProfile |
                                               ConfigurationSourceMask::Invocation | ConfigurationSourceMask::Session};
        std::vector<SettingDescriptor> descriptors;
        const auto add = [&](std::string key, const SettingValueType type, SettingValue value) {
            descriptors.emplace_back(SettingKey{std::move(key)}, type, std::move(value), SettingScope::Project, ReloadPolicy::ProjectReopen,
                                     SettingSensitivity::Public, policy);
        };
        add("xr.schema_version", SettingValueType::Integer, XRProjectSettingsSchemaVersion);
        add("xr.enabled", SettingValueType::Boolean, false);
        add("xr.runtime_selection", SettingValueType::Integer, std::int64_t{0});
        add("xr.capability_profile", SettingValueType::Integer, std::int64_t{0});
        add("xr.limits.views", SettingValueType::Integer, std::int64_t{2});
        add("xr.limits.spaces", SettingValueType::Integer, std::int64_t{2});
        add("xr.limits.actions", SettingValueType::Integer, std::int64_t{8});
        add("xr.limits.devices", SettingValueType::Integer, std::int64_t{2});
        for (const auto &feature : FeatureSettings)
            add(std::string(feature.key), SettingValueType::Integer, std::int64_t{0});
        return descriptors;
    }

    /** @copydoc XRProjectSettings::XRProjectSettings */
    XRProjectSettings::XRProjectSettings(ConfigurationSnapshot source) : source_(std::move(source)) {}

    /** @copydoc XRProjectSettings::Revision */
    ConfigurationRevision XRProjectSettings::Revision() const noexcept {
        return source_.Revision();
    }

    /** @copydoc XRProjectSettings::Enabled */
    bool XRProjectSettings::Enabled() const noexcept {
        return enabled_;
    }

    /** @copydoc XRProjectSettings::RuntimeSelection */
    XRRuntimeSelectionPolicy XRProjectSettings::RuntimeSelection() const noexcept {
        return runtimeSelection_;
    }

    /** @copydoc XRProjectSettings::Profile */
    XRFeatureProfile XRProjectSettings::Profile() const noexcept {
        return profile_;
    }

    /** @copydoc XRProjectSettings::Limits */
    const XRSystemLimits &XRProjectSettings::Limits() const noexcept {
        return limits_;
    }

    /** @copydoc XRProjectSettings::Features */
    std::span<const XRFeatureRequest> XRProjectSettings::Features() const noexcept {
        return {features_.data(), featureCount_};
    }

    /** @copydoc XRProjectSettings::Source */
    const ConfigurationSnapshot &XRProjectSettings::Source() const noexcept {
        return source_;
    }

    /** @copydoc ResolveXRProjectSettings */
    Result<XRProjectSettings> ResolveXRProjectSettings(const ConfigurationSnapshot &source) {
        if (const auto version = Integer(source, "xr.schema_version", XRProjectSettingsSchemaVersion, XRProjectSettingsSchemaVersion);
            version.HasError())
            return Result<XRProjectSettings>::Failure(Failure(source, "xr.schema_version", XRErrors::ContractVersionIncompatible,
                                                              "Migrate to XR project-settings schema version 1 before reopening."));
        XRProjectSettings settings{source};
        const auto *enabled = source.FindResolved(SettingKey{"xr.enabled"});
        const auto *flag = enabled ? std::get_if<bool>(&enabled->value) : nullptr;
        if (!flag)
            return Result<XRProjectSettings>::Failure(Failure(source, "xr.enabled", XRErrors::OperationInvalid,
                                                              "Register xr.enabled as a boolean before resolving project policy."));
        settings.enabled_ = *flag;
        const auto runtime = Integer(source, "xr.runtime_selection", 0, static_cast<std::int64_t>(XRRuntimeSelectionPolicy::Count) - 1);
        if (runtime.HasError())
            return Result<XRProjectSettings>::Failure(runtime.ErrorValue());
        settings.runtimeSelection_ = static_cast<XRRuntimeSelectionPolicy>(runtime.Value());
        const auto profile = Integer(source, "xr.capability_profile", 0, static_cast<std::int64_t>(XRFeatureProfile::Count) - 1);
        if (profile.HasError())
            return Result<XRProjectSettings>::Failure(profile.ErrorValue());
        settings.profile_ = static_cast<XRFeatureProfile>(profile.Value());

        struct LimitSetting final {
            std::string_view key;
            std::uint32_t minimum;
            std::uint32_t maximum;
            std::uint32_t *output;
        };

        const std::array limits{
            LimitSetting{"xr.limits.views", 2, 2, &settings.limits_.maximumViews},
            LimitSetting{"xr.limits.spaces", 1, XRHardLimits::MaximumSpaces, &settings.limits_.maximumSpaces},
            LimitSetting{"xr.limits.actions", 1, XRHardLimits::MaximumActions, &settings.limits_.maximumActions},
            LimitSetting{"xr.limits.devices", settings.profile_ == XRFeatureProfile::TrackedInteraction1_0 ? 2U : 1U,
                         XRHardLimits::MaximumDevices, &settings.limits_.maximumDevices},
        };
        for (const auto &limit : limits) {
            const auto value = Integer(source, limit.key, limit.minimum, limit.maximum);
            if (value.HasError())
                return Result<XRProjectSettings>::Failure(value.ErrorValue());
            *limit.output = static_cast<std::uint32_t>(value.Value());
        }
        for (const auto &feature : FeatureSettings) {
            const auto request = ResolveFeature(source, feature, settings.profile_);
            if (request.HasError())
                return Result<XRProjectSettings>::Failure(request.ErrorValue());
            if (request.Value())
                settings.features_[settings.featureCount_++] = *request.Value();
        }
        return Result<XRProjectSettings>::Success(std::move(settings));
    }

    /** @copydoc AdmitXRProjectSettings */
    Result<XRFeaturePlan> AdmitXRProjectSettings(const XRProjectSettings &settings, const XRProjectSettingsAdmission &admission) {
        const auto fail = [&](const std::string_view key, const ErrorCodeDescriptor &descriptor, const std::string_view reason) {
            return Result<XRFeaturePlan>::Failure(Failure(settings.Source(), key, descriptor, reason));
        };
        if (admission.activeConfigurationRevision == 0 || settings.Revision() != admission.activeConfigurationRevision)
            return fail("xr.schema_version", XRErrors::IdentityStale, "Re-resolve XR settings from the active configuration revision.");
        if (!settings.Enabled())
            return fail("xr.enabled", XRErrors::OperationUnavailable, "XR is explicitly disabled by project policy.");
        const auto &request = admission.currentLoaderRequest;
        if (request.cancellationRequested)
            return fail("xr.runtime_selection", XRErrors::LoaderPreflightCancelled,
                        "XR composition was cancelled before settings admission.");
        if (request.productMode >= XRPreflightProductMode::Count || request.runtimeSelection != settings.RuntimeSelection() ||
            request.loaderSource != admission.loader.LoaderSource() || admission.loader.RuntimeSelection() != settings.RuntimeSelection() ||
            (settings.RuntimeSelection() == XRRuntimeSelectionPolicy::ApprovedDeveloperOverride &&
             (request.productMode != XRPreflightProductMode::Development || !request.developerOverrideApproved)) ||
            (settings.RuntimeSelection() == XRRuntimeSelectionPolicy::SystemDefault && request.developerOverrideApproved))
            return fail("xr.runtime_selection", XRErrors::RuntimeOverrideRejected,
                        "Match current runtime policy and obtain host approval for a non-shipping developer override.");
        if (const auto loader = ValidateLoader(settings, admission); loader.HasError())
            return Result<XRFeaturePlan>::Failure(loader.ErrorValue());
        if (admission.renderer != XRRendererCompatibility::Compatible)
            return fail("xr.enabled", XRErrors::OperationIncompatible,
                        "Select a renderer/runtime tuple qualified for external XR targets.");
        const auto negotiated = NegotiateXRFeatures(admission.capabilities, admission.activeSystem, admission.expectedCapabilityRevision,
                                                    {settings.Profile(), settings.Limits(), settings.Features()});
        if (!negotiated.plan) {
            std::string_view key = "xr.capability_profile";
            if (negotiated.status == XRFeatureNegotiationStatus::CapacityExceeded) {
                const auto &available = admission.capabilities.Limits();
                if (settings.Limits().maximumViews > available.maximumViews)
                    key = "xr.limits.views";
                else if (settings.Limits().maximumSpaces > available.maximumSpaces)
                    key = "xr.limits.spaces";
                else if (settings.Limits().maximumActions > available.maximumActions)
                    key = "xr.limits.actions";
                else
                    key = "xr.limits.devices";
            }
            for (const auto &feature : FeatureSettings)
                if (feature.capability == negotiated.capability)
                    key = feature.key;
            return fail(key, NegotiationError(negotiated.status),
                        std::format("The exact XR system cannot admit the selected profile, feature state or budget. Capability={}, "
                                    "observed state={}.",
                                    static_cast<unsigned>(negotiated.capability), static_cast<unsigned>(negotiated.observed)));
        }
        return Result<XRFeaturePlan>::Success(*negotiated.plan);
    }
}  // namespace Horo::XR
