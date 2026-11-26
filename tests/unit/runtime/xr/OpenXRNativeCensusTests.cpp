#include "Horo/XR/XRLightProjection.h"
#include "OpenXRPlaneCapture.h"
#include "OpenXRPlaneSnapshot.h"
#include "OpenXRSessionEvents.h"
#include "OpenXRViewRecommendations.h"
#include "support/OpenXREventScriptTestSupport.h"
#include "support/OpenXRLightTestSupport.h"
#include "support/OpenXRPlaneCaptureTestSupport.h"

#include <bit>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <limits>

namespace Horo::Tests::OpenXR {
    namespace {

        /** @brief Emulates ordered stereo recommendations and rejects an uninitialized output contract. */
        XrResult XRAPI_CALL StereoRecommendations(XrInstance, XrSystemId system, XrViewConfigurationType configuration,
                                                  std::uint32_t capacity, std::uint32_t *count, XrViewConfigurationView *views) {
            if (configuration != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
                return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
            if (capacity == 0) {
                *count = system == 2 ? 4 : 2;
                return XR_SUCCESS;
            }
            if (capacity != 2 || views[0].type != XR_TYPE_VIEW_CONFIGURATION_VIEW || views[1].type != XR_TYPE_VIEW_CONFIGURATION_VIEW)
                return XR_ERROR_VALIDATION_FAILURE;
            *count = system == 3 ? 1 : 2;
            views[0] = {XR_TYPE_VIEW_CONFIGURATION_VIEW, nullptr, 1024, 2048, 768, 1536, 1, 4};
            views[1] = {XR_TYPE_VIEW_CONFIGURATION_VIEW, nullptr, 1280, 2048, system == 4 ? 0U : 800U, 1536, 1, 4};
            return system == 5 ? XR_ERROR_INSTANCE_LOST : XR_SUCCESS;
        }

        struct CensusFixture {
            Script native;
            CensusScript census;
            Graphics graphics;
            Fence fence;
            XRLoaderPreflightSnapshot preflight{Preflight()};
            OpenXRNativeSession owner{std::make_shared<CensusLibrary>(), graphics, fence};

            NativeDiscoveryRequest Discovery() const {
                return {.selection = {.attempt = preflight.Attempt(),
                                      .backend = preflight.Backend(),
                                      .installRecord = preflight.InstallRecord(),
                                      .productProfile = preflight.ProductProfile(),
                                      .admittedLoaderVersions = {{1, 0, 0}, {1, 1, 63}}},
                        .requestedApi = preflight.LoaderApiVersion()};
            }

            NativeSessionPreparation Preparation() const {
                return {preflight, preflight.Attempt()};
            }
        };

        /** @brief Keeps the scripted dispatch alive through the real owner's cleanup. */
        struct LightFixture final {
            LightScript script;
            CensusFixture native;
            const XRCapabilitySnapshot capabilities{Capabilities()};
            const XRFeaturePlan plan{Plan(capabilities)};
            XRSessionId session{capabilities.System(), {1, 1}};
            XRLightEstimateScope scope{session,
                                       {{session, {2, 1}}, XRSpaceKind::World, IdentityValue<XRWorldOriginRevision>(1)},
                                       IdentityValue<XRLightEstimateGrant>(1)};
            const XRLightEstimateLimits limits{Duration::FromNanoseconds(10), Duration::FromNanoseconds(30)};
            bool lightPermitted{true};

            void Create(bool enabled = true) {
                static constexpr std::array<std::string_view, 1> extensions{XR_ANDROID_LIGHT_ESTIMATION_EXTENSION_NAME};
                const std::array routes{NativeExtensionRequest{extensions.front()}};
                native.native.extraExtensions = extensions;
                native.native.extensionCount += static_cast<std::uint32_t>(extensions.size());
                native.census.extraProc = &LightScript::GetProc;
                const auto attempt = native.preflight.Attempt();
                REQUIRE(native.owner
                            .Create({.candidate = session,
                                     .preflight = native.preflight,
                                     .capabilities = capabilities,
                                     .plan = plan,
                                     .activeAttempt = attempt,
                                     .extensions = enabled ? std::span<const NativeExtensionRequest>{routes}
                                                           : std::span<const NativeExtensionRequest>{}})
                            .HasValue());
            }

            static XRRuntimeTime Time(const XrTime tick) {
                return XRRuntimeTime::Create(tick).Value();
            }

            NativeLightAuthorization Authorization() noexcept {
                return {this, [](void *context, const XRLightEstimateScope &) noexcept {
                    return static_cast<const LightFixture *>(context)->lightPermitted;
                }};
            }

            Result<void> Start(const XRLightEstimateScope &requested) {
                return native.owner.StartLightEstimation(requested, limits, Authorization());
            }

            Result<bool> Advance(const XrTime tick) {
                return native.owner.AdvanceLightEstimation(scope, NativeHandle<XrSpace>(15), Time(tick), Authorization());
            }

            Result<XRLightEstimate> Read(const XrTime tick) {
                return Read(scope, tick);
            }

            Result<XRLightEstimate> Read(const XRLightEstimateScope &requested, const XrTime tick) {
                return native.owner.ReadLightEstimation(requested, Time(tick), Authorization());
            }
        };

        /** @brief Provides event admission for a native session already created by the light fixture. */
        struct ObservationEventResources final : IXRSessionResources {
            Result<void> Prepare(XRSessionPreparation, const XRSessionId &, const XRFeaturePlan &) override {
                return Result<void>::Success();
            }

            void Release(XRSessionPreparation, const XRSessionId &) noexcept override {
                // The fixture owns native teardown independently of logical event admission.
            }
        };

        struct LightEvents final {
            ObservationEventResources resources;
            XRSessionLifecycle sessions{resources};
            XRFrameLifecycle frames{sessions};
            EventScript script;
            OpenXRSessionEvents events;

            explicit LightEvents(LightFixture &fixture) : events{fixture.native.owner, sessions, frames} {
                const auto activated =
                    sessions.Activate(fixture.capabilities, fixture.capabilities.System(), fixture.capabilities.Revision(), fixture.plan);
                REQUIRE(activated.HasValue());
                fixture.session = activated.Value();
                fixture.scope.session = fixture.session;
                fixture.scope.space.id.session = fixture.session;
                fixture.Create();
                script.session = fixture.native.native.lastSession;
                REQUIRE(events.Bind({&EventScript::GetProc}, {}).HasValue());
                EventScript::active = &script;
                script.Push(XR_SESSION_STATE_READY);
                script.Push(XR_SESSION_STATE_VISIBLE);
                script.Push(XR_SESSION_STATE_FOCUSED);
                REQUIRE(events.Poll(true).HasValue());
                REQUIRE(sessions.Snapshot().admitsActions);
            }

            ~LightEvents() {
                EventScript::active = nullptr;
            }

            void InterruptFocus() {
                script.Push(XR_SESSION_STATE_VISIBLE);
                script.Push(XR_SESSION_STATE_FOCUSED);
                const auto observed = events.Poll(true);
                REQUIRE(observed.HasValue());
                CHECK(observed.Value().actionsInterrupted);
                CHECK(sessions.Snapshot().admitsActions);
            }
        };

    }  // namespace

    TEST_CASE("Cold OpenXR stereo recommendations require both ordered valid views", "[unit][xr][native][stereo-recommendations]") {
        const auto instance = reinterpret_cast<XrInstance>(std::uintptr_t{1});
        const auto extents = XR::OpenXRInternal::ReadPrimaryStereoExtents(StereoRecommendations, instance, 1);
        REQUIRE(extents);
        CHECK((*extents)[0].width == 1024);
        CHECK((*extents)[0].height == 768);
        CHECK((*extents)[1].width == 1280);
        CHECK((*extents)[1].height == 800);
        const auto invalid = GENERATE(2, 3, 4, 5);
        CHECK_FALSE(XR::OpenXRInternal::ReadPrimaryStereoExtents(StereoRecommendations, instance, invalid));
    }

