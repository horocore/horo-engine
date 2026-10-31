#include "Horo/XR/XRErrors.h"
#include "Horo/XR/XRSpacePose.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

namespace Horo::XR {
    namespace {
        template <typename Generation> Generation MakeGeneration(const std::uint64_t value) {
            auto result = Generation::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        XRSystemId System(const std::uint64_t runtime = 1) {
            return {MakeGeneration<XRRuntimeGeneration>(runtime), {2, 3}};
        }

        XRSessionId Session(const std::uint32_t generation = 5) {
            return {System(), {4, generation}};
        }

        XRCoordinateSpace Space(const XRSessionId &session, const XRSpaceKind kind, const std::uint32_t index,
                                const std::uint64_t origin = 9) {
            return {.id = {session, {index, 1}}, .kind = kind, .worldOriginRevision = MakeGeneration<XRWorldOriginRevision>(origin)};
        }

        template <typename Value> XRPoseComponent<Value> Tracked(const Value &value) {
            return {.value = value, .validity = XRPoseComponentValidity::Tracked};
        }

        XRTimeEvidence SimulationTime() {
            return {.runtimeSample = MakeGeneration<XRRuntimeTime>(100), .simulation = MakeGeneration<XRSimulationTime>(80)};
        }

        XRPoseDescriptor Descriptor(const XRSessionId &session = Session()) {
            return {.session = session,
                    .source = Space(session, XRSpaceKind::View, 1),
                    .target = Space(session, XRSpaceKind::Local, 2),
                    .purpose = XRPosePurpose::SimulationInput,
                    .time = SimulationTime(),
                    .components = {.positionMeters = Tracked(Math::Vec3{1.0F, 2.0F, 3.0F}),
                                   .orientation = Tracked(Math::Quaternion::Identity()),
                                   .linearVelocityMetersPerSecond = Tracked(Math::Vec3{4.0F, 0.0F, 0.0F}),
                                   .angularVelocityRadiansPerSecond = Tracked(Math::Vec3{0.0F, 1.0F, 0.0F}),
                                   .confidence = XRTrackingConfidence::High,
                                   .loss = XRTrackingLossState::None}};
        }

        template <typename Value> void RequireFailure(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            const std::pair actualIdentity{result.ErrorValue().domain.Value(), result.ErrorValue().code.Value()};
            const std::pair expectedIdentity{expected.domain.Value(), expected.code.Value()};
            REQUIRE(actualIdentity == expectedIdentity);
        }
    }  // namespace

    TEST_CASE("XR time domains are strong and pose publications retain exact evidence", "[unit][xr][space-pose]") {
        static_assert(!std::is_same_v<XRRuntimeTime, XRSimulationTime>);
        static_assert(!std::is_same_v<XRRuntimeTime, XRRenderPredictionTime>);
        static_assert(!std::is_convertible_v<XRRuntimeTime, XRSimulationTime>);
        REQUIRE(XRRuntimeTime::Create(0).HasError());

        const auto session = Session();
        const auto origin = MakeGeneration<XRWorldOriginRevision>(9);
        auto sample = XRPoseSample::Create(Descriptor(session), session, origin);
        REQUIRE(sample.HasValue());
        REQUIRE(sample.Value().Session() == session);
        REQUIRE(sample.Value().Source().kind == XRSpaceKind::View);
        REQUIRE(sample.Value().Target().kind == XRSpaceKind::Local);
        REQUIRE(sample.Value().Time().runtimeSample.Nanoseconds() == 100);
        REQUIRE(sample.Value().Time().simulation->Nanoseconds() == 80);
        REQUIRE(*sample.Value().Components().positionMeters.value == Math::Vec3{1.0F, 2.0F, 3.0F});
    }

