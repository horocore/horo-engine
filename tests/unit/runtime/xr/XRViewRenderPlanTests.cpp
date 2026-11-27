#include "Horo/Runtime/Render/MultiviewExtractionErrors.h"
#include "Horo/XR/XRErrors.h"
#include "Horo/XR/XRMultiviewScene.h"
#include "Horo/XR/XRViewRenderPlan.h"
#include "support/AllocationProbe.h"
#include "support/TypedIdentityTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace Horo::XR {
    namespace {
        [[nodiscard]] XRSessionId Session(const std::uint32_t generation = 5) {
            return {{Horo::Tests::IdentityValue<XRRuntimeGeneration>(1), {2, 3}}, {4, generation}};
        }

        [[nodiscard]] XRCoordinateSpace Space(const XRSessionId &session, const XRSpaceKind kind, const std::uint32_t slot) {
            return {.id = {session, {slot, 1}}, .kind = kind, .worldOriginRevision = Horo::Tests::IdentityValue<XRWorldOriginRevision>(9)};
        }

        [[nodiscard]] XRPoseDescriptor PresentationPose(const XRSessionId &session, const XRRenderPredictionTime prediction) {
            return {.session = session,
                    .source = Space(session, XRSpaceKind::View, 20),
                    .target = Space(session, XRSpaceKind::Local, 21),
                    .purpose = XRPosePurpose::PresentationPrediction,
                    .time = {.runtimeSample = Horo::Tests::IdentityValue<XRRuntimeTime>(90), .renderPrediction = prediction},
                    .components = {
                        .positionMeters = {.value = Math::Vec3{0.0F, 1.7F, 0.0F}, .validity = XRPoseComponentValidity::Tracked},
                        .orientation = {.value = Math::Quaternion::Identity(), .validity = XRPoseComponentValidity::Tracked},
                        .confidence = XRTrackingConfidence::High,
                        .loss = XRTrackingLossState::None,
                    }};
        }

        template <typename Value> void RequireFailureIdentity(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE_FALSE(result.HasValue());
            CHECK(result.ErrorValue().domain.Value() == expected.domain.Value());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }

        class PlanFixture final {
        public:
            explicit PlanFixture(const std::size_t count = 2)
                : session(Session()), prediction(Horo::Tests::IdentityValue<XRRenderPredictionTime>(100)),
                  origin(Horo::Tests::IdentityValue<XRWorldOriginRevision>(9)),
                  revision(Horo::Tests::IdentityValue<XRViewConfigurationRevision>(7)), configurationId{session, {8, 1}} {
                REQUIRE(count > 0);
                REQUIRE(count <= views.size());
                configuration = {.session = session,
                                 .id = configurationId,
                                 .revision = revision,
                                 .type = count == 1
                                             ? XRViewConfigurationType::PrimaryMono
                                             : (count == 2 ? XRViewConfigurationType::PrimaryStereo : XRViewConfigurationType::QuadView),
                                 .blendMode = XREnvironmentBlendMode::Opaque,
                                 .viewCount = static_cast<std::uint32_t>(count)};
                constexpr std::array roles{XRViewRole::PrimaryLeft, XRViewRole::PrimaryRight, XRViewRole::FoveatedInsetLeft,
                                           XRViewRole::FoveatedInsetRight};
                for (std::size_t index = 0; index < count; ++index) {
                    views[index] = {.id = {session, {static_cast<std::uint32_t>(30 + index), 1}},
                                    .order = static_cast<std::uint32_t>(index),
                                    .role = count == 1 ? XRViewRole::Primary : roles[index],
                                    .pose = PresentationPose(session, prediction),
                                    .projection = {-1.0F, 1.0F, -1.0F, 1.0F},
                                    .recommendedExtent = {1024, 1024},
                                    .maximumExtent = {2048, 2048}};
                    const XRSwapchainTargetId target{session, {50, 1}};
                    const XRSwapchainImageId image{target, {70, 1}};
                    targets[index] = {.view = views[index].id,
                                      .target = target,
                                      .image = image,
                                      .format = Render::RenderTextureFormat::Rgba16Float,
                                      .usage = Render::RenderTextureUsage::RenderAttachment | Render::RenderTextureUsage::Sampled,
                                      .allocationExtent = {2048, 2048},
                                      .renderRectangle = {0, 0, 1024, 1024},
                                      .arrayLayer = static_cast<std::uint32_t>(index),
                                      .arrayLayerCount = static_cast<std::uint32_t>(count),
                                      .sampleCount = 1};
                    acquired[0] = image;
                }
                viewCount = count;
                targetCount = count;
                acquiredCount = 1;
            }

            [[nodiscard]] XRViewRenderPlanDescriptor Descriptor() const {
                return {.configuration = configuration,
                        .predictedDisplayTime = prediction,
                        .views = std::span{views}.first(viewCount),
                        .targets = std::span{targets}.first(targetCount),
                        .acquiredImages = std::span{acquired}.first(acquiredCount)};
            }

            [[nodiscard]] XRViewRenderPlan Create(const XRViewAdmissionPolicy policy = {}) const {
                auto result = TryCreate(policy);
                REQUIRE(result.HasValue());
                return result.Value();
            }

            [[nodiscard]] Result<XRViewRenderPlan> TryCreate(const XRViewAdmissionPolicy policy = {},
                                                             const std::uint32_t systemMaximumViews = XRHardLimits::MaximumViews) const {
                return XRViewRenderPlan::Create(Descriptor(), session, configurationId, revision, origin, systemMaximumViews, policy);
            }

            XRSessionId session;
            XRRenderPredictionTime prediction;
            XRWorldOriginRevision origin;
            XRViewConfigurationRevision revision;
            XRViewConfigurationId configurationId;
            XRViewConfigurationDescriptor configuration;
            std::array<XRViewDescriptor, 4> views{};
            std::array<XRExternalRenderTargetDescriptor, 8> targets{};
            std::array<XRSwapchainImageId, 8> acquired{};
            std::size_t viewCount{};
            std::size_t targetCount{};
            std::size_t acquiredCount{};
        };

        TEST_CASE("XR views use one bounded N-view layout for mono stereo and quad data", "[unit][xr][views]") {
            PlanFixture mono{1};
            const auto monoPlan = mono.Create({.mode = XRViewAdmissionMode::SimulatorSingleView, .maximumViews = 1});
            REQUIRE(monoPlan.Views().size() == 1);
            CHECK(monoPlan.Views().front().role == XRViewRole::Primary);

            PlanFixture stereo;
            const auto stereoPlan = stereo.Create();
            REQUIRE(stereoPlan.Views().size() == 2);
            CHECK(stereoPlan.Views()[0].role == XRViewRole::PrimaryLeft);
            CHECK(stereoPlan.Views()[1].role == XRViewRole::PrimaryRight);
            CHECK(stereoPlan.Targets().size() == 2);

            PlanFixture quad{4};
            const auto quadPlan = quad.Create({.mode = XRViewAdmissionMode::BoundedNView, .maximumViews = 4});
            REQUIRE(quadPlan.Views().size() == 4);
            CHECK(quadPlan.Views()[2].role == XRViewRole::FoveatedInsetLeft);
            CHECK(quadPlan.Views()[3].role == XRViewRole::FoveatedInsetRight);

            static_assert(std::is_trivially_copyable_v<XRSwapchainImageId>);
            static_assert(!std::is_same_v<XRSwapchainImageId, Render::RenderTextureHandle>);
        }

        TEST_CASE("Quad views require one peripheral and one inset per eye in runtime order", "[unit][xr][views]") {
            PlanFixture fixture{4};
            const XRViewAdmissionPolicy policy{.mode = XRViewAdmissionMode::BoundedNView, .maximumViews = 4};
            SECTION("Runtime order remains authoritative") {
                std::swap(fixture.views[0].role, fixture.views[2].role);
                const auto plan = fixture.Create(policy);
                REQUIRE(plan.Views().size() == 4);
                CHECK(plan.Views()[0].role == XRViewRole::FoveatedInsetLeft);
                CHECK(plan.Views()[2].role == XRViewRole::PrimaryLeft);
                CHECK(plan.Targets()[0].view == plan.Views()[0].id);
                CHECK(plan.Targets()[2].view == plan.Views()[2].id);
            }
            SECTION("A repeated role cannot stand in for a missing inset") {
                fixture.views[3].role = XRViewRole::FoveatedInsetLeft;
                RequireFailureIdentity(fixture.TryCreate(policy), XRErrors::OperationIncompatible);
            }
            SECTION("A generic view cannot stand in for a peripheral eye") {
                fixture.views[0].role = XRViewRole::Auxiliary;
                RequireFailureIdentity(fixture.TryCreate(policy), XRErrors::OperationIncompatible);
            }
        }

        TEST_CASE("XR implementation admission rejects unsupported complete configurations without truncation", "[unit][xr][views]") {
            PlanFixture quad{4};
            const auto descriptor = quad.Descriptor();
            const auto result = quad.TryCreate({.mode = XRViewAdmissionMode::PrimaryStereo, .maximumViews = 2});
            RequireFailureIdentity(result, XRErrors::OperationUnsupported);
            CHECK(descriptor.views.size() == 4);

            PlanFixture stereo;
            stereo.views[0].role = XRViewRole::PrimaryRight;
            stereo.views[1].role = XRViewRole::PrimaryLeft;
            RequireFailureIdentity(stereo.TryCreate(), XRErrors::OperationIncompatible);
        }

        TEST_CASE("XR view plan rejects count bounds ordering duplicates and malformed projection evidence", "[unit][xr][views]") {
            PlanFixture fixture;
            fixture.configuration.viewCount = 3;
            RequireFailureIdentity(fixture.TryCreate(), XRErrors::ViewPlanInvalid);

            fixture.configuration.viewCount = 2;
            fixture.views[1].order = 0;
            RequireFailureIdentity(fixture.TryCreate(), XRErrors::ViewPlanInvalid);
            fixture.views[1].order = 1;
            fixture.views[1].id = fixture.views[0].id;
            RequireFailureIdentity(fixture.TryCreate(), XRErrors::ViewPlanInvalid);
            fixture.views[1].id = {fixture.session, {31, 1}};
            fixture.targets[1].view = fixture.views[1].id;
            fixture.views[0].projection.rightTangent = std::numeric_limits<float>::infinity();
            RequireFailureIdentity(fixture.TryCreate(), XRErrors::ViewPlanInvalid);

            fixture.views[0].projection = {-1.0F, 1.0F, -1.0F, 1.0F};
            RequireFailureIdentity(fixture.TryCreate({}, 1), XRErrors::CapacityExceeded);
        }

        TEST_CASE("XR view plan rejects malformed admission configuration pose and extent evidence", "[unit][xr][views]") {
            PlanFixture fixture;

            SECTION("invalid admission mode") {
                RequireFailureIdentity(fixture.TryCreate({.mode = XRViewAdmissionMode::Count, .maximumViews = 2}),
                                       XRErrors::ViewPlanInvalid);
            }
            SECTION("zero admission capacity") {
                RequireFailureIdentity(fixture.TryCreate({.mode = XRViewAdmissionMode::PrimaryStereo, .maximumViews = 0}),
                                       XRErrors::ViewPlanInvalid);
            }
            SECTION("invalid configuration family") {
                fixture.configuration.type = XRViewConfigurationType::Count;
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::ViewPlanInvalid);
            }
            SECTION("incompatible contract version") {
                fixture.configuration.contractVersion.major += 1;
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::ContractVersionIncompatible);
            }
            SECTION("foreign view identity") {
                fixture.views[1].id.session = Session(6);
                fixture.targets[1].view = fixture.views[1].id;
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::IdentityStale);
            }
            SECTION("non-presentation pose") {
                fixture.views[0].pose.purpose = XRPosePurpose::SimulationInput;
                fixture.views[0].pose.time.renderPrediction.reset();
                fixture.views[0].pose.time.simulation = Horo::Tests::IdentityValue<XRSimulationTime>(1);
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::TimeDomainIncompatible);
            }
            SECTION("recommended extent exceeds maximum") {
                fixture.views[0].recommendedExtent.width = fixture.views[0].maximumExtent.width + 1;
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::ViewPlanInvalid);
            }
        }

        TEST_CASE("XR external targets preserve typed format usage extent and color depth roles", "[unit][xr][targets]") {
            PlanFixture fixture;
            fixture.targets[2] = {.view = fixture.views[0].id,
                                  .role = XRExternalRenderTargetRole::ProjectionDepth,
                                  .target = {fixture.session, {60, 1}},
                                  .format = Render::RenderTextureFormat::Depth32Float,
                                  .usage = Render::RenderTextureUsage::RenderAttachment,
                                  .allocationExtent = {2048, 2048},
                                  .renderRectangle = {16, 16, 1024, 1024},
                                  .sampleCount = 1};
            fixture.targets[2].image = {fixture.targets[2].target, {80, 1}};
            fixture.acquired[1] = fixture.targets[2].image;
            std::swap(fixture.targets[1], fixture.targets[2]);
            fixture.targetCount = 3;
            fixture.acquiredCount = 2;
            const auto plan = fixture.Create({.mode = XRViewAdmissionMode::PrimaryStereo, .maximumViews = 2, .supportsDepthTargets = true});
            REQUIRE(plan.Targets().size() == 3);
            CHECK(plan.Targets()[1].role == XRExternalRenderTargetRole::ProjectionDepth);

            fixture.targets[1].format = Render::RenderTextureFormat::Rgba16Float;
            RequireFailureIdentity(fixture.TryCreate(
                                       {.mode = XRViewAdmissionMode::PrimaryStereo, .maximumViews = 2, .supportsDepthTargets = true}),
                                   XRErrors::ExternalTargetInvalid);
            fixture.targets[1].format = Render::RenderTextureFormat::Depth32Float;
            fixture.targets[1].renderRectangle = {2040, 0, 16, 16};
            RequireFailureIdentity(fixture.TryCreate(
                                       {.mode = XRViewAdmissionMode::PrimaryStereo, .maximumViews = 2, .supportsDepthTargets = true}),
                                   XRErrors::ExternalTargetInvalid);
        }

        TEST_CASE("XR external targets reject malformed ownership and subresources", "[unit][xr][targets]") {
            PlanFixture fixture;

            SECTION("foreign target owner") {
                fixture.targets[0].target.session = Session(6);
                fixture.targets[0].image.target = fixture.targets[0].target;
                fixture.acquired[0] = fixture.targets[0].image;
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::IdentityStale);
            }
            SECTION("image belongs to another target") {
                fixture.targets[0].image.target.slot.index += 1;
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::ExternalTargetInvalid);
            }
            SECTION("usage lacks render attachment") {
                fixture.targets[0].usage = Render::RenderTextureUsage::Sampled;
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::ExternalTargetInvalid);
            }
            SECTION("usage has an unknown underlying bit") {
                fixture.targets[0].usage = static_cast<Render::RenderTextureUsage>(0x82U);
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::ExternalTargetInvalid);
            }
            SECTION("format is outside the closed Horo range") {
                fixture.targets[0].format = static_cast<Render::RenderTextureFormat>(255);
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::ExternalTargetInvalid);
            }
            SECTION("array layer is out of bounds") {
                fixture.targets[0].arrayLayer = fixture.targets[0].arrayLayerCount;
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::ExternalTargetInvalid);
            }
            SECTION("sample count is not a power of two") {
                fixture.targets[0].sampleCount = 3;
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::ExternalTargetInvalid);
            }
            SECTION("depth target is not admitted") {
                fixture.targets[0].role = XRExternalRenderTargetRole::ProjectionDepth;
                fixture.targets[0].format = Render::RenderTextureFormat::Depth32Float;
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::OperationUnsupported);
            }
        }

        TEST_CASE("XR external targets reject contradictory acquisition truth", "[unit][xr][targets]") {
            PlanFixture fixture;

            SECTION("one target exposes two acquired images") {
                fixture.targets[1].image.slot.index += 1;
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::ExternalTargetInvalid);
            }
            SECTION("two views alias one image layer") {
                fixture.targets[1].arrayLayer = fixture.targets[0].arrayLayer;
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::ExternalTargetInvalid);
            }
            SECTION("acquired image is invalid") {
                fixture.acquired[0] = {};
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::ExternalTargetInvalid);
            }
            SECTION("acquired image was replaced") {
                fixture.acquired[0].slot.generation += 1;
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::IdentityStale);
            }
            SECTION("unclaimed acquired image is rejected") {
                fixture.acquired[1] = {{fixture.session, {90, 1}}, {91, 1}};
                fixture.acquiredCount = 2;
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::ExternalTargetInvalid);
            }
        }

        TEST_CASE("XR external target validation rejects missing bindings and noncanonical order", "[unit][xr][targets]") {
            PlanFixture fixture;

            SECTION("target references an unlisted view") {
                fixture.targets[1].view.slot.index = 99;
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::ExternalTargetInvalid);
            }
            SECTION("view-major ordering is reversed") {
                std::swap(fixture.targets[0], fixture.targets[1]);
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::ExternalTargetInvalid);
            }
            SECTION("a view has no color binding") {
                fixture.targets[1].role = XRExternalRenderTargetRole::ProjectionDepth;
                fixture.targets[1].format = Render::RenderTextureFormat::Depth32Float;
                RequireFailureIdentity(fixture.TryCreate(
                                           {.mode = XRViewAdmissionMode::PrimaryStereo, .maximumViews = 2, .supportsDepthTargets = true}),
                                       XRErrors::ExternalTargetInvalid);
            }
        }

        TEST_CASE("XR acquired image validation rejects duplicate and missing current images", "[unit][xr][targets]") {
            PlanFixture fixture;

            SECTION("duplicate acquired image") {
                fixture.acquired[1] = fixture.acquired[0];
                fixture.acquiredCount = 2;
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::ExternalTargetInvalid);
            }
            SECTION("one current target image is missing") {
                fixture.targets[1].target.slot.index += 1;
                fixture.targets[1].image = {fixture.targets[1].target, {71, 1}};
                RequireFailureIdentity(fixture.TryCreate(), XRErrors::OperationUnavailable);
            }
        }

        TEST_CASE("XR plan revalidation fences session configuration image origin replacement and shutdown", "[unit][xr][lifecycle]") {
            PlanFixture fixture;
            const auto plan = fixture.Create();
            REQUIRE(ValidateXRViewRenderPlan(plan, fixture.session, fixture.configurationId, fixture.revision, fixture.origin,
                                             std::span{fixture.acquired}.first(fixture.acquiredCount))
                        .HasValue());
            RequireFailureIdentity(ValidateXRViewRenderPlan(plan, {}, fixture.configurationId, fixture.revision, fixture.origin,
                                                            std::span{fixture.acquired}.first(fixture.acquiredCount)),
                                   XRErrors::IdentityInvalid);
            RequireFailureIdentity(ValidateXRViewRenderPlan(plan, Session(6), fixture.configurationId, fixture.revision, fixture.origin,
                                                            std::span{fixture.acquired}.first(fixture.acquiredCount)),
                                   XRErrors::IdentityStale);
            const XRViewConfigurationId replacement{fixture.session, {fixture.configurationId.slot.index, 2}};
            RequireFailureIdentity(ValidateXRViewRenderPlan(plan, fixture.session, replacement, fixture.revision, fixture.origin,
                                                            std::span{fixture.acquired}.first(fixture.acquiredCount)),
                                   XRErrors::IdentityStale);
            RequireFailureIdentity(ValidateXRViewRenderPlan(plan, fixture.session, fixture.configurationId,
                                                            Horo::Tests::IdentityValue<XRViewConfigurationRevision>(8), fixture.origin,
                                                            std::span{fixture.acquired}.first(fixture.acquiredCount)),
                                   XRErrors::ViewConfigurationStale);
            RequireFailureIdentity(ValidateXRViewRenderPlan(plan, fixture.session, fixture.configurationId, fixture.revision,
                                                            Horo::Tests::IdentityValue<XRWorldOriginRevision>(10),
                                                            std::span{fixture.acquired}.first(fixture.acquiredCount)),
                                   XRErrors::OriginRevisionStale);

            auto replacedImages = fixture.acquired;
            replacedImages[0].slot.generation = 2;
            RequireFailureIdentity(ValidateXRViewRenderPlan(plan, fixture.session, fixture.configurationId, fixture.revision,
                                                            fixture.origin, std::span{replacedImages}.first(fixture.acquiredCount)),
                                   XRErrors::IdentityStale);
            RequireFailureIdentity(ValidateXRViewRenderPlan(plan, fixture.session, fixture.configurationId, fixture.revision,
                                                            fixture.origin, std::span<const XRSwapchainImageId>{}),
                                   XRErrors::OperationUnavailable);
        }

        /** @brief Exact per-view reference-to-world evidence, without an implicit local/stage-to-world conversion. */
        [[nodiscard]] XRPoseSample OriginSample(const PlanFixture &fixture, const std::size_t view, const float x = 3.0F) {
            auto descriptor = fixture.views[view].pose;
            descriptor.source = descriptor.target;
            descriptor.target = Space(fixture.session, XRSpaceKind::World, 99);
            descriptor.components.positionMeters.value = Math::Vec3{x, 0.0F, 0.0F};
            auto sample = XRPoseSample::Create(descriptor, fixture.session, fixture.origin);
            REQUIRE(sample.HasValue());
            return sample.Value();
        }

        [[nodiscard]] XRMultiviewOwnerState MultiviewOwner(const PlanFixture &fixture) {
            return {.session = fixture.session,
                    .configuration = fixture.configurationId,
                    .configurationRevision = fixture.revision,
                    .originRevision = fixture.origin,
                    .acquiredImages = std::span{fixture.acquired}.first(fixture.acquiredCount),
                    .renderer = {1, 2, fixture.origin.Value(), fixture.revision.Value(), 3, 4},
                    .capabilities = {XRHardLimits::MaximumViews, true, true, false}};
        }

        TEST_CASE("Quad extraction keeps all projections and requires exact layer routing for instancing", "[unit][xr][multiview]") {
            using enum Render::MultiviewExecutionPolicy;
            PlanFixture fixture{4};
            const XRViewAdmissionPolicy admission{.mode = XRViewAdmissionMode::BoundedNView, .maximumViews = 4};
            fixture.views[2].projection = {-0.3F, 0.4F, -0.2F, 0.2F};
            fixture.views[3].projection = {-0.4F, 0.3F, -0.2F, 0.2F};
            auto active = MultiviewOwner(fixture);
            active.capabilities.instancedTargetRouting = true;
            const std::array origins{OriginSample(fixture, 0), OriginSample(fixture, 1), OriginSample(fixture, 2),
                                     OriginSample(fixture, 3)};
            const auto extract = [&](const Render::MultiviewExecutionPolicy policy) {
                const auto plan = fixture.Create(admission);
                return XRMultiviewScene::Create({.plan = plan,
                                                 .referenceToWorld = origins,
                                                 .publication = active.renderer,
                                                 .policy = policy,
                                                 .limits = {.maximumViews = 4}},
                                                active);
            };

            SECTION("Canonical layers admit indexed instancing") {
                const auto scene = extract(RequireInstanced);
                REQUIRE(scene.HasValue());
                CHECK(scene.Value().Scene().Mode() == Render::MultiviewExecutionMode::Instanced);
                REQUIRE(scene.Value().Scene().Views().size() == 4);
                CHECK(scene.Value().Scene().Views()[0].worldToClip != scene.Value().Scene().Views()[2].worldToClip);
            }
            SECTION("Permuted layers remain an exact multipass plan") {
                std::swap(fixture.targets[0].arrayLayer, fixture.targets[2].arrayLayer);
                const auto scene = extract(PreferInstanced);
                REQUIRE(scene.HasValue());
                CHECK(scene.Value().Scene().Mode() == Render::MultiviewExecutionMode::Multipass);
                REQUIRE(scene.Value().Scene().Views().size() == 4);
                CHECK(scene.Value().Plan().Targets()[0].arrayLayer == 2);
                RequireFailureIdentity(extract(RequireInstanced), Render::MultiviewExtractionErrors::Unsupported);
            }
            SECTION("An independent inset image can be replaced without aliasing its old plan") {
                for (std::size_t index = 0; index < fixture.viewCount; ++index) {
                    auto &target = fixture.targets[index];
                    target.target.slot.index += static_cast<std::uint32_t>(index);
                    target.image.target = target.target;
                    target.arrayLayer = 0;
                    target.arrayLayerCount = 1;
                    fixture.acquired[index] = target.image;
                }
                fixture.targets[2].renderRectangle = {0, 0, 512, 512};
                fixture.acquiredCount = fixture.viewCount;
                active.acquiredImages = std::span{fixture.acquired}.first(fixture.acquiredCount);
                const auto scene = extract(PreferInstanced);
                REQUIRE(scene.HasValue());
                CHECK(scene.Value().Scene().Mode() == Render::MultiviewExecutionMode::Multipass);
                CHECK(scene.Value().Plan().Targets()[2].renderRectangle.width == 512);
                CHECK(scene.Value().Validate(active).HasValue());
                ++fixture.acquired[2].slot.generation;
                RequireFailureIdentity(scene.Value().Validate(active), XRErrors::IdentityStale);
            }
            SECTION("Stereo-only hardware rejects all four views") {
                active.capabilities.maximumViews = 2;
                RequireFailureIdentity(extract(PreferInstanced), Render::MultiviewExtractionErrors::Unsupported);
            }
        }

        TEST_CASE("XR World canvas uses each live eye rectangle and one shared readable plane", "[unit][xr][multiview][world-canvas]") {
            PlanFixture fixture;
            fixture.targets[1].renderRectangle = {21, 13, 512, 640};
            fixture.views[1].pose.components.positionMeters.value = Math::Vec3{0.064F, 1.7F, 0};
            const auto plan = fixture.Create();
            auto active = MultiviewOwner(fixture);
            const std::array origins{OriginSample(fixture, 0), OriginSample(fixture, 1)};
            const auto scene =
                XRMultiviewScene::Create({.plan = plan, .referenceToWorld = origins, .publication = active.renderer}, active);
            REQUIRE(scene.HasValue());

            XRWorldCanvasDescriptor canvas{Math::Mat4::Identity(), 120};
            canvas.worldFromCanvas.values[12] = 3;
            canvas.worldFromCanvas.values[13] = 1.7F;
            canvas.worldFromCanvas.values[14] = -2;
            const auto allocationsBefore = Tests::AllocationProbe::Count();
            const auto projected = scene.Value().ProjectWorldCanvas(canvas, active);
            const auto allocationsAfter = Tests::AllocationProbe::Count();
            REQUIRE(projected.HasValue());
            CHECK(allocationsBefore == allocationsAfter);
            const auto &stereo = projected.Value();
            REQUIRE(stereo.drawable);
            CHECK(stereo.publication == active.renderer);
            CHECK(stereo.worldFromCanvas.values == canvas.worldFromCanvas.values);
            CHECK(stereo.views[0].view == fixture.views[0].id);
            CHECK(stereo.views[1].view == fixture.views[1].id);
            CHECK(stereo.views[0].arrayLayer == 0);
            CHECK(stereo.views[1].arrayLayer == 1);
            CHECK(stereo.views[1].renderRectangle == fixture.targets[1].renderRectangle);
            CHECK(Math::NearlyEqual(stereo.views[0].projectedSpanPixels, 256.0F));
            CHECK(Math::NearlyEqual(stereo.views[1].projectedSpanPixels, 128.0F));
            CHECK(!Math::NearlyEqual(stereo.views[0].canvasToClip.At(0, 3), stereo.views[1].canvasToClip.At(0, 3)));

            canvas.minimumSpanPixels = 129;
            const auto unreadable = scene.Value().ProjectWorldCanvas(canvas, active);
            REQUIRE(unreadable.HasValue());
            CHECK_FALSE(unreadable.Value().drawable);

            canvas.minimumSpanPixels = 24;
            canvas.worldFromCanvas.values[12] = 5.45F;
            const auto beforeClip = Tests::AllocationProbe::Count();
            const auto sliver = scene.Value().ProjectWorldCanvas(canvas, active);
            const auto afterClip = Tests::AllocationProbe::Count();
            REQUIRE(sliver.HasValue());
            CHECK(afterClip == beforeClip);
            CHECK_FALSE(sliver.Value().drawable);
            for (const auto &eye : sliver.Value().views) {
                CHECK(eye.projectedSpanPixels > 10);
                CHECK(eye.projectedSpanPixels < 24);
            }
            canvas.minimumSpanPixels = 10;
            const auto partiallyVisible = scene.Value().ProjectWorldCanvas(canvas, active);
            REQUIRE(partiallyVisible.HasValue());
            CHECK(partiallyVisible.Value().drawable);

            const auto overfill = Math::Transform{.translation = {3, 1.7F, -2},
                                                  .rotation = Math::Quaternion::FromEulerRadians({0, 0, Math::Pi / 4}),
                                                  .scale = {4, 4, 1}}
                                      .TryToMatrix();
            REQUIRE(overfill.HasValue());
            canvas.worldFromCanvas = overfill.Value();
            const auto clippedCorners = scene.Value().ProjectWorldCanvas(canvas, active);
            REQUIRE(clippedCorners.HasValue());
            CHECK(clippedCorners.Value().drawable);
            for (const auto &eye : clippedCorners.Value().views)
                CHECK(eye.projectedSpanPixels <= static_cast<float>(std::min(eye.renderRectangle.width, eye.renderRectangle.height)));

            canvas.minimumSpanPixels = 1;
            canvas.worldFromCanvas = stereo.worldFromCanvas;
            canvas.worldFromCanvas.values[14] = -0.05F;
            const auto tooClose = scene.Value().ProjectWorldCanvas(canvas, active);
            REQUIRE(tooClose.HasValue());
            CHECK_FALSE(tooClose.Value().drawable);
            canvas.worldFromCanvas.values[14] = -2;
            canvas.worldFromCanvas.values[12] = 40;
            const auto offscreen = scene.Value().ProjectWorldCanvas(canvas, active);
            REQUIRE(offscreen.HasValue());
            CHECK_FALSE(offscreen.Value().drawable);

            const auto diagonal = Math::Transform{.translation = {5.12F, 3.82F, -2},
                                                  .rotation = Math::Quaternion::FromEulerRadians({0, 0, Math::Pi / 4}),
                                                  .scale = {0.3F, 0.3F, 1}}
                                      .TryToMatrix();
            REQUIRE(diagonal.HasValue());
            canvas.worldFromCanvas = diagonal.Value();
            const auto beyondCorner = scene.Value().ProjectWorldCanvas(canvas, active);
            REQUIRE(beyondCorner.HasValue());
            CHECK_FALSE(beyondCorner.Value().drawable);

            const auto edgeOn =
                Math::Transform{.translation = {3, 1.7F, -2}, .rotation = Math::Quaternion::FromEulerRadians({0, Math::Pi / 2, 0})}
                    .TryToMatrix();
            REQUIRE(edgeOn.HasValue());
            canvas.worldFromCanvas = edgeOn.Value();
            const auto thin = scene.Value().ProjectWorldCanvas(canvas, active);
            REQUIRE(thin.HasValue());
            CHECK_FALSE(thin.Value().drawable);

            canvas.minimumSpanPixels = 0;
            RequireFailureIdentity(scene.Value().ProjectWorldCanvas(canvas, active), XRErrors::ViewPlanInvalid);
            canvas.minimumSpanPixels = 1;
            active.originRevision = Horo::Tests::IdentityValue<XRWorldOriginRevision>(10);
            RequireFailureIdentity(scene.Value().ProjectWorldCanvas(canvas, active), XRErrors::OriginRevisionStale);
        }

        TEST_CASE("XR multiview converts actual asymmetric projections and declared reference poses", "[unit][xr][multiview]") {
            PlanFixture fixture;
            fixture.views[0].projection = {-0.5F, 1.5F, -1.0F, 1.0F};
            const auto plan = fixture.Create();
            const auto active = MultiviewOwner(fixture);
            const std::array origins{OriginSample(fixture, 0), OriginSample(fixture, 1)};
            const XRMultiviewSceneInput input{.plan = plan, .referenceToWorld = origins, .publication = active.renderer};
            const auto result = XRMultiviewScene::Create(input, active);
            REQUIRE(result.HasValue());
            CHECK(result.Value().Plan().Targets()[0].image == fixture.acquired[0]);
            CHECK(result.Value().Scene().Mode() == Render::MultiviewExecutionMode::Multipass);
            REQUIRE(result.Value().Scene().Views().size() == 2);
            const auto &clip = result.Value().Scene().Views()[0].worldToClip;
            CHECK(Math::NearlyEqual(clip.At(0, 2), 0.5F));
            CHECK(Math::NearlyEqual(clip.At(0, 3), -3.0F));
            const auto nearPoint = Math::TryTransformPoint(clip, {3.0F, 1.7F, -input.nearPlane});
            const auto farPoint = Math::TryTransformPoint(clip, {3.0F, 1.7F, -input.farPlane});
            REQUIRE(nearPoint.HasValue());
            REQUIRE(farPoint.HasValue());
            CHECK(Math::NearlyEqual(nearPoint.Value().z, 0.0F));
            CHECK(Math::NearlyEqual(farPoint.Value().z, 1.0F));
            CHECK(result.Value().Validate(active).HasValue());
            auto lost = active;
            lost.acquiredImages = {};
            RequireFailureIdentity(result.Value().Validate(lost), XRErrors::OperationUnavailable);
            lost = active;
            lost.session.slot.generation++;
            RequireFailureIdentity(result.Value().Validate(lost), XRErrors::IdentityStale);
        }

        TEST_CASE("XR multiview conversion rejects mismatched origin time and invalid projection atomically", "[unit][xr][multiview]") {
            PlanFixture fixture;
            const auto plan = fixture.Create();
            const auto active = MultiviewOwner(fixture);
            std::array origins{OriginSample(fixture, 0), OriginSample(fixture, 1)};
            XRMultiviewSceneInput input{.plan = plan, .referenceToWorld = origins, .publication = active.renderer};
            SECTION("Missing explicit origin conversion") {
                input.referenceToWorld = {};
                RequireFailureIdentity(XRMultiviewScene::Create(input, active), XRErrors::CoordinateSpaceIncompatible);
            }
            SECTION("World origin revision differs") {
                ++input.publication.originRevision;
                RequireFailureIdentity(XRMultiviewScene::Create(input, active), XRErrors::OriginRevisionStale);
            }
            SECTION("Wrong time evidence") {
                auto descriptor = fixture.views[1].pose;
                descriptor.source = descriptor.target;
                descriptor.target = Space(fixture.session, XRSpaceKind::World, 99);
                descriptor.time.runtimeSample = Horo::Tests::IdentityValue<XRRuntimeTime>(91);
                auto replacement = XRPoseSample::Create(descriptor, fixture.session, fixture.origin);
                REQUIRE(replacement.HasValue());
                origins[1] = replacement.Value();
                RequireFailureIdentity(XRMultiviewScene::Create(input, active), XRErrors::TimeDomainIncompatible);
            }
            SECTION("Invalid finite projection interval") {
                input.farPlane = input.nearPlane;
                RequireFailureIdentity(XRMultiviewScene::Create(input, active), XRErrors::ViewPlanInvalid);
            }
        }

        TEST_CASE("XR multiview never treats an Action target as World origin conversion", "[unit][xr][multiview]") {
            PlanFixture fixture;
            const auto plan = fixture.Create();
            const auto active = MultiviewOwner(fixture);
            auto descriptor = fixture.views[0].pose;
            descriptor.source = descriptor.target;
            descriptor.target = Space(fixture.session, XRSpaceKind::Action, 99);
            const auto action = XRPoseSample::Create(descriptor, fixture.session, fixture.origin);
            REQUIRE(action.HasValue());
            const std::array origins{action.Value(), OriginSample(fixture, 1)};
            const XRMultiviewSceneInput input{.plan = plan, .referenceToWorld = origins, .publication = active.renderer};
            RequireFailureIdentity(XRMultiviewScene::Create(input, active), XRErrors::CoordinateSpaceIncompatible);
        }

        TEST_CASE("XR multiview conversion retains immutable values without heap allocation", "[unit][xr][multiview][allocation]") {
            PlanFixture fixture;
            const auto plan = fixture.Create();
            const auto active = MultiviewOwner(fixture);
            const std::array origins{OriginSample(fixture, 0), OriginSample(fixture, 1)};
            const XRMultiviewSceneInput input{.plan = plan, .referenceToWorld = origins, .publication = active.renderer};
            const auto before = Tests::AllocationProbe::Count();
            const auto result = XRMultiviewScene::Create(input, active);
            const auto after = Tests::AllocationProbe::Count();
            REQUIRE(result.HasValue());
            CHECK(after == before);
            fixture.views[0].pose.components.positionMeters.value = Math::Vec3{99.0F, 0.0F, 0.0F};
            CHECK(result.Value().Scene().Views()[0].worldToClip.At(0, 3) == -3.0F);
        }

        TEST_CASE("XR view plan construction and hot revalidation allocate no heap storage", "[unit][xr][allocation]") {
            PlanFixture fixture;
            const auto beforeCreate = Tests::AllocationProbe::Count();
            auto result = fixture.TryCreate();
            const auto afterCreate = Tests::AllocationProbe::Count();
            REQUIRE(result.HasValue());
            CHECK(afterCreate == beforeCreate);

            const auto beforeValidate = Tests::AllocationProbe::Count();
            const auto validation = ValidateXRViewRenderPlan(result.Value(), fixture.session, fixture.configurationId, fixture.revision,
                                                             fixture.origin, std::span{fixture.acquired}.first(fixture.acquiredCount));
            const auto afterValidate = Tests::AllocationProbe::Count();
            REQUIRE(validation.HasValue());
            CHECK(afterValidate == beforeValidate);
        }
    }  // namespace
}  // namespace Horo::XR
