#include "Horo/XR/XRProjectSettings.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::XR {
    namespace {
        template <typename T> T Identity(const std::uint64_t value = 1) {
            const auto result = T::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        ConfigurationSnapshot Configuration(ConfigurationResolutionRequest request = {}, const ConfigurationRevision revision = 1) {
            ConfigurationSchema schema;
            for (const auto &descriptor : XRProjectSettingDescriptors())
                REQUIRE(schema.Register(descriptor).HasValue());
            REQUIRE(schema.Seal().HasValue());
            const auto result = ConfigurationResolver::Resolve(schema, request, revision);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        void Project(ConfigurationResolutionRequest &request, const std::string &key, SettingValue value) {
            request.project.emplace(SettingKey{key}, ConfigurationInputValue{std::move(value), SourceLocation{"project.json", 12, 4}});
        }

        struct Evidence final {
            XRLoaderPreflightRequest request{
                .attempt = Identity<XRLoaderPreflightAttempt>(),
                .backend = Identity<XRBackendId>(),
                .installRecord = Identity<XRInstallRecordId>(),
                .productProfile = Identity<XRProductProfileId>(),
                .admittedLoaderVersions = {{1, 0, 0}, {1, 1, 0}},
            };
            XRLoaderPreflightSnapshot loader = Loader();
            XRCapabilitySnapshot capabilities = Capabilities();

            XRLoaderPreflightSnapshot Loader() const {
                const auto result = CreateXRLoaderPreflightSnapshot(request, {
                                                                                 .attempt = request.attempt,
                                                                                 .loader = XRLoaderAvailability::Available,
                                                                                 .runtime = XRRuntimeAvailability::Available,
                                                                                 .system = XRSystemAvailability::Supported,
                                                                                 .loaderApiVersion = {1, 0, 0},
                                                                                 .runtimeGeneration = Identity<XRRuntimeGeneration>(),
                                                                                 .consumedProbeSteps = 3,
                                                                             });
                REQUIRE(result.HasValue());
                return result.Value();
            }

            XRCapabilitySnapshot Capabilities() const {
                XRCapabilityDescriptor descriptor{
                    .system = {.runtime = Identity<XRRuntimeGeneration>(), .slot = {.index = 1, .generation = 1}},
                    .revision = Identity<XRCapabilityRevision>(),
                    .limits = {2, 8, 16, 4},
                };
                descriptor.states.fill(XRCapabilityState::Available);
                const auto result = XRCapabilitySnapshot::Create(descriptor);
                REQUIRE(result.HasValue());
                return result.Value();
            }

            XRProjectSettingsAdmission Admission(const ConfigurationRevision revision = 1) const {
                return {revision,
                        loader,
                        request,
                        XRRendererCompatibility::Compatible,
                        capabilities,
                        capabilities.System(),
                        capabilities.Revision()};
            }
        };

        XRProjectSettings Enabled(ConfigurationResolutionRequest request = {}, const ConfigurationRevision revision = 1) {
            Project(request, "xr.enabled", true);
            const auto result = ResolveXRProjectSettings(Configuration(std::move(request), revision));
            REQUIRE(result.HasValue());
            return result.Value();
        }

        TEST_CASE("XR settings defaults are inert and disabled", "[unit][xr][settings]") {
            const auto descriptors = XRProjectSettingDescriptors();
            REQUIRE(descriptors.size() == 15);
            for (const auto &descriptor : descriptors)
                REQUIRE(descriptor.reloadPolicy == ReloadPolicy::ProjectReopen);
            const auto resolved = ResolveXRProjectSettings(Configuration());
            REQUIRE(resolved.HasValue());
            REQUIRE_FALSE(resolved.Value().Enabled());
            REQUIRE(resolved.Value().Profile() == XRFeatureProfile::Projection1_0);
            REQUIRE(resolved.Value().Features().empty());
            const Evidence evidence;
            REQUIRE(AdmitXRProjectSettings(resolved.Value(), evidence.Admission()).ErrorValue().code ==
                    XRErrors::OperationUnavailable.code);
        }

        TEST_CASE("XR settings preserve Foundation precedence and winning source diagnostics", "[unit][xr][settings]") {
            ConfigurationResolutionRequest request;
            Project(request, "xr.capability_profile", std::int64_t{1});
            request.invocation.emplace(SettingKey{"xr.capability_profile"},
                                       ConfigurationInputValue{std::int64_t{99}, SourceLocation{"cli", 1, 2}});
            const auto result = ResolveXRProjectSettings(Configuration(request));
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().diagnostics.front().location.source == "cli");
            REQUIRE(result.ErrorValue().diagnostics.front().path == "xr.capability_profile");
            request.invocation.clear();
            const auto valid = ResolveXRProjectSettings(Configuration(request));
            REQUIRE(valid.HasValue());
            REQUIRE(valid.Value().Profile() == XRFeatureProfile::TrackedInteraction1_0);
        }

        TEST_CASE("XR projection rejects unsupported versions, invalid enums and capacity boundaries atomically", "[unit][xr][settings]") {
            const std::array invalid{
                std::pair{"xr.schema_version", std::int64_t{2}},
                std::pair{"xr.runtime_selection", std::int64_t{-1}},
                std::pair{"xr.capability_profile", std::int64_t{2}},
                std::pair{"xr.limits.views", std::int64_t{3}},
                std::pair{"xr.limits.spaces", std::int64_t{0}},
                std::pair{"xr.limits.actions", std::int64_t{513}},
                std::pair{"xr.limits.devices", std::numeric_limits<std::int64_t>::max()},
                std::pair{"xr.features.depth_composition", std::int64_t{5}},
            };
            for (const auto &[key, value] : invalid) {
                ConfigurationResolutionRequest request;
                Project(request, key, value);
                const auto result = ResolveXRProjectSettings(Configuration(request));
                REQUIRE(result.HasError());
                REQUIRE(result.ErrorValue().diagnostics.front().path == key);
                REQUIRE(result.ErrorValue().diagnostics.front().location.line == 12);
            }
            ConfigurationSchema empty;
            REQUIRE(empty.Seal().HasValue());
            const auto missing = ConfigurationResolver::Resolve(empty, {}, 1);
            REQUIRE(missing.HasValue());
            REQUIRE(ResolveXRProjectSettings(missing.Value()).HasError());
        }

        TEST_CASE("XR project feature policy rejects profile and fallback conflicts", "[unit][xr][settings]") {
            for (const auto key : {"xr.features.stage_space", "xr.features.controller_presentation", "xr.features.android_standalone"}) {
                ConfigurationResolutionRequest request;
                Project(request, key, std::int64_t{3});
                REQUIRE(ResolveXRProjectSettings(Configuration(request)).HasError());
            }
            ConfigurationResolutionRequest request;
            Project(request, "xr.capability_profile", std::int64_t{1});
            Project(request, "xr.limits.devices", std::int64_t{1});
            REQUIRE(ResolveXRProjectSettings(Configuration(request)).HasError());
        }

        TEST_CASE("XR settings admit exact runtime and independent capability profiles", "[unit][xr][settings]") {
            const Evidence evidence;
            for (const auto profile : {std::int64_t{0}, std::int64_t{1}}) {
                ConfigurationResolutionRequest request;
                Project(request, "xr.capability_profile", profile);
                Project(request, "xr.features.depth_composition", std::int64_t{3});
                const auto settings = Enabled(request);
                const auto result = AdmitXRProjectSettings(settings, evidence.Admission());
                REQUIRE(result.HasValue());
                REQUIRE(result.Value().Profile() == settings.Profile());
                REQUIRE(result.Value().Decision(XRCapability::DepthComposition).state == XRFeatureDecisionState::OptionalEnabled);
            }
        }

        TEST_CASE("XR retained settings reject replacement, owner shutdown and incompatible renderer", "[unit][xr][settings]") {
            const auto settings = Enabled();
            const Evidence evidence;
            for (const auto revision : {ConfigurationRevision{0}, ConfigurationRevision{2}}) {
                const auto result = AdmitXRProjectSettings(settings, evidence.Admission(revision));
                REQUIRE(result.HasError());
                REQUIRE(result.ErrorValue().code == XRErrors::IdentityStale.code);
            }
            auto admission = evidence.Admission();
            admission.activeSystem = {};
            REQUIRE(AdmitXRProjectSettings(settings, admission).HasError());
            auto incompatible = evidence.Admission();
            incompatible.renderer = XRRendererCompatibility::Incompatible;
            REQUIRE(AdmitXRProjectSettings(settings, incompatible).ErrorValue().code == XRErrors::OperationIncompatible.code);
            REQUIRE(settings.Enabled());
            REQUIRE(settings.Revision() == 1);
            REQUIRE(AdmitXRProjectSettings(Enabled({}, 2), evidence.Admission(2)).HasValue());
        }

        TEST_CASE("XR settings cannot grant developer approval or reuse replaced runtime evidence", "[unit][xr][settings]") {
            Evidence evidence;
            ConfigurationResolutionRequest request;
            Project(request, "xr.runtime_selection", std::int64_t{1});
            const auto settings = Enabled(request);
            REQUIRE(AdmitXRProjectSettings(settings, evidence.Admission()).HasError());
            evidence.request.productMode = XRPreflightProductMode::Development;
            evidence.request.runtimeSelection = XRRuntimeSelectionPolicy::ApprovedDeveloperOverride;
            evidence.request.developerOverrideApproved = true;
            evidence.loader = evidence.Loader();
            REQUIRE(AdmitXRProjectSettings(settings, evidence.Admission()).HasValue());
            evidence.request.productMode = XRPreflightProductMode::Shipping;
            REQUIRE(AdmitXRProjectSettings(settings, evidence.Admission()).HasError());
            evidence.request.productMode = XRPreflightProductMode::Development;
            evidence.request.attempt = Identity<XRLoaderPreflightAttempt>(2);
            REQUIRE(AdmitXRProjectSettings(settings, evidence.Admission()).HasError());
        }

        TEST_CASE("XR admission preserves failure provenance for unavailable required feature and finite budgets", "[unit][xr][settings]") {
            const Evidence evidence;
            ConfigurationResolutionRequest request;
            Project(request, "xr.limits.actions", std::int64_t{17});
            const auto over = AdmitXRProjectSettings(Enabled(request), evidence.Admission());
            REQUIRE(over.HasError());
            REQUIRE(over.ErrorValue().code == XRErrors::CapacityExceeded.code);
            XRCapabilityDescriptor descriptor{
                .system = evidence.capabilities.System(),
                .revision = evidence.capabilities.Revision(),
                .limits = evidence.capabilities.Limits(),
            };
            descriptor.states.fill(XRCapabilityState::Available);
            descriptor.states[static_cast<std::size_t>(XRCapability::DepthComposition)] = XRCapabilityState::PermissionRequired;
            const auto limited = XRCapabilitySnapshot::Create(descriptor);
            REQUIRE(limited.HasValue());
            request.project.clear();
            Project(request, "xr.features.depth_composition", std::int64_t{1});
            const XRProjectSettingsAdmission admission{1,
                                                       evidence.loader,
                                                       evidence.request,
                                                       XRRendererCompatibility::Compatible,
                                                       limited.Value(),
                                                       limited.Value().System(),
                                                       limited.Value().Revision()};
            const auto failed = AdmitXRProjectSettings(Enabled(request), admission);
            REQUIRE(failed.HasError());
            REQUIRE(failed.ErrorValue().code == XRErrors::OperationUnavailable.code);
            REQUIRE(failed.ErrorValue().diagnostics.front().path == "xr.features.depth_composition");
            REQUIRE(failed.ErrorValue().diagnostics.front().location.source == "project.json");
        }
    }  // namespace
}  // namespace Horo::XR