    TEST_CASE("OpenXR light estimator requires an enabled route, scope and system support", "[unit][xr][native][lighting]") {
        LightFixture fixture;
        auto expected = &XRErrors::OperationUnsupported;
        bool enabled = true;
        bool expectsCreate = false;
        SECTION("advertised light route was not enabled") {
            enabled = false;
            fixture.native.census.lightEstimationSupport = XR_TRUE;
        }
        SECTION("enabled extension provides no system light support") {}
        SECTION("an invalid system support flag fails closed") {
            fixture.native.census.lightEstimationSupport = 9;
            expected = &XRErrors::OperationInvalid;
        }
        SECTION("native Android policy denied creation") {
            fixture.native.census.lightEstimationSupport = XR_TRUE;
            fixture.script.createResult = XR_ERROR_PERMISSION_INSUFFICIENT;
            expected = &XRErrors::PermissionDenied;
            expectsCreate = true;
        }
        fixture.Create(enabled);
        RequireFailureIdentity(fixture.Start(fixture.scope), *expected);
        CHECK(fixture.script.createCalls == (expectsCreate ? 1U : 0U));
        CHECK(fixture.script.sampleCalls == 0);
        CHECK_FALSE(fixture.script.live);
        REQUIRE(fixture.native.owner.Close().HasValue());
    }

    TEST_CASE("A failed native light startup cannot conceal permission withdrawal", "[unit][xr][native][lighting]") {
        LightFixture fixture;
        fixture.native.census.lightEstimationSupport = XR_TRUE;
        fixture.Create();
        const bool query = GENERATE(false, true);
        if (query) {
            fixture.native.census.fail = "light-estimation-properties";
            fixture.native.census.onLightProperties = [&] { fixture.lightPermitted = false; };
        } else {
            fixture.script.createResult = XR_ERROR_RUNTIME_FAILURE;
            fixture.script.onCreate = [&] { fixture.lightPermitted = false; };
        }
        const auto denied = fixture.Start(fixture.scope);
        RequireFailureIdentity(denied, XRErrors::PermissionDenied);
        CHECK(ErrorChainContains(denied.ErrorValue(), XRErrors::OperationUnavailable.domain, XRErrors::OperationUnavailable.code));
        CHECK(fixture.script.createCalls == (query ? 0U : 1U));
        CHECK_FALSE(fixture.script.live);

        fixture.native.census.fail = {};
        fixture.native.census.onLightProperties = {};
        fixture.script.createResult = XR_SUCCESS;
        fixture.script.onCreate = {};
        fixture.lightPermitted = true;
        RequireFailureIdentity(fixture.Start(fixture.scope), XRErrors::IdentityStale);
        fixture.scope.grant = IdentityValue<XRLightEstimateGrant>(2);
        REQUIRE(fixture.Start(fixture.scope).HasValue());
        REQUIRE(fixture.native.owner.StopLightEstimation(fixture.scope).HasValue());
    }

    TEST_CASE("Live light consent is checked before native creation and sample publication", "[unit][xr][native][lighting]") {
        LightFixture fixture;
        fixture.native.census.lightEstimationSupport = XR_TRUE;
        fixture.Create();
        auto &owner = fixture.native.owner;
        SECTION("denial during capability discovery never creates a sensitive native estimator") {
            fixture.native.census.onLightProperties = [&fixture] { fixture.lightPermitted = false; };
            RequireFailureIdentity(fixture.Start(fixture.scope), XRErrors::PermissionDenied);
            CHECK(fixture.script.createCalls == 0);
        }
        SECTION("denial inside native creation rolls back the unpublished estimator") {
            fixture.script.onCreate = [&fixture] { fixture.lightPermitted = false; };
            RequireFailureIdentity(fixture.Start(fixture.scope), XRErrors::PermissionDenied);
            CHECK(fixture.script.createCalls == 1);
            CHECK(fixture.script.retireCalls == 1);
        }
        SECTION("denial before a query redacts the previous sample without querying the provider") {
            REQUIRE(fixture.Start(fixture.scope).HasValue());
            REQUIRE(fixture.Advance(100).Value());
            fixture.lightPermitted = false;
            RequireFailureIdentity(fixture.Advance(110), XRErrors::PermissionDenied);
            CHECK(fixture.script.sampleCalls == 1);
            RequireFailureIdentity(fixture.Read(110), XRErrors::OperationUnavailable);
        }
        SECTION("denial inside a query prevents either the candidate or cached sample from being read") {
            REQUIRE(fixture.Start(fixture.scope).HasValue());
            REQUIRE(fixture.Advance(100).Value());
            fixture.script.onSample = [&fixture] { fixture.lightPermitted = false; };
            fixture.script.sampleTime = 110;
            RequireFailureIdentity(fixture.Advance(110), XRErrors::PermissionDenied);
            CHECK(fixture.script.sampleCalls == 2);
            RequireFailureIdentity(fixture.Read(110), XRErrors::OperationUnavailable);
        }
        SECTION("a failed native query still observes withdrawal before permission can be restored") {
            REQUIRE(fixture.Start(fixture.scope).HasValue());
            REQUIRE(fixture.Advance(100).Value());
            fixture.script.onSample = [&fixture] { fixture.lightPermitted = false; };
            fixture.script.sampleResult = XR_ERROR_RUNTIME_FAILURE;
            RequireFailureIdentity(fixture.Advance(110), XRErrors::PermissionDenied);
            CHECK(owner.NeedsLightRetirement(fixture.scope));

            fixture.lightPermitted = true;
            fixture.script.onSample = {};
            fixture.script.sampleResult = XR_SUCCESS;
            RequireFailureIdentity(fixture.Read(120), XRErrors::OperationUnavailable);
            RequireFailureIdentity(fixture.Advance(120), XRErrors::OperationUnavailable);
            CHECK(fixture.script.sampleCalls == 2);
            REQUIRE(owner.StopLightEstimation(fixture.scope).HasValue());
            RequireFailureIdentity(fixture.Start(fixture.scope), XRErrors::IdentityStale);
        }
        REQUIRE(owner.Close().HasValue());
        CHECK_FALSE(fixture.script.live);
    }

    TEST_CASE("A light read withdraws the exact native grant with or without a cached sample", "[unit][xr][native][lighting]") {
        LightFixture fixture;
        fixture.native.census.lightEstimationSupport = XR_TRUE;
        fixture.Create();
        REQUIRE(fixture.Start(fixture.scope).HasValue());
        SECTION("Before the first estimate") {
            RequireFailureIdentity(fixture.Read(100), XRErrors::OperationUnavailable);
            CHECK(fixture.script.sampleCalls == 0);
        }
        SECTION("With a cached estimate") {
            REQUIRE(fixture.Advance(100).Value());
        }
        const auto sampled = fixture.script.sampleCalls;
        fixture.lightPermitted = false;
        RequireFailureIdentity(fixture.Read(101), XRErrors::PermissionDenied);
        CHECK(fixture.native.owner.NeedsLightRetirement(fixture.scope));
        fixture.lightPermitted = true;
        RequireFailureIdentity(fixture.Read(102), XRErrors::OperationUnavailable);
        RequireFailureIdentity(fixture.Advance(103), XRErrors::OperationUnavailable);
        CHECK(fixture.script.sampleCalls == sampled);
        REQUIRE(fixture.native.owner.Close().HasValue());
    }