    TEST_CASE("XR simulation and presentation timestamps cannot be mixed implicitly", "[unit][xr][space-pose]") {
        const auto session = Session();
        const auto origin = MakeGeneration<XRWorldOriginRevision>(9);
        auto mixed = Descriptor(session);
        mixed.time.renderPrediction = MakeGeneration<XRRenderPredictionTime>(120);
        RequireFailure(XRPoseSample::Create(mixed, session, origin), XRErrors::TimeDomainIncompatible);

        auto predicted = Descriptor(session);
        predicted.purpose = XRPosePurpose::PresentationPrediction;
        predicted.time.simulation.reset();
        predicted.time.renderPrediction = MakeGeneration<XRRenderPredictionTime>(120);
        auto accepted = XRPoseSample::Create(predicted, session, origin);
        REQUIRE(accepted.HasValue());
        REQUIRE(accepted.Value().Purpose() == XRPosePurpose::PresentationPrediction);
    }

    TEST_CASE("XR simulation epoch is valid without manufacturing runtime or prediction evidence", "[unit][xr][space-pose]") {
        static_assert(sizeof(XRSimulationTime) == sizeof(std::int64_t));
        static_assert(sizeof(XRRuntimeTime) == sizeof(std::int64_t));
        REQUIRE_FALSE(XRSimulationTime{}.IsValid());
        REQUIRE(XRSimulationTime{}.Nanoseconds() == -1);
        REQUIRE(XRSimulationTime::Create(XRSimulationTime{}.Nanoseconds()).HasError());
        REQUIRE(XRSimulationTime::Create(-1).HasError());
        REQUIRE(XRSimulationTime::Create(std::numeric_limits<std::int64_t>::min()).HasError());
        REQUIRE(XRRuntimeTime::Create(0).HasError());
        REQUIRE(XRRenderPredictionTime::Create(0).HasError());
        REQUIRE_FALSE(XRRuntimeTime{}.IsValid());
        REQUIRE_FALSE(XRRenderPredictionTime{}.IsValid());

        const auto epoch = XRSimulationTime::Create(0);
        REQUIRE(epoch.HasValue());
        REQUIRE(epoch.Value().IsValid());
        REQUIRE(epoch.Value().Nanoseconds() == 0);
        REQUIRE(XRSimulationTime::Create(epoch.Value().Nanoseconds()).Value().IsValid());
        REQUIRE(epoch.Value() != XRSimulationTime{});
        const auto maximum = XRSimulationTime::Create(std::numeric_limits<std::int64_t>::max());
        REQUIRE(maximum.HasValue());
        REQUIRE(maximum.Value().Nanoseconds() == std::numeric_limits<std::int64_t>::max());

        auto descriptor = Descriptor();
        descriptor.time.simulation = epoch.Value();
        const auto origin = MakeGeneration<XRWorldOriginRevision>(9);
        auto first = XRPoseSample::Create(descriptor, Session(), origin);
        REQUIRE(first.HasValue());
        REQUIRE(first.Value().Time().simulation->IsValid());
        REQUIRE(first.Value().Time().simulation->Nanoseconds() == 0);
        // Pausing keeps the same canonical elapsed time; native sample evidence remains independent.
        descriptor.time.runtimeSample = MakeGeneration<XRRuntimeTime>(101);
        auto paused = XRPoseSample::Create(descriptor, Session(), origin);
        REQUIRE(paused.HasValue());
        REQUIRE(paused.Value().Time().simulation == first.Value().Time().simulation);
        REQUIRE(paused.Value().Time().runtimeSample != first.Value().Time().runtimeSample);
        descriptor.time.simulation = XRSimulationTime{};
        RequireFailure(XRPoseSample::Create(descriptor, Session(), origin), XRErrors::TimeDomainIncompatible);
    }

