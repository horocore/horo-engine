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