    TEST_CASE("An out-of-order light request still withdraws a revoked grant", "[unit][xr][native][lighting]") {
        LightFixture fixture;
        fixture.native.census.lightEstimationSupport = XR_TRUE;
        fixture.Create();
        REQUIRE(fixture.Start(fixture.scope).HasValue());
        REQUIRE(fixture.Advance(100).Value());
        fixture.lightPermitted = false;

        SECTION("Cached read") {
            RequireFailureIdentity(fixture.Read(99), XRErrors::PermissionDenied);
        }
        SECTION("Native sample") {
            RequireFailureIdentity(fixture.Advance(99), XRErrors::PermissionDenied);
        }

        CHECK(fixture.native.owner.NeedsLightRetirement(fixture.scope));
        fixture.lightPermitted = true;
        RequireFailureIdentity(fixture.Read(100), XRErrors::OperationUnavailable);
        RequireFailureIdentity(fixture.Advance(110), XRErrors::OperationUnavailable);
        CHECK(fixture.script.sampleCalls == 1);
        REQUIRE(fixture.native.owner.Close().HasValue());
    }

    TEST_CASE("A withdrawn light estimator retains its exact scope until native cleanup succeeds", "[unit][xr][native][lighting]") {
        LightFixture fixture;
        fixture.native.census.lightEstimationSupport = XR_TRUE;
        fixture.Create();
        auto &owner = fixture.native.owner;
        fixture.script.onCreate = [&fixture] { fixture.lightPermitted = false; };
        fixture.script.failRetirement = true;
        const auto started = fixture.Start(fixture.scope);
        RequireFailureIdentity(started, XRErrors::OperationUnavailable);
        REQUIRE(started.ErrorValue().cause);
        CHECK(started.ErrorValue().cause.Get()->code.Value() == XRErrors::PermissionDenied.code.Value());
        CHECK(owner.NeedsLightRetirement(fixture.scope));
        CHECK(fixture.script.live);
        CHECK(fixture.script.retireCalls == 1);
        RequireFailureIdentity(fixture.Read(100), XRErrors::OperationUnavailable);
        auto replacement = fixture.scope;
        replacement.grant = IdentityValue<XRLightEstimateGrant>(2);
        CHECK_FALSE(owner.NeedsLightRetirement(replacement));
        RequireFailureIdentity(owner.StopLightEstimation(replacement), XRErrors::IdentityStale);
        RequireFailureIdentity(fixture.Start(replacement), XRErrors::CapacityExceeded);

        fixture.script.failRetirement = false;
        REQUIRE(owner.StopLightEstimation(fixture.scope).HasValue());
        CHECK_FALSE(owner.NeedsLightRetirement(fixture.scope));
        CHECK_FALSE(fixture.script.live);
        fixture.lightPermitted = true;
        fixture.script.onCreate = {};
        RequireFailureIdentity(fixture.Start(fixture.scope), XRErrors::IdentityStale);
        REQUIRE(fixture.Start(replacement).HasValue());
        CHECK_FALSE(owner.NeedsLightRetirement(replacement));
        RequireFailureIdentity(fixture.Read(replacement, 100), XRErrors::OperationUnavailable);
        REQUIRE(owner.Close().HasValue());
    }

    TEST_CASE("OpenXR samples bounded light lobes for an exact World grant and retires them before the session",
              "[unit][xr][native][lighting]") {
        LightFixture fixture;
        fixture.native.census.lightEstimationSupport = XR_TRUE;
        fixture.Create();
        auto &owner = fixture.native.owner;
        REQUIRE(fixture.Start(fixture.scope).HasValue());
        auto second = fixture.scope;
        second.grant = IdentityValue<XRLightEstimateGrant>(2);
        RequireFailureIdentity(fixture.Start(second), XRErrors::CapacityExceeded);
        CHECK(fixture.script.createCalls == 1);
        const auto first = fixture.Advance(100);
        REQUIRE(first.HasValue());
        CHECK(first.Value());
        const auto reading = fixture.Read(100);
        REQUIRE(reading.HasValue());
        CHECK(reading.Value().scope == fixture.scope);
        CHECK(reading.Value().sampledAt == LightFixture::Time(100));
        CHECK(reading.Value().provenance == XRLightEstimateProvenance::AndroidOpenXR);
        REQUIRE(reading.Value().ambient);
        CHECK(reading.Value().ambient->relativeRgb.y == 3.0F);
        CHECK(reading.Value().ambient->confidence == XRLightEstimateConfidence::ProviderValid);
        REQUIRE(reading.Value().directional);
        CHECK(reading.Value().directional->providerDirection.y == -1.0F);
        CHECK(reading.Value().directional->intensity.relativeRgb.x == 4.0F);

        SECTION("explicit host policy projects the native observation and withdraws it on source retirement") {
            const Render::SceneColorSourcePolicy color{Render::SceneColorEncoding::LinearAcesCg};
            auto created = XRLightProjection::Create(fixture.scope, {.ambientColor = color,
                                                                     .directionalColor = color,
                                                                     .direction = XRLightDirectionConvention::TowardScene,
                                                                     .ambientScale = 0.5F,
                                                                     .directionalScale = 0.25F,
                                                                     .maximumAbsoluteChannel = 12.0F,
                                                                     .directionalIdentity = 67,
                                                                     .maximumAge = fixture.limits.maximumAge});
            REQUIRE(created.HasValue());
            auto projection = std::move(created).Value();
            const auto projected = projection.Update(fixture.scope, LightFixture::Time(100), &reading.Value());
            REQUIRE(projected.HasValue());
            REQUIRE(projected.Value());
            REQUIRE(projected.Value()->ambient);
            CHECK(projected.Value()->ambient->y == 1.5F);
            REQUIRE(projected.Value()->directional);
            CHECK(projected.Value()->directional->color.x == 1.0F);
            CHECK(projected.Value()->directional->direction.y == -1.0F);
            REQUIRE(owner.StopLightEstimation(fixture.scope).HasValue());
            CHECK_FALSE(projection.Update(fixture.scope, LightFixture::Time(101), nullptr).Value());
            CHECK_FALSE(projection.Update(fixture.scope, LightFixture::Time(102), &reading.Value()).Value());
        }
        SECTION("cadence and a repeated runtime sample do not renew expiry") {
            const auto bounded = fixture.Advance(109);
            REQUIRE(bounded.HasValue());
            CHECK_FALSE(bounded.Value());
            CHECK(fixture.script.sampleCalls == 1);
            const auto repeated = fixture.Advance(110);
            REQUIRE(repeated.HasValue());
            CHECK_FALSE(repeated.Value());
            CHECK(fixture.script.sampleCalls == 2);
            CHECK(fixture.Read(130).HasValue());
            RequireFailureIdentity(fixture.Read(131), XRErrors::TrackingSnapshotStale);
            RequireFailureIdentity(fixture.Read(130), XRErrors::TimeDomainIncompatible);
            RequireFailureIdentity(fixture.Advance(130), XRErrors::TimeDomainIncompatible);
            CHECK(fixture.script.sampleCalls == 2);
            RequireFailureIdentity(fixture.Advance(131), XRErrors::TrackingSnapshotStale);
            RequireFailureIdentity(fixture.Read(131), XRErrors::OperationUnavailable);
        }
        SECTION("independent native validity ignores undefined channels in a missing lobe") {
            fixture.script.ambientValid = false;
            fixture.script.sampleTime = 110;
            REQUIRE(fixture.Advance(110).Value());
            const auto partial = fixture.Read(110);
            REQUIRE(partial.HasValue());
            CHECK_FALSE(partial.Value().ambient);
            REQUIRE(partial.Value().directional);
            fixture.script.malformedDirection = true;
            fixture.script.sampleTime = 120;
            RequireFailureIdentity(fixture.Advance(120), XRErrors::OperationInvalid);
            RequireFailureIdentity(fixture.Read(120), XRErrors::OperationUnavailable);
        }
        SECTION("revoked lobe is cleared even if the provider repeats a timestamp") {
            fixture.script.ambientValid = false;
            const auto duplicate = fixture.Advance(110);
            REQUIRE(duplicate.HasValue());
            CHECK_FALSE(duplicate.Value());
            const auto partial = fixture.Read(110);
            REQUIRE(partial.HasValue());
            CHECK_FALSE(partial.Value().ambient);
            REQUIRE(partial.Value().directional);
        }
        SECTION("different native space cannot reuse the same scoped grant") {
            RequireFailureIdentity(owner.AdvanceLightEstimation(fixture.scope, NativeHandle<XrSpace>(16), LightFixture::Time(110),
                                                                 fixture.Authorization()),
                                   XRErrors::CoordinateSpaceIncompatible);
            CHECK(fixture.script.sampleCalls == 1);
            RequireFailureIdentity(fixture.Read(110), XRErrors::OperationUnavailable);
        }
        SECTION("sensor permission loss invalidates and prevents another native query") {
            fixture.script.sampleResult = XR_ERROR_PERMISSION_INSUFFICIENT;
            RequireFailureIdentity(fixture.Advance(110), XRErrors::PermissionDenied);
            RequireFailureIdentity(fixture.Read(110), XRErrors::OperationUnavailable);
            RequireFailureIdentity(fixture.Advance(120), XRErrors::OperationUnavailable);
            CHECK(fixture.script.sampleCalls == 2);
        }
        SECTION("origin replacement needs a new grant and cannot consume old observations") {
            auto replacement = fixture.scope;
            replacement.space.worldOriginRevision = IdentityValue<XRWorldOriginRevision>(2);
            RequireFailureIdentity(owner.StopLightEstimation(replacement), XRErrors::IdentityStale);
            RequireFailureIdentity(fixture.Read(replacement, 100), XRErrors::IdentityStale);
            REQUIRE(owner.StopLightEstimation(fixture.scope).HasValue());
            RequireFailureIdentity(fixture.Start(replacement), XRErrors::IdentityStale);
            replacement.grant = IdentityValue<XRLightEstimateGrant>(2);
            REQUIRE(fixture.Start(replacement).HasValue());
            RequireFailureIdentity(fixture.Read(replacement, 100), XRErrors::OperationUnavailable);
            RequireFailureIdentity(fixture.Read(100), XRErrors::IdentityStale);
        }
        SECTION("a failed child retirement fences reads and blocks parent destruction until retry") {
            fixture.script.failRetirement = true;
            const auto failed = owner.RetireSession();
            fixture.script.failRetirement = false;
            RequireFailureIdentity(failed, XRErrors::OperationUnavailable);
            CHECK(fixture.script.live);
            CHECK(fixture.script.retireCalls == 1);
            CHECK(fixture.native.native.liveSessions == 1);
            RequireFailureIdentity(fixture.Read(110), XRErrors::OperationUnavailable);
        }

        REQUIRE(owner.Close().HasValue());
        CHECK_FALSE(fixture.script.live);
        CHECK(fixture.native.native.liveSessions == 0);
    }