    TEST_CASE("XR pose components reject nonfinite fabricated and contradictory validity", "[unit][xr][space-pose]") {
        const auto session = Session();
        const auto origin = MakeGeneration<XRWorldOriginRevision>(9);
        auto nonfinite = Descriptor(session);
        nonfinite.components.positionMeters.value->x = std::numeric_limits<float>::quiet_NaN();
        RequireFailure(XRPoseSample::Create(nonfinite, session, origin), XRErrors::PoseInvalid);

        auto fabricated = Descriptor(session);
        fabricated.components.orientation.validity = XRPoseComponentValidity::Invalid;
        RequireFailure(XRPoseSample::Create(fabricated, session, origin), XRErrors::PoseInvalid);

        auto notUnit = Descriptor(session);
        notUnit.components.orientation.value = Math::Quaternion{0.0F, 0.0F, 0.0F, 2.0F};
        RequireFailure(XRPoseSample::Create(notUnit, session, origin), XRErrors::PoseInvalid);
    }

    TEST_CASE("XR loss states preserve independently absent pose components", "[unit][xr][space-pose]") {
        const auto session = Session();
        const auto origin = MakeGeneration<XRWorldOriginRevision>(9);
        auto positionLost = Descriptor(session);
        positionLost.components.positionMeters = {};
        positionLost.components.linearVelocityMetersPerSecond = {};
        positionLost.components.loss = XRTrackingLossState::PositionLost;
        auto accepted = XRPoseSample::Create(positionLost, session, origin);
        REQUIRE(accepted.HasValue());
        REQUIRE_FALSE(accepted.Value().Components().positionMeters.value.has_value());
        REQUIRE(accepted.Value().Components().orientation.value.has_value());

        positionLost.components.loss = XRTrackingLossState::None;
        RequireFailure(XRPoseSample::Create(positionLost, session, origin), XRErrors::PoseInvalid);

        auto fullyLostWithVelocity = Descriptor(session);
        fullyLostWithVelocity.components.positionMeters = {};
        fullyLostWithVelocity.components.orientation = {};
        fullyLostWithVelocity.components.angularVelocityRadiansPerSecond = {};
        fullyLostWithVelocity.components.confidence = XRTrackingConfidence::None;
        fullyLostWithVelocity.components.loss = XRTrackingLossState::FullyLost;
        RequireFailure(XRPoseSample::Create(fullyLostWithVelocity, session, origin), XRErrors::PoseInvalid);
    }

    TEST_CASE("XR poses reject replacement shutdown foreign spaces and stale origins", "[unit][xr][space-pose]") {
        const auto session = Session();
        const auto origin = MakeGeneration<XRWorldOriginRevision>(9);
        RequireFailure(XRPoseSample::Create(Descriptor(session), {}, origin), XRErrors::IdentityInvalid);
        RequireFailure(XRPoseSample::Create(Descriptor(session), Session(6), origin), XRErrors::IdentityStale);

        auto foreign = Descriptor(session);
        foreign.target = Space(Session(6), XRSpaceKind::Local, 2);
        RequireFailure(XRPoseSample::Create(foreign, session, origin), XRErrors::IdentityStale);

        RequireFailure(XRPoseSample::Create(Descriptor(session), session, MakeGeneration<XRWorldOriginRevision>(10)),
                       XRErrors::OriginRevisionStale);
        RequireFailure(XRPoseSample::Create(Descriptor(session), session, {}), XRErrors::OperationInvalid);

        auto unsupportedConvention = Descriptor(session);
        unsupportedConvention.source.convention = XRCoordinateConvention::LeftHandedYUpPositiveZForward;
        RequireFailure(XRPoseSample::Create(unsupportedConvention, session, origin), XRErrors::OperationUnsupported);

        auto invalidSpace = Descriptor(session);
        invalidSpace.source.kind = XRSpaceKind::Count;
        RequireFailure(XRPoseSample::Create(invalidSpace, session, origin), XRErrors::OperationInvalid);
    }

