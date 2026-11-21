#include "../physics/CharacterWorldTestHelpers.h"
#include "Horo/XR/XRErrors.h"
#include "Horo/XR/XRLocomotionPolicy.h"
#include "support/TypedIdentityTestSupport.h"

#include <catch2/catch_approx.hpp>
#include <limits>
#include <memory>
#include <numbers>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::XR;
    using Character::TestDetail::RequireError;
    using Tests::IdentityValue;

    constexpr Input::PlayerId Player = 3;
    constexpr std::uint64_t Assignment = 7;
    constexpr std::int64_t Step = 20'000'000;
    constexpr std::int64_t MaxGap = 100'000'000;

    XRSessionId TestSession() {
        return {{IdentityValue<XRRuntimeGeneration>(1), {2, 1}}, {3, 1}};
    }

    std::unique_ptr<XRTrackingSnapshot> Tracking(std::uint32_t revision, std::int64_t sampledAt,
                                                 XRTrackingFocusState focus = XRTrackingFocusState::Focused,
                                                 std::uint32_t originValue = 1) {
        const auto origin = IdentityValue<XRWorldOriginRevision>(originValue);
        const XRTrackingSnapshotDescriptor descriptor{
            .session = TestSession(),
            .revision = IdentityValue<XRTrackingSnapshotRevision>(revision),
            .sequence = revision,
            .sampledAt = IdentityValue<XRRuntimeTime>(sampledAt),
            .worldOriginRevision = origin,
            .focus = focus,
            .visibility = XRTrackingVisibilityState::Visible,
            .state = XRTrackingState::Active,
            .limits = {.maximumDevices = 1, .maximumPoses = 1},
        };
        auto snapshot = XRTrackingSnapshot::Create(descriptor, descriptor.session, origin);
        REQUIRE(snapshot.HasValue());
        return std::make_unique<XRTrackingSnapshot>(std::move(snapshot).Value());
    }

    XRLocomotionSample Sample(const XRTrackingSnapshot &tracking, std::uint64_t tick) {
        return {.scope = {DescribeXRInteractionCutoff(tracking), Player, Assignment},
                .tick = tick,
                .sequence = tick,
                .fixedStepNanoseconds = Step};
    }

    XRLocomotionPolicy Policy(XRLocomotionProfile profile, std::weak_ptr<const IXRLocomotionDestinationOwner> destination = {}) {
        auto policy = XRLocomotionPolicy::Create(std::move(profile), TestSession(), Player, Assignment, std::move(destination));
        REQUIRE(policy.HasValue());
        return std::move(policy).Value();
    }

    XRLocomotionMotion Motion(XRLocomotionPolicy &policy, const XRLocomotionSample &sample, const XRTrackingSnapshot &tracking) {
        auto evaluated = policy.Evaluate(sample, tracking);
        REQUIRE(evaluated.HasValue());
        REQUIRE(evaluated.Value());
        return std::get<XRLocomotionMotion>(evaluated.Value()->operation);
    }

    struct DestinationOwner final : IXRLocomotionDestinationOwner {
        enum class Response : std::uint8_t {
            Valid,
            Busy,
            Outside,
            Foreign
        };
        Response response{Response::Valid};
        mutable std::uint32_t observed{};

        Result<std::optional<XRLocomotionTeleport>> Resolve(const XRLocomotionSample &sample) const override {
            using Outcome = Result<std::optional<XRLocomotionTeleport>>;
            using enum Response;
            ++observed;
            if (response == Busy)
                return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
            if (response == Outside)
                return Outcome::Success({});
            const auto &cutoff = sample.scope.tracking;
            const auto origin = response == Foreign ? IdentityValue<XRWorldOriginRevision>(99) : cutoff.origin;
            const XRCoordinateSpace world{.id = {cutoff.session, {4, 1}}, .kind = XRSpaceKind::World, .worldOriginRevision = origin};
            return Outcome::Success(XRLocomotionTeleport{world, 17, {2, 0, 4}});
        }
    };

    TEST_CASE("XR smooth movement bounds diagonal speed and snap turns once per release", "[xr][locomotion]") {
        auto policy = Policy({XRSmoothMove{3.0F, 0.25F}, XRSnapTurn{0.5F, 0.8F, 0.3F}});
        const auto tracking = Tracking(1, Step);
        auto sample = Sample(*tracking, 1);
        REQUIRE(Motion(policy, sample, *tracking).velocityMetersPerSecond == Math::Vec3{});
        sample = Sample(*tracking, 2);
        sample.move = {1, 1};
        sample.turn = 1;
        sample.gameplayYawRadians = 0.75F;
        const auto motion = Motion(policy, sample, *tracking);
        REQUIRE(Math::Length(*motion.velocityMetersPerSecond) == Catch::Approx(3.0F));
        REQUIRE(motion.heading == Math::Quaternion::FromAxisAngle({0, 1, 0}, 1.25F));
        ++sample.tick;
        ++sample.sequence;
        REQUIRE_FALSE(Motion(policy, sample, *tracking).heading);
        sample = Sample(*tracking, 4);
        sample.move.x = 0.25F;
        REQUIRE(Motion(policy, sample, *tracking).velocityMetersPerSecond == Math::Vec3{});
        sample = Sample(*tracking, 5);
        sample.turn = -1;
        REQUIRE(Motion(policy, sample, *tracking).heading == Math::Quaternion::FromAxisAngle({0, 1, 0}, -0.5F));
    }

    TEST_CASE("XR focus loss and policy replacement rearm only after neutral input", "[xr][locomotion]") {
        auto policy = Policy({XRSmoothMove{3.0F, 0.0F}, XRSnapTurn{0.5F, 0.8F, 0.3F}});
        const auto focused = Tracking(2, Step * 2);
        const auto unfocused = Tracking(1, Step, XRTrackingFocusState::Unfocused);
        REQUIRE_FALSE(policy.Evaluate(Sample(*unfocused, 1), *unfocused).Value());
        auto held = Sample(*focused, 2);
        held.turn = 1;
        held.move.y = 1;
        REQUIRE(Motion(policy, held, *focused).velocityMetersPerSecond == Math::Vec3{});
        ++held.tick;
        ++held.sequence;
        REQUIRE_FALSE(Motion(policy, held, *focused).heading);
        RequireError(policy.SetProfile({XRSmoothMove{1.0F, 1.0F}}), XRErrors::OperationInvalid);
        ++held.tick;
        ++held.sequence;
        REQUIRE(Motion(policy, held, *focused).velocityMetersPerSecond == Math::Vec3{0, 0, -3});
        REQUIRE(policy.SetProfile({XRNoTranslation{}}).HasValue());
        ++held.tick;
        ++held.sequence;
        REQUIRE(Motion(policy, held, *focused).velocityMetersPerSecond == Math::Vec3{});
        ++held.tick;
        ++held.sequence;
        REQUIRE_FALSE(policy.Evaluate(held, *focused).Value());
        policy.Shutdown();
        RequireError(policy.Evaluate(held, *focused), XRErrors::OperationUnavailable);
    }

    TEST_CASE("XR comfort follows accepted motion and reduced motion preserves snap turning", "[xr][locomotion][comfort]") {
        const XRLocomotionProfile normal{.translation = XRSmoothMove{4.0F, 0.0F},
                                         .turn = XRSnapTurn{0.5F, 0.8F, 0.3F},
                                         .comfort = {.maximumVignetteOpacity = 0.75F}};
        auto policy = Policy(normal);
        const auto tracking = Tracking(1, Step);
        const auto neutral = policy.EvaluateFrame(Sample(*tracking, 1), *tracking);
        REQUIRE(neutral.HasValue());
        CHECK(neutral.Value().vignetteOpacity == 0.0F);
        auto move = Sample(*tracking, 2);
        move.move.y = 0.5F;
        auto frame = policy.EvaluateFrame(move, *tracking);
        REQUIRE(frame.HasValue());
        REQUIRE(frame.Value().intent);
        CHECK(frame.Value().vignetteOpacity == Catch::Approx(0.375F));
        CHECK(std::get<XRLocomotionMotion>(frame.Value().intent->operation).velocityMetersPerSecond == Math::Vec3{0, 0, -2});

        auto invalid = normal;
        invalid.comfort.maximumVignetteOpacity = std::numeric_limits<float>::infinity();
        RequireError(policy.SetProfile(invalid), XRErrors::OperationInvalid);
        move.tick = move.sequence = 3;
        frame = policy.EvaluateFrame(move, *tracking);
        REQUIRE(frame.HasValue());
        CHECK(frame.Value().vignetteOpacity == Catch::Approx(0.375F));

        auto reduced = normal;
        reduced.comfort.reducedMotion = true;
        REQUIRE(policy.SetProfile(reduced).HasValue());
        frame = policy.EvaluateFrame(Sample(*tracking, 4), *tracking);
        REQUIRE(frame.HasValue());
        CHECK(frame.Value().vignetteOpacity == 0.0F);
        move.tick = move.sequence = 5;
        move.turn = 1.0F;
        frame = policy.EvaluateFrame(move, *tracking);
        REQUIRE(frame.HasValue());
        REQUIRE(frame.Value().intent);
        const auto &snap = std::get<XRLocomotionMotion>(frame.Value().intent->operation);
        CHECK_FALSE(snap.velocityMetersPerSecond);
        CHECK(snap.heading == Math::Quaternion::FromAxisAngle({0, 1, 0}, 0.5F));
        CHECK(frame.Value().vignetteOpacity == 0.0F);

        const auto unfocused = Tracking(2, Step * 2, XRTrackingFocusState::Unfocused);
        frame = policy.EvaluateFrame(Sample(*unfocused, 6), *unfocused);
        REQUIRE(frame.HasValue());
        CHECK_FALSE(frame.Value().intent);
        CHECK(frame.Value().vignetteOpacity == 0.0F);
        policy.Shutdown();
        RequireError(policy.EvaluateFrame(Sample(*tracking, 7), *tracking), XRErrors::OperationUnavailable);
    }

    TEST_CASE("XR comfort bounds are selected once without increasing authored travel", "[xr][locomotion][comfort]") {
        const XRLocomotionProfile authored{.translation = XRSmoothMove{4.0F, 0.0F},
                                           .turn = XRSnapTurn{0.5F, 0.8F, 0.3F},
                                           .comfort = {.maximumVignetteOpacity = 0.6F,
                                                       .artificialSpeedCapMetersPerSecond = 2.0F,
                                                       .snapTurnAngleRadians = 1.0F}};
        auto policy = Policy(authored);
        const auto tracking = Tracking(1, Step);
        REQUIRE(policy.EvaluateFrame(Sample(*tracking, 1), *tracking).HasValue());
        auto sample = Sample(*tracking, 2);
        sample.move.y = sample.turn = 1.0F;
        const auto frame = policy.EvaluateFrame(sample, *tracking);
        REQUIRE(frame.HasValue());
        REQUIRE(frame.Value().intent);
        const auto &motion = std::get<XRLocomotionMotion>(frame.Value().intent->operation);
        CHECK(motion.velocityMetersPerSecond == Math::Vec3{0, 0, -2});
        CHECK(motion.heading == Math::Quaternion::FromAxisAngle({0, 1, 0}, 1.0F));
        CHECK(frame.Value().vignetteOpacity == Catch::Approx(0.6F));

        auto replacement = authored;
        replacement.comfort.artificialSpeedCapMetersPerSecond = 6.0F;
        REQUIRE(policy.SetProfile(replacement).HasValue());
        REQUIRE(policy.EvaluateFrame(Sample(*tracking, 3), *tracking).HasValue());
        sample.tick = sample.sequence = 4;
        CHECK(Motion(policy, sample, *tracking).velocityMetersPerSecond == Math::Vec3{0, 0, -4});
        CHECK(std::get<XRSmoothMove>(authored.translation).maximumSpeed == 4.0F);
        replacement.comfort.snapTurnAngleRadians = -0.1F;
        RequireError(policy.SetProfile(replacement), XRErrors::OperationInvalid);
    }

    TEST_CASE("XR reduced motion keeps physical correction and a configured teleport alternative", "[xr][locomotion][comfort]") {
        auto owner = std::make_shared<DestinationOwner>();
        auto policy = Policy({.translation = XRRoomScaleMove{4.0F, 0.5F},
                              .maximumTrackingGapNanoseconds = MaxGap,
                              .comfort = {.reducedMotion = true, .maximumVignetteOpacity = 0.7F, .teleportWhenReduced = true}},
                             owner);
        const auto initial = Tracking(1, Step);
        auto sample = Sample(*initial, 1);
        sample.head = Math::Vec3{};
        REQUIRE(policy.EvaluateFrame(sample, *initial).HasValue());

        const auto tracking = Tracking(2, Step * 2);
        sample = Sample(*tracking, 2);
        sample.head = Math::Vec3{0.04F, 0.0F, 0.0F};
        auto frame = policy.EvaluateFrame(sample, *tracking);
        REQUIRE(frame.HasValue());
        REQUIRE(frame.Value().intent);
        REQUIRE(std::get<XRLocomotionMotion>(frame.Value().intent->operation).velocityMetersPerSecond);
        CHECK(std::get<XRLocomotionMotion>(frame.Value().intent->operation).velocityMetersPerSecond->x == Catch::Approx(2.0F));
        CHECK(frame.Value().vignetteOpacity == 0.0F);

        sample.tick = sample.sequence = 3;
        sample.teleportPressed = true;
        frame = policy.EvaluateFrame(sample, *tracking);
        REQUIRE(frame.HasValue());
        REQUIRE(frame.Value().intent);
        CHECK(std::holds_alternative<XRLocomotionTeleport>(frame.Value().intent->operation));
        CHECK(frame.Value().vignetteOpacity == 0.0F);
        CHECK(owner->observed == 1);
    }

    TEST_CASE("Unavailable optional reduced-motion teleport leaves snap turning available", "[xr][locomotion][comfort]") {
        auto policy = Policy({.translation = XRSmoothMove{4.0F, 0.0F},
                              .turn = XRSnapTurn{0.5F, 0.8F, 0.3F},
                              .comfort = {.reducedMotion = true, .maximumVignetteOpacity = 0.7F, .teleportWhenReduced = true}});
        const auto tracking = Tracking(1, Step);
        REQUIRE(policy.EvaluateFrame(Sample(*tracking, 1), *tracking).HasValue());
        auto sample = Sample(*tracking, 2);
        sample.move = {1, 1};
        sample.turn = 1;
        sample.teleportPressed = true;
        const auto result = policy.EvaluateFrame(sample, *tracking);
        REQUIRE(result.HasValue());
        const auto &frame = result.Value();
        REQUIRE(frame.intent);
        const auto &motion = std::get<XRLocomotionMotion>(frame.intent->operation);
        CHECK(motion.heading);
        CHECK_FALSE(motion.velocityMetersPerSecond);
        CHECK(frame.vignetteOpacity == 0.0F);
    }

    TEST_CASE("XR room scale submits a fresh horizontal displacement only once per tracking sample", "[xr][locomotion]") {
        auto policy = Policy({XRRoomScaleMove{4.0F, 0.5F}, {}, MaxGap});
        const auto first = Tracking(1, Step);
        const auto next = Tracking(2, Step * 2);
        auto sample = Sample(*first, 1);
        sample.head = Math::Vec3{0, 2, 0};
        REQUIRE(Motion(policy, sample, *first).velocityMetersPerSecond == Math::Vec3{});
        sample = Sample(*next, 2);
        sample.head = Math::Vec3{0.02F, 4, 0};
        const auto velocity = Motion(policy, sample, *next).velocityMetersPerSecond.value();
        REQUIRE(velocity.x == Catch::Approx(1.0F));
        REQUIRE(velocity.y == 0);
        ++sample.tick;
        ++sample.sequence;
        REQUIRE(Motion(policy, sample, *next).velocityMetersPerSecond == Math::Vec3{});
        const auto rebased = Tracking(3, Step * 3, XRTrackingFocusState::Focused, 2);
        sample = Sample(*rebased, 4);
        sample.head = Math::Vec3{50, 0, 0};
        REQUIRE(Motion(policy, sample, *rebased).velocityMetersPerSecond == Math::Vec3{});
    }

    TEST_CASE("XR room-scale motion honors gameplay rotation and renews the baseline on head replacement", "[xr][locomotion]") {
        auto policy = Policy({XRRoomScaleMove{2.0F, 0.4F}, {}, MaxGap});
        const auto first = Tracking(1, Step);
        auto sample = Sample(*first, 1);
        sample.head = Math::Vec3{};
        sample.headSource = XRDeviceId{TestSession(), {7, 1}};
        sample.trackingToGameplayYawRadians = std::numbers::pi_v<float> * 0.5F;
        CHECK(Motion(policy, sample, *first).velocityMetersPerSecond == Math::Vec3{});

        const auto moved = Tracking(2, Step * 2);
        sample.tick = sample.sequence = 2;
        sample.scope.tracking = DescribeXRInteractionCutoff(*moved);
        sample.head = Math::Vec3{0, 0, -0.05F};
        const auto velocity = Motion(policy, sample, *moved).velocityMetersPerSecond;
        REQUIRE(velocity);
        CHECK(velocity->x == Catch::Approx(-2.0F));
        CHECK(velocity->z == Catch::Approx(0.0F).margin(0.00001F));

        const auto replaced = Tracking(3, Step * 3);
        sample.tick = sample.sequence = 3;
        sample.scope.tracking = DescribeXRInteractionCutoff(*replaced);
        sample.head = Math::Vec3{0, 0, -0.1F};
        sample.headSource = XRDeviceId{TestSession(), {7, 2}};
        CHECK(Motion(policy, sample, *replaced).velocityMetersPerSecond == Math::Vec3{});
    }

    TEST_CASE("XR arm swing requires paired tracked hands and a bounded fresh interval", "[xr][locomotion]") {
        auto policy = Policy({XRArmSwingMove{2.0F, 2.0F}, {}, MaxGap});
        const auto first = Tracking(1, Step);
        auto sample = Sample(*first, 1);
        sample.leftHand = Math::Vec3{};
        sample.rightHand = Math::Vec3{};
        REQUIRE(Motion(policy, sample, *first).velocityMetersPerSecond == Math::Vec3{});
        const auto next = Tracking(2, Step * 2);
        sample = Sample(*next, 2);
        sample.leftHand = Math::Vec3{0, 0, 0.05F};
        sample.rightHand = Math::Vec3{0, 0, -0.05F};
        REQUIRE(Motion(policy, sample, *next).velocityMetersPerSecond == Math::Vec3{0, 0, -2});
        const auto late = Tracking(3, Step * 2 + MaxGap + 1);
        ++sample.tick;
        ++sample.sequence;
        sample.scope.tracking = DescribeXRInteractionCutoff(*late);
        std::swap(sample.leftHand, sample.rightHand);
        REQUIRE(Motion(policy, sample, *late).velocityMetersPerSecond == Math::Vec3{});
    }

    TEST_CASE("XR arm swing follows rotated gameplay heading without consuming motion across controller replacement", "[xr][locomotion]") {
        auto policy = Policy({XRArmSwingMove{2.0F, 2.0F}, {}, MaxGap});
        const auto first = Tracking(1, Step);
        auto sample = Sample(*first, 1);
        sample.gameplayYawRadians = std::numbers::pi_v<float> * 0.5F;
        sample.trackingYawRadians = 0.0F;
        sample.leftHand = Math::Vec3{};
        sample.rightHand = Math::Vec3{};
        sample.handSources = std::array{XRDeviceId{TestSession(), {8, 1}}, XRDeviceId{TestSession(), {9, 1}}};
        REQUIRE(Motion(policy, sample, *first).velocityMetersPerSecond == Math::Vec3{});

        const auto swing = [&](std::uint32_t tick, const XRTrackingSnapshot &tracking) {
            sample.tick = tick;
            sample.sequence = tick;
            sample.scope.tracking = DescribeXRInteractionCutoff(tracking);
            return Motion(policy, sample, tracking).velocityMetersPerSecond;
        };
        const auto moved = Tracking(2, Step * 2);
        sample.leftHand = Math::Vec3{0, 0, 0.05F};
        sample.rightHand = Math::Vec3{0, 0, -0.05F};
        const auto velocity = swing(2, *moved);
        REQUIRE(velocity);
        CHECK(velocity->x == Catch::Approx(-2.0F));
        CHECK(velocity->z == Catch::Approx(0.0F).margin(0.00001F));

        const auto replaced = Tracking(3, Step * 3);
        (*sample.handSources)[0] = XRDeviceId{TestSession(), {8, 2}};
        std::swap(sample.leftHand, sample.rightHand);
        CHECK(swing(3, *replaced) == Math::Vec3{});
        const auto resumed = Tracking(4, Step * 4);
        std::swap(sample.leftHand, sample.rightHand);
        const auto resumedVelocity = swing(4, *resumed);
        REQUIRE(resumedVelocity);
        CHECK(resumedVelocity->x == Catch::Approx(-2.0F));
    }

    TEST_CASE("XR teleport performs one owning query per edge and retries exact owner failures", "[xr][locomotion]") {
        auto owner = std::make_shared<DestinationOwner>();
        auto policy = Policy({XRTeleportMove{}, XRSnapTurn{0.5F, 0.8F, 0.3F}}, owner);
        const auto tracking = Tracking(1, Step);
        REQUIRE(Motion(policy, Sample(*tracking, 1), *tracking).velocityMetersPerSecond == Math::Vec3{});
        auto edge = Sample(*tracking, 2);
        edge.teleportPressed = true;
        edge.turn = 1;
        owner->response = DestinationOwner::Response::Busy;
        RequireError(policy.Evaluate(edge, *tracking), XRErrors::CapacityExceeded);
        owner->response = DestinationOwner::Response::Foreign;
        RequireError(policy.Evaluate(edge, *tracking), XRErrors::CoordinateSpaceIncompatible);
        owner->response = DestinationOwner::Response::Valid;
        const auto admitted = policy.Evaluate(edge, *tracking).Value();
        REQUIRE(std::get<XRLocomotionTeleport>(admitted->operation).destinationRevision == 17);
        ++edge.tick;
        ++edge.sequence;
        REQUIRE_FALSE(policy.Evaluate(edge, *tracking).Value());
        REQUIRE(owner->observed == 3);
        REQUIRE_FALSE(policy.Evaluate(Sample(*tracking, 4), *tracking).Value());
        edge.tick = 5;
        edge.sequence = 5;
        edge.turn = 0;
        owner->response = DestinationOwner::Response::Outside;
        REQUIRE_FALSE(policy.Evaluate(edge, *tracking).Value());
        REQUIRE(owner->observed == 4);
    }

    TEST_CASE("XR policy rejects invalid action and assignment before querying an expired teleport owner", "[xr][locomotion]") {
        auto policy = Policy({XRTeleportMove{}});
        const auto tracking = Tracking(1, Step);
        REQUIRE(Motion(policy, Sample(*tracking, 1), *tracking).velocityMetersPerSecond == Math::Vec3{});
        auto edge = Sample(*tracking, 2);
        edge.teleportPressed = true;
        edge.turn = std::numeric_limits<float>::infinity();
        RequireError(policy.Evaluate(edge, *tracking), XRErrors::OperationInvalid);
        edge.turn = 0;
        ++edge.scope.assignmentRevision;
        RequireError(policy.Evaluate(edge, *tracking), XRErrors::IdentityStale);
        --edge.scope.assignmentRevision;
        RequireError(policy.Evaluate(edge, *tracking), XRErrors::OperationUnsupported);
    }
}  // namespace