    TEST_CASE("OpenXR focus interruption withdraws the exact light grant across native calls",
              "[unit][xr][native][lighting][light-focus-revocation]") {
        LightFixture fixture;
        fixture.native.census.lightEstimationSupport = XR_TRUE;
        auto &owner = fixture.native.owner;
        LightEvents events{fixture};

        SECTION("focus interruption during creation rolls back the unpublished estimator") {
            fixture.script.onCreate = [&events] {
                events.InterruptFocus();
            };
            RequireFailureIdentity(fixture.Start(fixture.scope), XRErrors::PermissionDenied);
            CHECK_FALSE(fixture.script.live);
            CHECK(fixture.script.retireCalls == 1);
            REQUIRE(owner.Close().HasValue());
        }
        SECTION("the native query cannot publish after the same event batch restores focus") {
            REQUIRE(fixture.Start(fixture.scope).HasValue());
            REQUIRE(fixture.Advance(100).Value());
            fixture.script.onSample = [&events] {
                events.InterruptFocus();
            };
            fixture.script.sampleTime = 110;
            RequireFailureIdentity(fixture.Advance(110), XRErrors::OperationUnavailable);
            RequireFailureIdentity(fixture.Read(110), XRErrors::OperationUnavailable);
            REQUIRE(owner.Close().HasValue());
        }
        SECTION("a read that loses focus during native validation cannot return its cached sample") {
            REQUIRE(fixture.Start(fixture.scope).HasValue());
            REQUIRE(fixture.Advance(100).Value());
            bool interrupt = true;
            fixture.native.fence.onValidate = [&events, &interrupt] {
                if (std::exchange(interrupt, false))
                    events.InterruptFocus();
            };
            RequireFailureIdentity(fixture.Read(101), XRErrors::OperationUnavailable);
            fixture.native.fence.onValidate = {};
            RequireFailureIdentity(fixture.Read(102), XRErrors::OperationUnavailable);
            REQUIRE(owner.StopLightEstimation(fixture.scope).HasValue());
            RequireFailureIdentity(fixture.Start(fixture.scope), XRErrors::IdentityStale);
            auto fresh = fixture.scope;
            fresh.grant = IdentityValue<XRLightEstimateGrant>(2);
            REQUIRE(fixture.Start(fresh).HasValue());
            RequireFailureIdentity(fixture.Read(fresh, 102), XRErrors::OperationUnavailable);
            REQUIRE(owner.Close().HasValue());
        }
    }

    TEST_CASE("OpenXR discovery observes actual pre-session system views and distinct runtime implementation version",
              "[unit][xr][native]") {
        CensusFixture fixture;
        REQUIRE(fixture.owner.PrepareDiscovery(fixture.Discovery()).HasValue());
        const auto actual = fixture.owner.DiscoverSystem(fixture.preflight.Attempt());
        REQUIRE(actual.HasValue());
        CHECK(actual.Value().runtime.runtimeVersion == XR_MAKE_VERSION(9, 8, 7));
        CHECK(actual.Value().primaryViewCount == 2);
        CHECK(actual.Value().primaryViews[0].recommendedImageRectWidth == 1024);
        CHECK(actual.Value().primaryBlendModes[0] == XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
        CHECK(fixture.native.liveSessions == 0);
        CHECK(fixture.owner.DiscoverSession(fixture.Preparation()).HasError());
        CHECK(std::ranges::count(fixture.census.calls, "spaces") == 0);
        CHECK(std::ranges::count(fixture.census.calls, "formats") == 0);
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR quad discovery preserves native role order without changing stereo admission", "[unit][xr][native][quad]") {
        using enum XRCapabilityState;
        CensusFixture fixture;
        fixture.census.advertisesQuad = true;
        auto expected = Available;
        auto discovery = fixture.Discovery();
        constexpr std::array<std::string_view, 1> legacy{XR_VARJO_QUAD_VIEWS_EXTENSION_NAME};
        const std::array routes{NativeExtensionRequest{legacy.front()}};

        SECTION("OpenXR 1.1 discovers the core view configuration without a legacy extension") {}
        SECTION("OpenXR 1.0 requires the legacy route to be enabled") {
            discovery.requestedApi = {1, 0, 0};
            expected = DependencyMissing;
        }
        SECTION("OpenXR 1.0 can use the explicitly enabled legacy route") {
            discovery.requestedApi = {1, 0, 0};
            fixture.native.extraExtensions = legacy;
            ++fixture.native.extensionCount;
            discovery.extensions = routes;
        }
        SECTION("Absence does not enumerate quad geometry") {
            fixture.census.advertisesQuad = false;
            expected = Unsupported;
        }
        SECTION("An oversized view list is rejected whole") {
            fixture.census.quadViews = 5;
            expected = Incompatible;
        }
        SECTION("A short view list is rejected whole") {
            fixture.census.quadViews = 3;
            expected = Incompatible;
        }
        SECTION("One malformed inset suppresses the whole optional result") {
            fixture.census.invalidQuadView = true;
            expected = Incompatible;
        }
        SECTION("Duplicate blend modes invalidate the optional result") {
            fixture.census.duplicateQuadBlend = true;
            expected = Incompatible;
        }
        SECTION("A native query failure remains distinct from a malformed list") {
            fixture.census.fail = "quad-views";
            expected = Unavailable;
        }

        REQUIRE(fixture.owner.PrepareDiscovery(discovery).HasValue());
        const auto observed = fixture.owner.DiscoverSystem(fixture.preflight.Attempt());
        REQUIRE(observed.HasValue());
        CHECK(observed.Value().primaryViewCount == 2);
        const auto quad = fixture.owner.DiscoverQuadViews(fixture.preflight.Attempt(), observed.Value());
        REQUIRE(quad.HasValue());
        CHECK(quad.Value().state == expected);
        CHECK(fixture.native.liveSessions == 0);
        if (expected == Available) {
            constexpr std::array roles{XRViewRole::PrimaryLeft, XRViewRole::PrimaryRight, XRViewRole::FoveatedInsetLeft,
                                       XRViewRole::FoveatedInsetRight};
            for (std::size_t index = 0; index < roles.size(); ++index) {
                const auto &view = quad.Value().views[index];
                CHECK(view.role == roles[index]);
                CHECK(view.recommendedExtent.width == 1024 - index * 128);
                CHECK(view.maximumExtent.width == 2048);
                CHECK(view.recommendedSamples == 1);
            }
            REQUIRE(quad.Value().blendModeCount == 2);
            CHECK(quad.Value().blendModes[1] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND);
        } else {
            CHECK(quad.Value().blendModeCount == 0);
            CHECK_FALSE(quad.Value().views.front().recommendedExtent.IsValid());
            if (expected == Unsupported || expected == DependencyMissing)
                CHECK(std::ranges::count(fixture.census.calls, "quad-views") == 0);
        }
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.owner.DiscoverQuadViews(fixture.preflight.Attempt(), observed.Value()).HasError());
    }