    TEST_CASE("XR action spaces preserve independent pose validity and exact session origin fences", "[unit][xr][space-pose]") {
        static_assert(static_cast<unsigned>(XRSpaceKind::View) == 0);
        static_assert(static_cast<unsigned>(XRSpaceKind::Local) == 1);
        static_assert(static_cast<unsigned>(XRSpaceKind::Stage) == 2);
        static_assert(static_cast<unsigned>(XRSpaceKind::World) == 3);
        static_assert(static_cast<unsigned>(XRSpaceKind::Action) == 4);
        const auto session = Session();
        const auto origin = MakeGeneration<XRWorldOriginRevision>(9);
        auto descriptor = Descriptor(session);
        descriptor.source.kind = XRSpaceKind::Action;
        descriptor.components.positionMeters = {};
        descriptor.components.linearVelocityMetersPerSecond = {};
        descriptor.components.loss = XRTrackingLossState::PositionLost;
        const auto sample = XRPoseSample::Create(descriptor, session, origin);
        REQUIRE(sample.HasValue());
        CHECK(sample.Value().Source().kind == XRSpaceKind::Action);
        CHECK_FALSE(sample.Value().Components().positionMeters.value.has_value());
        CHECK(sample.Value().Components().orientation.value.has_value());
        RequireFailure(XRPoseSample::Create(descriptor, Session(6), origin), XRErrors::IdentityStale);
        RequireFailure(XRPoseSample::Create(descriptor, session, MakeGeneration<XRWorldOriginRevision>(10)), XRErrors::OriginRevisionStale);
        RequireFailure(XRPoseSample::Create(descriptor, {}, origin), XRErrors::IdentityInvalid);
    }

    TEST_CASE("XR coordinate transforms compose deterministically without changing time authority", "[unit][xr][space-pose]") {
        const auto session = Session();
        const auto origin = MakeGeneration<XRWorldOriginRevision>(9);
        auto firstDescriptor = Descriptor(session);
        auto secondDescriptor = Descriptor(session);
        secondDescriptor.source = firstDescriptor.target;
        secondDescriptor.target = Space(session, XRSpaceKind::World, 3);
        secondDescriptor.components.positionMeters = Tracked(Math::Vec3{10.0F, 0.0F, 0.0F});
        auto first = XRPoseSample::Create(firstDescriptor, session, origin);
        auto second = XRPoseSample::Create(secondDescriptor, session, origin);
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());

        const std::array chain{first.Value(), second.Value()};
        auto composed = ComposeXRSpaceTransforms(chain, session, origin);
        REQUIRE(composed.HasValue());
        REQUIRE(composed.Value().Source() == first.Value().Source());
        REQUIRE(composed.Value().Target() == second.Value().Target());
        REQUIRE(*composed.Value().Components().positionMeters.value == Math::Vec3{11.0F, 2.0F, 3.0F});
        REQUIRE(*composed.Value().Components().linearVelocityMetersPerSecond.value == Math::Vec3{11.0F, 0.0F, -1.0F});
        REQUIRE(composed.Value().Time() == first.Value().Time());
    }

