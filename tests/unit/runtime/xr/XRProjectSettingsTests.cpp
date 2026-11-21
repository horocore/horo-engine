#include "Horo/XR/XRProjectSettings.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <numbers>

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
                descriptor.states[static_cast<std::size_t>(XRCapability::SpaceWarp)] = XRCapabilityState::Unsupported;
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
            REQUIRE(descriptors.size() == 28);
            for (const auto &descriptor : descriptors)
                REQUIRE(descriptor.reloadPolicy == ReloadPolicy::ProjectReopen);
            const auto resolved = ResolveXRProjectSettings(Configuration());
            REQUIRE(resolved.HasValue());
            REQUIRE_FALSE(resolved.Value().Enabled());
            REQUIRE(resolved.Value().Profile() == XRFeatureProfile::Projection1_0);
            REQUIRE(resolved.Value().Features().empty());
            CHECK_FALSE(resolved.Value().AllowsEyeGazePreview());
            CHECK_FALSE(resolved.Value().AllowsPlaneCapturePreview());
            CHECK_FALSE(resolved.Value().AllowsSpatialAnchorPreview());
            CHECK(resolved.Value().WorldCanvasMinimumSpanPixels() == 1);
            CHECK(resolved.Value().StageBoundaryWarningMeters() == 0.30F);
            const Evidence evidence;
            REQUIRE(AdmitXRProjectSettings(resolved.Value(), evidence.Admission()).ErrorValue().code.Value() ==
                    XRErrors::OperationUnavailable.code.Value());
        }

        TEST_CASE("XR canvas readability uses the admitted immutable project pixel floor", "[unit][xr][settings][canvas]") {
            ConfigurationResolutionRequest request;
            Project(request, "xr.world_canvas.minimum_span_pixels", std::int64_t{40});
            const auto project = Enabled(request);
            CHECK(project.WorldCanvasMinimumSpanPixels() == 40);
            request.invocation.try_emplace(SettingKey{"xr.world_canvas.minimum_span_pixels"}, std::int64_t{80},
                                           SourceLocation{"cli", 1, 2});
            const auto overridden = Enabled(request, 2);
            CHECK(overridden.WorldCanvasMinimumSpanPixels() == 80);
            CHECK(project.WorldCanvasMinimumSpanPixels() == 40);
            const Evidence evidence;
            CHECK(AdmitXRProjectSettings(project, evidence.Admission(2)).HasError());
            CHECK(AdmitXRProjectSettings(overridden, evidence.Admission(2)).HasValue());
        }

        TEST_CASE("XR Stage warning margin is immutable and checked", "[unit][xr][settings][comfort]") {
            ConfigurationResolutionRequest request;
            Project(request, "xr.stage_boundary.warning_centimeters", std::int64_t{65});
            const auto first = Enabled(request);
            CHECK(first.StageBoundaryWarningMeters() == 0.65F);
            request.invocation.try_emplace(SettingKey{"xr.stage_boundary.warning_centimeters"}, std::int64_t{-1},
                                           SourceLocation{"cli", 1, 2});
            CHECK(ResolveXRProjectSettings(Configuration(request)).HasError());
            CHECK(first.StageBoundaryWarningMeters() == 0.65F);
        }

        TEST_CASE("XR locomotion comfort is copied from immutable project preference evidence", "[unit][xr][settings][comfort]") {
            ConfigurationResolutionRequest request;
            Project(request, "xr.locomotion.reduced_motion", true);
            Project(request, "xr.locomotion.teleport_when_reduced", true);
            Project(request, "xr.locomotion.maximum_vignette_percent", std::int64_t{40});
            Project(request, "xr.locomotion.artificial_speed_cap_centimeters_per_second", std::int64_t{225});
            Project(request, "xr.locomotion.snap_turn_degrees", std::int64_t{180});
            const auto accepted = Enabled(request);
            const auto &comfort = accepted.LocomotionComfort();
            CHECK(comfort.reducedMotion);
            CHECK(comfort.teleportWhenReduced);
            CHECK(comfort.maximumVignetteOpacity == 0.4F);
            CHECK(comfort.artificialSpeedCapMetersPerSecond == 2.25F);
            CHECK(comfort.snapTurnAngleRadians == std::numbers::pi_v<float>);

            request.invocation.try_emplace(SettingKey{"xr.locomotion.maximum_vignette_percent"}, std::int64_t{101},
                                           SourceLocation{"cli", 7, 1});
            const auto rejected = ResolveXRProjectSettings(Configuration(request));
            REQUIRE(rejected.HasError());
            REQUIRE_FALSE(rejected.ErrorValue().diagnostics.empty());
            CHECK(rejected.ErrorValue().diagnostics.front().location.source == "cli");
            CHECK(comfort.maximumVignetteOpacity == 0.4F);
        }

        TEST_CASE("XR settings capture a bounded advisory Stage warning margin", "[unit][xr][settings][comfort]") {
            CHECK(Enabled().StageBoundaryWarningMeters() == 0.30F);
            for (const auto centimeters : {0, 45}) {
                ConfigurationResolutionRequest request;
                Project(request, "xr.stage_boundary.warning_centimeters", static_cast<std::int64_t>(centimeters));
                CHECK(Enabled(request).StageBoundaryWarningMeters() == static_cast<float>(centimeters) / 100.0F);
            }
            ConfigurationResolutionRequest invalid;
            Project(invalid, "xr.stage_boundary.warning_centimeters", std::int64_t{-1});
            const auto failed = ResolveXRProjectSettings(Configuration(invalid));
            REQUIRE(failed.HasError());
            REQUIRE_FALSE(failed.ErrorValue().diagnostics.empty());
            CHECK(failed.ErrorValue().diagnostics.front().path == "xr.stage_boundary.warning_centimeters");
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
                std::pair{"xr.world_canvas.minimum_span_pixels", std::int64_t{0}},
                std::pair{"xr.world_canvas.minimum_span_pixels", std::int64_t{65'536}},
                std::pair{"xr.features.depth_composition", std::int64_t{5}},
                std::pair{"xr.features.hand_tracking", std::int64_t{5}},
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

        TEST_CASE("XR spatial plane preview remains outside required product settings", "[unit][xr][settings]") {
            ConfigurationResolutionRequest request;
            const auto baseline = ResolveXRProjectSettings(Configuration(request));
            REQUIRE(baseline.HasValue());
            CHECK_FALSE(baseline.Value().AllowsPlaneCapturePreview());
            Project(request, "xr.preview.spatial_planes", true);
            const auto preview = ResolveXRProjectSettings(Configuration(request));
            REQUIRE(preview.HasValue());
            CHECK(preview.Value().AllowsPlaneCapturePreview());
            CHECK_FALSE(preview.Value().AllowsSpatialAnchorPreview());
            CHECK(preview.Value().Profile() == baseline.Value().Profile());
            CHECK(preview.Value().Features().size() == baseline.Value().Features().size());
        }

        TEST_CASE("XR spatial anchors are an independent optional preview", "[unit][xr][settings]") {
            ConfigurationResolutionRequest request;
            Project(request, "xr.preview.spatial_anchors", true);
            const auto settings = ResolveXRProjectSettings(Configuration(request));
            REQUIRE(settings.HasValue());
            CHECK(settings.Value().AllowsSpatialAnchorPreview());
            CHECK_FALSE(settings.Value().AllowsSpatialAnchorPersistence());
            CHECK_FALSE(settings.Value().AllowsPlaneCapturePreview());
            CHECK(settings.Value().Features().empty());
        }

        TEST_CASE("XR anchor storage has separate opt-in project policy", "[unit][xr][settings]") {
            ConfigurationResolutionRequest request;
            const auto defaults = ResolveXRProjectSettings(Configuration(request));
            REQUIRE(defaults.HasValue());
            CHECK_FALSE(defaults.Value().AllowsSpatialAnchorPersistence());

            Project(request, "xr.persistence.spatial_anchors", true);
            const auto settings = ResolveXRProjectSettings(Configuration(request));
            REQUIRE(settings.HasValue());
            CHECK(settings.Value().AllowsSpatialAnchorPersistence());
            CHECK_FALSE(settings.Value().AllowsSpatialAnchorPreview());
            CHECK(settings.Value().Features().empty());
        }

        TEST_CASE("XR project policy can offer optional eye gaze without granting consent", "[unit][xr][settings]") {
            ConfigurationResolutionRequest request;
            Project(request, "xr.features.eye_gaze", static_cast<std::int64_t>(XRProjectFeaturePolicy::OptionalDisable));
            const auto resolved = ResolveXRProjectSettings(Configuration(request));
            REQUIRE(resolved.HasValue());
            CHECK(resolved.Value().AllowsEyeGazePreview());
            CHECK(resolved.Value().Features().empty());
            request.project.clear();
            Project(request, "xr.features.eye_gaze", static_cast<std::int64_t>(XRProjectFeaturePolicy::Required));
            const auto required = ResolveXRProjectSettings(Configuration(request));
            REQUIRE(required.HasError());
            CHECK(required.ErrorValue().code.Value() == XRErrors::OperationIncompatible.code.Value());
        }

        TEST_CASE("XR project feature policy rejects profile and fallback conflicts", "[unit][xr][settings]") {
            for (const auto key : {"xr.features.stage_space", "xr.features.hand_tracking", "xr.features.controller_presentation",
                                   "xr.features.android_standalone"}) {
                ConfigurationResolutionRequest request;
                Project(request, key, std::int64_t{3});
                REQUIRE(ResolveXRProjectSettings(Configuration(request)).HasError());
            }
            ConfigurationResolutionRequest request;
            Project(request, "xr.capability_profile", std::int64_t{1});
            Project(request, "xr.limits.devices", std::int64_t{1});
            REQUIRE(ResolveXRProjectSettings(Configuration(request)).HasError());
        }

        TEST_CASE("XR project space-warp requests resolve to ordinary projection without provider evidence", "[unit][xr][settings]") {
            const Evidence evidence;
            ConfigurationResolutionRequest request;
            Project(request, "xr.features.space_warp", static_cast<std::int64_t>(XRProjectFeaturePolicy::OptionalProjection));
            const auto settings = Enabled(request);
            REQUIRE(settings.Features().size() == 1);
            CHECK(settings.Features().front().capability == XRCapability::SpaceWarp);
            const auto plan = AdmitXRProjectSettings(settings, evidence.Admission());
            REQUIRE(plan.HasValue());
            CHECK(plan.Value().Decision(XRCapability::SpaceWarp).state == XRFeatureDecisionState::BaselineProjection);
            CHECK(plan.Value().Decision(XRCapability::Projection).state == XRFeatureDecisionState::Required);
            ConfigurationResolutionRequest required;
            Project(required, "xr.features.space_warp", static_cast<std::int64_t>(XRProjectFeaturePolicy::Required));
            CHECK(ResolveXRProjectSettings(Configuration(required)).HasError());
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
                REQUIRE(result.ErrorValue().code.Value() == XRErrors::IdentityStale.code.Value());
            }
            auto admission = evidence.Admission();
            admission.activeSystem = {};
            REQUIRE(AdmitXRProjectSettings(settings, admission).HasError());
            auto incompatible = evidence.Admission();
            incompatible.renderer = XRRendererCompatibility::Incompatible;
            REQUIRE(AdmitXRProjectSettings(settings, incompatible).ErrorValue().code.Value() ==
                    XRErrors::OperationIncompatible.code.Value());
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

        TEST_CASE("XR settings require a fresh preflight when the product mode changes", "[unit][xr][settings]") {
            const auto settings = Enabled();
            Evidence evidence;
            REQUIRE(AdmitXRProjectSettings(settings, evidence.Admission()).HasValue());
            for (const auto productMode : {XRPreflightProductMode::Development, XRPreflightProductMode::Shipping}) {
                evidence.request.productMode = productMode;
                const auto changed = AdmitXRProjectSettings(settings, evidence.Admission());
                REQUIRE(changed.HasError());
                CHECK(changed.ErrorValue().code.Value() == XRErrors::LoaderPreflightStale.code.Value());
                REQUIRE_FALSE(changed.ErrorValue().diagnostics.empty());
                CHECK(changed.ErrorValue().diagnostics.front().path == "xr.runtime_selection");
                evidence.loader = evidence.Loader();
                REQUIRE(AdmitXRProjectSettings(settings, evidence.Admission()).HasValue());
            }
        }

        TEST_CASE("XR admission rechecks changed loader policy and capability revisions", "[unit][xr][settings]") {
            const auto settings = Enabled();
            Evidence evidence;
            evidence.request.admittedLoaderVersions = {{1, 1, 0}, {1, 2, 0}};
            const auto incompatible = AdmitXRProjectSettings(settings, evidence.Admission());
            REQUIRE(incompatible.HasError());
            REQUIRE(incompatible.ErrorValue().code.Value() == XRErrors::LoaderIncompatible.code.Value());
            REQUIRE(incompatible.ErrorValue().diagnostics.front().code.Value() == XRErrors::LoaderIncompatible.code.Value());
            evidence.request.admittedLoaderVersions = {{1, 0, 0}, {1, 1, 0}};
            evidence.request.loaderSource = XRLoaderSourcePolicy::PlatformProvided;
            REQUIRE(AdmitXRProjectSettings(settings, evidence.Admission()).HasError());
            evidence.request.loaderSource = XRLoaderSourcePolicy::BundledVerified;
            auto stale = evidence.Admission();
            stale.expectedCapabilityRevision = Identity<XRCapabilityRevision>(2);
            REQUIRE(AdmitXRProjectSettings(settings, stale).ErrorValue().code.Value() == XRErrors::CapabilityStale.code.Value());
            evidence.request.cancellationRequested = true;
            REQUIRE(AdmitXRProjectSettings(settings, evidence.Admission()).ErrorValue().code.Value() ==
                    XRErrors::LoaderPreflightCancelled.code.Value());
        }

        TEST_CASE("XR admission preserves failure provenance for unavailable required feature and finite budgets", "[unit][xr][settings]") {
            const Evidence evidence;
            ConfigurationResolutionRequest request;
            Project(request, "xr.limits.actions", std::int64_t{17});
            const auto over = AdmitXRProjectSettings(Enabled(request), evidence.Admission());
            REQUIRE(over.HasError());
            REQUIRE(over.ErrorValue().code.Value() == XRErrors::CapacityExceeded.code.Value());
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
            REQUIRE(failed.ErrorValue().code.Value() == XRErrors::OperationUnavailable.code.Value());
            REQUIRE(failed.ErrorValue().diagnostics.front().path == "xr.features.depth_composition");
            REQUIRE(failed.ErrorValue().diagnostics.front().location.source == "project.json");
        }
    }  // namespace
}  // namespace Horo::XR