    TEST_CASE("OpenXR eye gaze support is a bounded cold census and malformed optional data leaves baseline available",
              "[unit][xr][native][gaze]") {
        using enum XRCapabilityState;
        CensusFixture fixture;
        fixture.native.gazeExtension = true;
        fixture.native.extensionCount = 2;
        fixture.census.gazeSupport = XR_TRUE;
        XRCapabilityState expected = Available;
        bool request = true;
        SECTION("extension advertisement without instance enablement") {
            request = false;
            expected = Unsupported;
        }
        SECTION("runtime reports a supported device") {}
        SECTION("runtime reports an unsupported device") {
            fixture.census.gazeSupport = XR_FALSE;
            expected = Unsupported;
        }
        SECTION("native permission denial grants no authority") {
            fixture.census.gazeResult = XR_ERROR_PERMISSION_INSUFFICIENT;
            expected = Denied;
        }
        SECTION("unavailable optional system property") {
            fixture.census.gazeResult = XR_ERROR_RUNTIME_FAILURE;
            expected = Unavailable;
        }
        SECTION("malformed boolean fails the optional observation") {
            fixture.census.gazeSupport = 9;
            expected = Incompatible;
        }
        const std::array routes{NativeExtensionRequest{XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME}};
        auto discovery = fixture.Discovery();
        if (request)
            discovery.extensions = routes;
        REQUIRE(fixture.owner.PrepareDiscovery(discovery).HasValue());
        const auto actual = fixture.owner.DiscoverSystem(fixture.preflight.Attempt());
        REQUIRE(actual.HasValue());
        CHECK(actual.Value().eyeGazeProvider == expected);
        CHECK(actual.Value().system.next == nullptr);
        CHECK(actual.Value().primaryViewCount == 2);
        CHECK(std::ranges::count(fixture.census.calls, "eye-gaze-properties") == static_cast<int>(request));
        CHECK(fixture.native.liveSessions == 0);
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.owner.DiscoverSystem(fixture.preflight.Attempt()).HasError());
    }

    TEST_CASE("OpenXR spatial plane census remains cold and follows exact enabled capabilities", "[unit][xr][native][planes]") {
        using enum XRCapabilityState;
        CensusFixture fixture;
        constexpr std::array<std::string_view, 3> extensions{XR_EXT_SPATIAL_PLANE_TRACKING_EXTENSION_NAME,
                                                             XR_EXT_SPATIAL_ENTITY_EXTENSION_NAME, XR_EXT_FUTURE_EXTENSION_NAME};
        const std::array routes{NativeExtensionRequest{extensions[0]}, NativeExtensionRequest{extensions[1]},
                                NativeExtensionRequest{extensions[2]}};
        fixture.native.extraExtensions = extensions;
        fixture.native.extensionCount += static_cast<std::uint32_t>(extensions.size());
        auto discovery = fixture.Discovery();
        discovery.extensions = routes;
        auto expected = Available;
        bool labels = true;
        SECTION("all required components exist") {}
        SECTION("semantics are optional") {
            fixture.census.spatialLabels = false;
            labels = false;
        }
        SECTION("require enabled future and spatial entity dependencies") {
            discovery.extensions = std::span{routes}.first(2);
            expected = DependencyMissing;
        }
        SECTION("advertising does not enable") {
            discovery.extensions = {};
            expected = Unsupported;
        }
        SECTION("selected runtime provides another capability set") {
            fixture.census.spatialPlanes = false;
            expected = Unsupported;
        }
        SECTION("bounded geometry is required") {
            fixture.census.spatialBounds = false;
            expected = Incompatible;
        }
        SECTION("runtime has denied permission") {
            fixture.census.spatialComponentsResult = XR_ERROR_PERMISSION_INSUFFICIENT;
            expected = Denied;
        }
        SECTION("large tool publications are not truncated") {
            fixture.census.spatialComponentsResult = XR_ERROR_SIZE_INSUFFICIENT;
            expected = Incompatible;
        }
        REQUIRE(fixture.owner.PrepareDiscovery(discovery).HasValue());
        const auto actual = fixture.owner.DiscoverSystem(fixture.preflight.Attempt());
        REQUIRE(actual.HasValue());
        CHECK(actual.Value().planes.state == expected);
        CHECK(actual.Value().planes.semanticLabels == (expected == Available && labels));
        CHECK(actual.Value().primaryViewCount == 2);
        CHECK(fixture.native.liveSessions == 0);
        CHECK(std::ranges::count(fixture.census.calls, "spatial-capabilities") == (discovery.extensions.size() == routes.size()));
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR spatial snapshot joins tracked native geometry with optional semantic labels", "[unit][xr][native][planes]") {
        PlaneSnapshotFixture fixture;
        auto census = NativePlaneCensus{XRCapabilityState::Available, true};
        SECTION("optional labels are queried separately") {}
        SECTION("unsupported labels remain unknown") {
            census.semanticLabels = false;
        }
        const auto snapshot = NativeHandle<XrSpatialSnapshotEXT>(1);
        const auto result = ReadNativeSpatialPlanes(snapshot, &PlaneSnapshotFixture::Query, census, 4);
        REQUIRE(result.HasValue());
        const auto planes = result.Value().Planes();
        REQUIRE(planes.size() == 2);
        CHECK(planes[0].entity == 5);
        CHECK(planes[1].entity == 20);
        CHECK(planes[0].classification == (census.semanticLabels ? XRPlaneClassification::Platform : XRPlaneClassification::Unknown));
        CHECK(planes[1].classification == XRPlaneClassification::Unknown);
        CHECK(planes[1].confidence == XRTrackingConfidence::High);
        CHECK(planes[1].centerMeters.z == 3.0F);
        CHECK(planes[1].extentMeters.y == 5.0F);
        const auto normal = planes[1].orientation.Rotate({0.0F, 1.0F, 0.0F});
        CHECK(normal.x == Catch::Approx(0.0F).margin(0.00001F));
        CHECK(normal.y == Catch::Approx(0.0F).margin(0.00001F));
        CHECK(normal.z == Catch::Approx(1.0F));
        CHECK(fixture.labelCalls == static_cast<std::uint32_t>(census.semanticLabels));
    }