    TEST_CASE("XR coordinate chains reject gaps time conflicts and bounded overflow", "[unit][xr][space-pose]") {
        const auto session = Session();
        const auto origin = MakeGeneration<XRWorldOriginRevision>(9);
        auto first = XRPoseSample::Create(Descriptor(session), session, origin);
        REQUIRE(first.HasValue());

        auto gapDescriptor = Descriptor(session);
        gapDescriptor.source = Space(session, XRSpaceKind::Stage, 7);
        gapDescriptor.target = Space(session, XRSpaceKind::World, 8);
        auto gap = XRPoseSample::Create(gapDescriptor, session, origin);
        REQUIRE(gap.HasValue());
        const std::array gapChain{first.Value(), gap.Value()};
        RequireFailure(ComposeXRSpaceTransforms(gapChain, session, origin), XRErrors::CoordinateSpaceIncompatible);

        auto conflictingDescriptor = Descriptor(session);
        conflictingDescriptor.source = first.Value().Target();
        conflictingDescriptor.source.kind = XRSpaceKind::Stage;
        conflictingDescriptor.target = Space(session, XRSpaceKind::World, 8);
        auto conflicting = XRPoseSample::Create(conflictingDescriptor, session, origin);
        REQUIRE(conflicting.HasValue());
        const std::array conflictingChain{first.Value(), conflicting.Value()};
        RequireFailure(ComposeXRSpaceTransforms(conflictingChain, session, origin), XRErrors::CoordinateSpaceIncompatible);

        auto contradictoryIdentity = Descriptor(session);
        contradictoryIdentity.target.id = contradictoryIdentity.source.id;
        RequireFailure(XRPoseSample::Create(contradictoryIdentity, session, origin), XRErrors::CoordinateSpaceIncompatible);

        auto predictedDescriptor = Descriptor(session);
        predictedDescriptor.source = first.Value().Target();
        predictedDescriptor.target = Space(session, XRSpaceKind::World, 8);
        predictedDescriptor.purpose = XRPosePurpose::PresentationPrediction;
        predictedDescriptor.time.simulation.reset();
        predictedDescriptor.time.renderPrediction = MakeGeneration<XRRenderPredictionTime>(120);
        auto predicted = XRPoseSample::Create(predictedDescriptor, session, origin);
        REQUIRE(predicted.HasValue());
        const std::array timeConflict{first.Value(), predicted.Value()};
        RequireFailure(ComposeXRSpaceTransforms(timeConflict, session, origin), XRErrors::TimeDomainIncompatible);

        const std::array excessive{first.Value(), first.Value(), first.Value(), first.Value(), first.Value(),
                                   first.Value(), first.Value(), first.Value(), first.Value()};
        RequireFailure(ComposeXRSpaceTransforms(excessive, session, origin), XRErrors::CapacityExceeded);
        RequireFailure(ComposeXRSpaceTransforms(std::span<const XRPoseSample>{}, session, origin), XRErrors::OperationInvalid);
    }

    TEST_CASE("XR coordinate composition accepts the exact hard hop boundary", "[unit][xr][space-pose]") {
        const auto session = Session();
        const auto origin = MakeGeneration<XRWorldOriginRevision>(9);
        const auto makeTransform = [&](const std::uint32_t source, const std::uint32_t target) {
            auto descriptor = Descriptor(session);
            descriptor.source = Space(session, XRSpaceKind::Local, source);
            descriptor.target = Space(session, XRSpaceKind::Local, target);
            auto sample = XRPoseSample::Create(descriptor, session, origin);
            REQUIRE(sample.HasValue());
            return sample.Value();
        };
        const std::array chain{makeTransform(1, 2), makeTransform(2, 3), makeTransform(3, 4), makeTransform(4, 5),
                               makeTransform(5, 6), makeTransform(6, 7), makeTransform(7, 8), makeTransform(8, 9)};
        auto composed = ComposeXRSpaceTransforms(chain, session, origin);
        REQUIRE(composed.HasValue());
        REQUIRE(composed.Value().Source() == chain.front().Source());
        REQUIRE(composed.Value().Target() == chain.back().Target());
    }

    TEST_CASE("XR single-transform composition revalidates replacement shutdown and origin fences", "[unit][xr][space-pose]") {
        const auto session = Session();
        const auto origin = MakeGeneration<XRWorldOriginRevision>(9);
        auto sample = XRPoseSample::Create(Descriptor(session), session, origin);
        REQUIRE(sample.HasValue());
        const std::array chain{sample.Value()};

        REQUIRE(ComposeXRSpaceTransforms(chain, session, origin).HasValue());
        RequireFailure(ComposeXRSpaceTransforms(chain, {}, origin), XRErrors::IdentityInvalid);
        RequireFailure(ComposeXRSpaceTransforms(chain, Session(6), origin), XRErrors::IdentityStale);
        RequireFailure(ComposeXRSpaceTransforms(chain, session, MakeGeneration<XRWorldOriginRevision>(10)), XRErrors::OriginRevisionStale);
    }
}  // namespace Horo::XR