    TEST_CASE("OpenXR spatial snapshots reject invalid native batches and remove untracked geometry", "[unit][xr][native][planes]") {
        PlaneSnapshotFixture fixture;
        const auto snapshot = NativeHandle<XrSpatialSnapshotEXT>(1);
        const auto read = [&](std::uint32_t capacity = 4) {
            return ReadNativeSpatialPlanes(snapshot, &PlaneSnapshotFixture::Query, {XRCapabilityState::Available, true}, capacity);
        };
        SECTION("native oversized batch is actionable") {
            RequireFailureIdentity(read(2), XRErrors::CapacityExceeded);
        }
        SECTION("duplicated native identity") {
            fixture.duplicate = true;
            RequireFailureIdentity(read(), XRErrors::TrackingSnapshotInvalid);
        }
        SECTION("inconsistent native counts") {
            fixture.countMismatch = true;
            RequireFailureIdentity(read(), XRErrors::TrackingSnapshotInvalid);
        }
        SECTION("unknown labeled plane in the same snapshot") {
            fixture.staleLabel = true;
            RequireFailureIdentity(read(), XRErrors::TrackingSnapshotInvalid);
        }
        SECTION("malformed native rectangle") {
            fixture.malformed = true;
            RequireFailureIdentity(read(), XRErrors::TrackingSnapshotInvalid);
        }
        SECTION("snapshot lost while querying") {
            fixture.result = XR_SESSION_LOSS_PENDING;
            RequireFailureIdentity(read(), XRErrors::IdentityStale);
        }
        SECTION("paused and stopped evidence disappears without inspecting its geometry") {
            fixture.noTracking = true;
            fixture.malformed = true;
            const auto result = read();
            REQUIRE(result.HasValue());
            CHECK(result.Value().Planes().empty());
            CHECK(fixture.calls == 1);
        }
    }

    TEST_CASE("OpenXR plane capture owns asynchronous work and publishes only completed authorized scopes", "[unit][xr][native][planes]") {
        using enum XRCapabilityState;
        CensusFixture fixture;
        SpatialCaptureScript script;
        PlaneSnapshotFixture geometry;
        constexpr std::array<std::string_view, 3> extensions{XR_EXT_FUTURE_EXTENSION_NAME, XR_EXT_SPATIAL_ENTITY_EXTENSION_NAME,
                                                             XR_EXT_SPATIAL_PLANE_TRACKING_EXTENSION_NAME};
        const std::array routes{NativeExtensionRequest{extensions[0]}, NativeExtensionRequest{extensions[1]},
                                NativeExtensionRequest{extensions[2]}};
        fixture.native.extraExtensions = extensions;
        fixture.native.extensionCount += static_cast<std::uint32_t>(extensions.size());
        fixture.census.extraProc = &SpatialCaptureScript::GetProc;
        const auto capabilities = Capabilities();
        const auto plan = Plan(capabilities);
        ObservationEventResources resources;
        XRSessionLifecycle sessions{resources};
        const auto active = sessions.Activate(capabilities, capabilities.System(), capabilities.Revision(), plan);
        REQUIRE(active.HasValue());
        const auto session = active.Value();
        const auto attempt = fixture.preflight.Attempt();
        NativeSessionRequest request{.candidate = session,
                                     .preflight = fixture.preflight,
                                     .capabilities = capabilities,
                                     .plan = plan,
                                     .activeAttempt = attempt,
                                     .extensions = routes};
        REQUIRE(fixture.owner.Create(request).HasValue());
        const XRPlaneScope scope{session,
                                 {{session, {2, 1}}, XRSpaceKind::World, IdentityValue<XRWorldOriginRevision>(1)},
                                 IdentityValue<XRPlaneGrant>(1)};
        const XRPlaneLimits limits{4, Duration::FromNanoseconds(10), Duration::FromNanoseconds(30)};
        OpenXRPlaneCapture capture{fixture.owner};
        const auto time = [](const std::int64_t tick) {
            return XRRuntimeTime::Create(tick).Value();
        };
        const auto world = NativeHandle<XrSpace>(4);
        const auto advance = [&](const std::int64_t tick) {
            return capture.Advance(scope, world, time(tick));
        };
        const auto noPublication = [&](const std::int64_t tick) {
            const auto result = advance(tick);
            REQUIRE(result.HasValue());
            CHECK_FALSE(result.Value());
        };
        const auto publish = [&](const std::int64_t tick) {
            const auto result = advance(tick);
            REQUIRE(result.HasValue());
            CHECK(result.Value());
            const auto snapshot = capture.Read(scope, time(tick));
            REQUIRE(snapshot.HasValue());
            return snapshot.Value();
        };
        RequireFailureIdentity(capture.Start(attempt, scope, XR_NULL_HANDLE, limits, time(100)), XRErrors::CoordinateSpaceIncompatible);
        CHECK(script.contextStarts == 0);
        REQUIRE(capture.Start(attempt, scope, world, limits, time(100)).HasValue());
        SECTION("timed-out context cancels its one outstanding future") {
            RequireFailureIdentity(advance(131), XRErrors::TrackingSnapshotStale);
            CHECK(script.cancels == 1);
        }
        SECTION("context and discoveries use the same exact World grant") {
            noPublication(102);
            script.ready = true;
            noPublication(103);
            noPublication(109);
            CHECK(script.scans == 0);
            noPublication(110);
            CHECK(script.scans == 1);
            const auto first = publish(111);
            REQUIRE(first.Planes().size() == 2);
            const auto original = first.Planes()[0].id;
            CHECK(first.Planes()[0].classification == XRPlaneClassification::Platform);
            CHECK_FALSE(script.hasSnapshot);
            noPublication(121);

            SECTION("focus recovery cannot authorize geometry returned by an interrupted native query") {
                for (const auto event : {XRSessionEvent::Started, XRSessionEvent::BecameVisible, XRSessionEvent::BecameFocused})
                    REQUIRE(sessions.ApplyEvent(session, event).HasValue());
                XRFrameLifecycle frames{sessions};
                OpenXRSessionEvents events{fixture.owner, sessions, frames};
                REQUIRE(events.Bind({&EventScript::GetProc}, {}).HasValue());
                EventScript eventScript;
                eventScript.session = fixture.native.lastSession;
                eventScript.Push(XR_SESSION_STATE_VISIBLE);
                eventScript.Push(XR_SESSION_STATE_FOCUSED);
                geometry.onQuery = [&events, &eventScript] {
                    EventScript::active = &eventScript;
                    const auto observed = events.Poll(true);
                    EventScript::active = nullptr;
                    REQUIRE(observed.HasValue());
                    CHECK(observed.Value().actionsInterrupted);
                };
                const auto labels = geometry.labelCalls;
                const auto interrupted = advance(122);
                geometry.onQuery = {};
                RequireFailureIdentity(interrupted, XRErrors::PermissionDenied);
                CHECK(sessions.Snapshot().admitsActions);
                CHECK(geometry.labelCalls == labels);
                CHECK_FALSE(script.hasSnapshot);
                RequireFailureIdentity(capture.Read(scope, time(122)), XRErrors::PermissionDenied);
                REQUIRE(capture.Close().HasValue());
                RequireFailureIdentity(capture.Start(attempt, scope, world, limits, time(123)), XRErrors::IdentityStale);
            }
            SECTION("paused world observations expire and recovered entities use new portable generations") {
                geometry.noTracking = true;
                CHECK(publish(122).Planes().empty());
                geometry.noTracking = false;
                noPublication(132);
                const auto recovered = publish(133);
                REQUIRE(recovered.Planes().size() == 2);
                CHECK(recovered.Planes()[0].id != original);
            }
            SECTION("native permission loss immediately revokes the previously published result") {
                script.completeResult = XR_ERROR_PERMISSION_INSUFFICIENT;
                RequireFailureIdentity(advance(122), XRErrors::PermissionDenied);
                RequireFailureIdentity(capture.Read(scope, time(122)), XRErrors::OperationUnavailable);
            }
            SECTION("native query denial revokes old geometry even when snapshot cleanup fails") {
                geometry.result = XR_ERROR_PERMISSION_INSUFFICIENT;
                script.failSnapshotDestroy = true;
                const auto failed = advance(122);
                RequireFailureIdentity(failed, XRErrors::OperationUnavailable);
                REQUIRE(failed.ErrorValue().cause.Get());
                CHECK(failed.ErrorValue().cause.Get()->code.Value() == XRErrors::PermissionDenied.code.Value());
                CHECK(script.hasSnapshot);
                RequireFailureIdentity(capture.Read(scope, time(122)), XRErrors::OperationUnavailable);
                RequireFailureIdentity(capture.Close(), XRErrors::OperationUnavailable);
                script.failSnapshotDestroy = false;
                REQUIRE(capture.Close().HasValue());
                CHECK_FALSE(script.hasSnapshot);
            }
            SECTION("snapshot cleanup distinguishes access loss from a transient provider failure") {
                const auto [status, expected] = GENERATE(Catch::Generators::table<XrResult, const ErrorCodeDescriptor *>(
                    {{XR_ERROR_PERMISSION_INSUFFICIENT, &XRErrors::PermissionDenied},
                     {XR_ERROR_SESSION_LOST, &XRErrors::IdentityStale},
                     {XR_ERROR_FEATURE_UNSUPPORTED, &XRErrors::OperationUnsupported},
                     {XR_ERROR_RUNTIME_FAILURE, &XRErrors::OperationUnavailable}}));
                script.snapshotDestroyResult = status;
                const auto failed = advance(122);
                CHECK(script.hasSnapshot);
                const auto previous = capture.Read(scope, time(122));
                const auto polls = script.polls;
                script.snapshotDestroyResult = XR_SUCCESS;
                const auto resumed = status == XR_ERROR_RUNTIME_FAILURE ? failed : advance(123);
                CHECK(script.polls == polls);
                REQUIRE(capture.Close().HasValue());
                CHECK_FALSE(script.hasSnapshot);
                RequireFailureIdentity(failed, *expected);
                if (status == XR_ERROR_RUNTIME_FAILURE) {
                    REQUIRE(previous.HasValue());
                    CHECK(previous.Value().Planes()[0].id == original);
                } else {
                    RequireFailureIdentity(previous, XRErrors::OperationUnavailable);
                    RequireFailureIdentity(resumed, XRErrors::OperationUnavailable);
                }
            }
            SECTION("withdrawal in a native callback stops the remaining reads and permits a new grant after retirement") {
                enum class Revocation {
                    Poll,
                    Complete,
                    Geometry
                };
                const auto where = GENERATE(Revocation::Poll, Revocation::Complete, Revocation::Geometry);
                const auto revoke = [&capture] {
                    capture.Revoke();
                };
                if (where == Revocation::Poll)
                    script.onPoll = revoke;
                else if (where == Revocation::Complete)
                    script.onCompleteScan = revoke;
                else
                    geometry.onQuery = revoke;
                const auto queries = geometry.calls;
                const auto labels = geometry.labelCalls;
                RequireFailureIdentity(advance(122), XRErrors::PermissionDenied);
                RequireFailureIdentity(capture.Read(scope, time(122)), XRErrors::OperationUnavailable);
                CHECK(geometry.calls == queries + (where == Revocation::Geometry ? 1U : 0U));
                CHECK(geometry.labelCalls == labels);
                CHECK_FALSE(script.hasSnapshot);
                script.onPoll = {};
                script.onCompleteScan = {};
                geometry.onQuery = {};

                script.failDestroy = true;
                RequireFailureIdentity(capture.Close(), XRErrors::OperationUnavailable);
                CHECK(script.hasContext);
                auto renewed = scope;
                renewed.grant = IdentityValue<XRPlaneGrant>(2);
                RequireFailureIdentity(capture.Start(attempt, renewed, world, limits, time(123)), XRErrors::OperationIncompatible);
                script.failDestroy = false;
                REQUIRE(capture.Close().HasValue());
                REQUIRE(capture.Start(attempt, renewed, world, limits, time(130)).HasValue());
                REQUIRE(capture.Advance(renewed, world, time(131)).HasValue());
                REQUIRE(capture.Advance(renewed, world, time(140)).HasValue());
                REQUIRE(capture.Advance(renewed, world, time(141)).HasValue());
                REQUIRE(capture.Read(renewed, time(141)).HasValue());
                RequireFailureIdentity(capture.Read(scope, time(141)), XRErrors::IdentityStale);
            }
            SECTION("a different native space revokes the unchanged portable grant before polling") {
                const auto polls = script.polls;
                const auto queries = geometry.calls;
                const bool missingSpace = GENERATE(false, true);
                const auto replacement = missingSpace ? XR_NULL_HANDLE : NativeHandle<XrSpace>(5);
                RequireFailureIdentity(capture.Advance(scope, replacement, time(122)), XRErrors::CoordinateSpaceIncompatible);
                RequireFailureIdentity(capture.Read(scope, time(122)), XRErrors::OperationUnavailable);
                RequireFailureIdentity(advance(122), XRErrors::OperationUnavailable);
                CHECK(script.polls == polls);
                CHECK(geometry.calls == queries);
                CHECK(script.cancels == 0);
                REQUIRE(capture.Close().HasValue());
                CHECK(script.cancels == 1);
            }
            SECTION("recenter rejects the old grant and retirement retains retryable native debt") {
                auto replacement = scope;
                replacement.space.worldOriginRevision = IdentityValue<XRWorldOriginRevision>(2);
                RequireFailureIdentity(capture.Advance(replacement, world, time(122)), XRErrors::IdentityStale);
                RequireFailureIdentity(capture.Read(scope, time(122)), XRErrors::OperationUnavailable);
                script.failDestroy = true;
                RequireFailureIdentity(capture.Close(), XRErrors::OperationUnavailable);
                CHECK(script.cancels == 1);
                CHECK(script.hasContext);
                script.failDestroy = false;
            }
        }
        REQUIRE(capture.Close().HasValue());
        CHECK_FALSE(script.hasContext);
        CHECK_FALSE(script.hasSnapshot);
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR reprojection census observes advertised routes without enabling them or issuing sizing calls",
              "[unit][xr][native][reprojection]") {
        CensusFixture fixture;
        NativeReprojectionRoute expected = NativeReprojectionRoute::None;
        SECTION("no route") {}
        SECTION("legacy native offer") {
            fixture.native.spaceWarpExtension = true;
            expected = NativeReprojectionRoute::SpaceWarpFb;
        }
        SECTION("standard offer preferred with both advertised") {
            fixture.native.spaceWarpExtension = true;
            fixture.native.synthesisExtension = true;
            expected = NativeReprojectionRoute::FrameSynthesisExt;
        }
        fixture.native.extensionCount += fixture.native.spaceWarpExtension + fixture.native.synthesisExtension;
        REQUIRE(fixture.owner.PrepareDiscovery(fixture.Discovery()).HasValue());
        const auto callCount = fixture.census.calls.size();
        const auto offer = fixture.owner.DiscoverReprojection(fixture.preflight.Attempt());
        REQUIRE(offer.HasValue());
        CHECK(offer.Value().route == expected);
        CHECK(offer.Value().availabilityWithoutProducer ==
              (expected == NativeReprojectionRoute::None ? XRCapabilityState::Unsupported : XRCapabilityState::DependencyMissing));
        CHECK_FALSE(offer.Value().extents[0].IsValid());
        CHECK(fixture.census.calls.size() == callCount);
        CHECK(std::ranges::find(fixture.native.enabledExtensions, XR_EXT_FRAME_SYNTHESIS_EXTENSION_NAME) ==
              fixture.native.enabledExtensions.end());
        CHECK(std::ranges::find(fixture.native.enabledExtensions, XR_FB_SPACE_WARP_EXTENSION_NAME) ==
              fixture.native.enabledExtensions.end());
        REQUIRE(fixture.owner.DiscoverSystem(fixture.preflight.Attempt()).HasValue());
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.owner.DiscoverReprojection(fixture.preflight.Attempt()).HasError());
    }

    TEST_CASE("OpenXR enabled synthesis census checks native sizing without claiming renderer support",
              "[unit][xr][native][reprojection]") {
        const bool extension = GENERATE(false, true);
        CensusFixture fixture;
        fixture.native.synthesisExtension = extension;
        fixture.native.spaceWarpExtension = !extension;
        fixture.native.extensionCount = 2;
        const std::array routes{
            NativeExtensionRequest{extension ? XR_EXT_FRAME_SYNTHESIS_EXTENSION_NAME : XR_FB_SPACE_WARP_EXTENSION_NAME}};
        auto discovery = fixture.Discovery();
        discovery.extensions = routes;
        REQUIRE(fixture.owner.PrepareDiscovery(discovery).HasValue());
        const auto offer = fixture.owner.DiscoverReprojection(fixture.preflight.Attempt());
        REQUIRE(offer.HasValue());
        CHECK(offer.Value().route == (extension ? NativeReprojectionRoute::FrameSynthesisExt : NativeReprojectionRoute::SpaceWarpFb));
        CHECK(offer.Value().version == 1);
        CHECK(offer.Value().availabilityWithoutProducer == XRCapabilityState::DependencyMissing);
        CHECK(offer.Value().extents[0] == Render::FramebufferExtent{640, 512});
        CHECK(offer.Value().extents[1] == Render::FramebufferExtent{extension ? 800U : 640U, 512});
        fixture.census.invalidReprojectionExtent = true;
        const auto incompatible = fixture.owner.DiscoverReprojection(fixture.preflight.Attempt());
        REQUIRE(incompatible.HasValue());
        CHECK(incompatible.Value().availabilityWithoutProducer == XRCapabilityState::Incompatible);
        CHECK_FALSE(incompatible.Value().extents[0].IsValid());
        CHECK_FALSE(incompatible.Value().extents[1].IsValid());
        REQUIRE(fixture.owner.DiscoverSystem(fixture.preflight.Attempt()).HasValue());
        fixture.census.fail = "system-properties";
        const auto unavailable = fixture.owner.DiscoverReprojection(fixture.preflight.Attempt());
        REQUIRE(unavailable.HasValue());
        CHECK(unavailable.Value().availabilityWithoutProducer == XRCapabilityState::TemporarilyUnavailable);
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR session-dependent census runs only on the actual unpublished prepared session", "[unit][xr][native]") {
        CensusFixture fixture;
        REQUIRE(fixture.owner.PrepareDiscovery(fixture.Discovery()).HasValue());
        REQUIRE(fixture.owner.PrepareSession(fixture.Preparation()).HasValue());
        const auto actual = fixture.owner.DiscoverSession(fixture.Preparation());
        REQUIRE(actual.HasValue());
        CHECK(actual.Value().spaceCount == 2);
        CHECK(actual.Value().spaces[0] == XR_REFERENCE_SPACE_TYPE_VIEW);
        CHECK(actual.Value().spaces[1] == XR_REFERENCE_SPACE_TYPE_LOCAL);
        CHECK(actual.Value().formatCount == 2);
        CHECK(actual.Value().formats[0] == 37);
        CHECK(actual.Value().displayRates.state == XRCapabilityState::Unsupported);
        CHECK(std::ranges::find(fixture.census.calls, "display-rates") == fixture.census.calls.end());
        CHECK_FALSE(fixture.owner.Session().IsValid());
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR display census validates the optional runtime list before publishing any rates", "[unit][xr][native][display]") {
        CensusFixture fixture;
        constexpr std::array<std::string_view, 1> extensions{XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME};
        const std::array routes{NativeExtensionRequest{extensions.front()}};
        fixture.native.extraExtensions = extensions;
        ++fixture.native.extensionCount;
        auto discovery = fixture.Discovery();
        discovery.extensions = routes;
        std::array rates{59.94F, 72.0F, 120.0F};
        std::array<float, NativeDisplayRates::MaximumRates + 1> overflow{};
        fixture.census.displayRates = rates;
        auto expected = XRCapabilityState::Available;
        SECTION("the exact runtime rates are expressed in millihertz") {}
        SECTION("too many rates leave no partial observation") {
            fixture.census.displayRates = overflow;
            expected = XRCapabilityState::Incompatible;
        }
        SECTION("a changed count leaves no partial observation") {
            fixture.census.changedDisplayRateCount = true;
            expected = XRCapabilityState::Incompatible;
        }
        SECTION("invalid rates leave no partial observation") {
            rates.back() = std::numeric_limits<float>::quiet_NaN();
            expected = XRCapabilityState::Incompatible;
        }
        SECTION("out-of-order rates leave no partial observation") {
            std::swap(rates.front(), rates.back());
            expected = XRCapabilityState::Incompatible;
        }
        SECTION("observed session loss prevents a successful census") {
            fixture.census.displayRateResult = XR_SESSION_LOSS_PENDING;
        }
        REQUIRE(fixture.owner.PrepareDiscovery(discovery).HasValue());
        REQUIRE(fixture.owner.PrepareSession(fixture.Preparation()).HasValue());
        const auto observed = fixture.owner.DiscoverSession(fixture.Preparation());
        if (fixture.census.displayRateResult == XR_SESSION_LOSS_PENDING) {
            CHECK(observed.HasError());
        } else {
            REQUIRE(observed.HasValue());
            const auto &display = observed.Value().displayRates;
            CHECK(display.state == expected);
            if (expected == XRCapabilityState::Available) {
                REQUIRE(display.count == 3);
                const std::array expectedRates{59'940U, 72'000U, 120'000U};
                CHECK(std::ranges::equal(std::span{display.milliHertz}.first(display.count), expectedRates));
            } else {
                CHECK(display.count == 0);
                CHECK(display.milliHertz.front() == 0);
            }
        }
        CHECK_FALSE(fixture.owner.Session().IsValid());
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR native census rejects oversized malformed or duplicate publications without truncation", "[unit][xr][native]") {
        CensusFixture fixture;
        SECTION("too many views") {
            fixture.census.views = XRHardLimits::MaximumViews + 1;
        }
        SECTION("malformed view limits") {
            fixture.census.invalidView = true;
        }
        REQUIRE(fixture.owner.PrepareDiscovery(fixture.Discovery()).HasValue());
        CHECK(fixture.owner.DiscoverSystem(fixture.preflight.Attempt()).HasError());
        CHECK(fixture.native.liveSessions == 0);
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR unpublished session census failures never grant Ready or lose native ownership", "[unit][xr][native]") {
        CensusFixture fixture;
        SECTION("no spaces") {
            fixture.census.spaces = 0;
        }
        SECTION("oversized formats") {
            fixture.census.formats = 129;
        }
        SECTION("duplicate spaces") {
            fixture.census.duplicateSpaces = true;
        }
        SECTION("native failure") {
            fixture.census.fail = "spaces";
        }
        REQUIRE(fixture.owner.PrepareDiscovery(fixture.Discovery()).HasValue());
        REQUIRE(fixture.owner.PrepareSession(fixture.Preparation()).HasValue());
        CHECK(fixture.owner.DiscoverSession(fixture.Preparation()).HasError());
        CHECK_FALSE(fixture.owner.Session().IsValid());
        CHECK(fixture.native.liveSessions == 1);
        CHECK(fixture.native.liveInstances == 1);
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.native.liveSessions == 0);
        CHECK(fixture.native.liveInstances == 0);
    }
}  // namespace Horo::Tests::OpenXR
