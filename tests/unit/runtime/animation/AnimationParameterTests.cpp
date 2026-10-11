#include "AnimationGraphInstanceTestFixtures.h"
#include "Horo/Animation/AnimationParameterCurve.h"

#include <array>
#include <limits>
#include <thread>

namespace Horo::Animation {
    using Test::Id;
    using Test::Instance::Prepared;

    namespace {
        /** @brief Prepares one real immutable authored curve at the control boundary. */
        std::shared_ptr<const AnimationParameterCurve> Curve(
            const GraphCurveInterpolation interpolation = GraphCurveInterpolation::Linear) {
            auto result = AnimationParameterCurve::Create({1, Id<GraphParameterId>(1), interpolation, {{{0}, 0.0F}, {{100}, 1.0F}}});
            REQUIRE(result.HasValue());
            return std::make_shared<const AnimationParameterCurve>(std::move(result).Value());
        }
    }  // namespace

    TEST_CASE("Animation parameter triggers survive abort and consume only one committed tick", "[animation][parameters]") {
        Prepared fixture(Test::Instance::AllScalarTypes());
        auto instance = fixture.Create();
        const auto trigger = Id<GraphParameterId>(4);
        std::array<GraphParameterInput, 4> first, second;
        const auto before = HoroAnimationTestAllocationCount();
        const auto queued = instance.QueueTrigger(trigger);
        const auto coalesced = instance.QueueTrigger(trigger);
        const auto staged = instance.Evaluate({Id<AnimationTickId>(1), {1}});
        const auto rejectedWhilePending = instance.QueueTrigger(trigger);
        const auto aborted = instance.Abort();
        const auto retry = instance.Evaluate({Id<AnimationTickId>(2), {1}});
        const auto committed = instance.Commit(Id<AnimationTickId>(2));
        const auto readFirst = instance.CopyParameters(committed.publication.currentPose, first);
        const auto next = instance.Evaluate({Id<AnimationTickId>(3), {1}});
        const auto nextCommit = instance.Commit(Id<AnimationTickId>(3));
        const auto readSecond = instance.CopyParameters(nextCommit.publication.currentPose, second);
        const auto readOld = instance.CopyParameters(committed.publication.currentPose, first);
        const auto after = HoroAnimationTestAllocationCount();
        CHECK(after == before);
        CHECK(queued.HasValue());
        CHECK(coalesced.HasValue());
        CHECK(staged.HasValue());
        CHECK(rejectedWhilePending.failure == GraphInstanceFailure::StateInvalid);
        CHECK(aborted.HasValue());
        CHECK(retry.HasValue());
        REQUIRE(committed.HasValue());
        CHECK(readFirst.HasValue());
        CHECK(next.HasValue());
        REQUIRE(nextCommit.HasValue());
        CHECK(readSecond.HasValue());
        CHECK(readOld.HasValue());
        CHECK(first[3].id == trigger);
        CHECK(std::get<bool>(first[3].value));
        CHECK_FALSE(std::get<bool>(second[3].value));
        CHECK(std::get<float>(second[0].value) == 0.5F);
    }

    TEST_CASE("Animation parameter cancelled commits retain queued events and last publication", "[animation][parameters]") {
        Prepared fixture(Test::Instance::AllScalarTypes());
        auto instance = fixture.Create();
        CancellationSource cancellation;
        std::array<GraphParameterInput, 4> output;
        REQUIRE(instance.QueueTrigger(Id<GraphParameterId>(4)).HasValue());
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(1), {1}, {}, {}, cancellation.Token()}).HasValue());
        cancellation.RequestCancellation();
        CHECK(instance.Commit(Id<AnimationTickId>(1)).failure == GraphInstanceFailure::Cancelled);
        CHECK_FALSE(instance.Publication().currentPose.IsValid());
        const std::array<GraphParameterInput, 1> falseInput{{{Id<GraphParameterId>(4), false}}};
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(2), {1}, {}, falseInput}).HasValue());
        const auto committed = instance.Commit(Id<AnimationTickId>(2));
        REQUIRE(committed.HasValue());
        REQUIRE(instance.CopyParameters(committed.publication.currentPose, output).HasValue());
        CHECK(std::get<bool>(output[3].value));
    }

    TEST_CASE("Animation parameter failures and explicit invocation triggers cannot latch", "[animation][parameters]") {
        Prepared fixture(Test::Instance::AllScalarTypes());
        auto instance = fixture.Create();
        const std::array<GraphParameterInput, 1> bad{{{Id<GraphParameterId>(1), true}}};
        REQUIRE(instance.QueueTrigger(Id<GraphParameterId>(4)).HasValue());
        CHECK(instance.Evaluate({Id<AnimationTickId>(1), {1}, {}, bad}).failure == GraphInstanceFailure::TypeMismatch);
        CHECK(instance.QueueTrigger(Id<GraphParameterId>(1)).failure == GraphInstanceFailure::TypeMismatch);
        CHECK(instance.QueueTrigger(Id<GraphParameterId>(99)).failure == GraphInstanceFailure::BindingMismatch);
        CHECK(instance.QueueTrigger({}).failure == GraphInstanceFailure::BindingMismatch);
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(1), {1}}).HasValue());
        REQUIRE(instance.Commit(Id<AnimationTickId>(1)).HasValue());
        const std::array<GraphParameterInput, 1> event{{{Id<GraphParameterId>(4), true}}};
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(2), {1}, {}, event}).HasValue());
        const auto fired = instance.Commit(Id<AnimationTickId>(2));
        REQUIRE(fired.HasValue());
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(3), {1}}).HasValue());
        const auto next = instance.Commit(Id<AnimationTickId>(3));
        REQUIRE(next.HasValue());
        std::array<GraphParameterInput, 4> output;
        REQUIRE(instance.CopyParameters(next.publication.currentPose, output).HasValue());
        CHECK_FALSE(std::get<bool>(output[3].value));
        GraphInstanceOutcome foreign;
        std::thread thread([&] {
            foreign = instance.QueueTrigger(Id<GraphParameterId>(4));
        });
        thread.join();
        CHECK(foreign.failure == GraphInstanceFailure::ThreadViolation);
    }

    TEST_CASE("Animation parameter snapshots retire with publication and reload generations", "[animation][parameters]") {
        Prepared fixture(Test::Instance::AllScalarTypes());
        auto instance = fixture.Create();
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(1), {1}}).HasValue());
        const auto first = instance.Commit(Id<AnimationTickId>(1));
        REQUIRE(first.HasValue());
        for (std::uint64_t tick = 2; tick <= 3; ++tick) {
            REQUIRE(instance.Evaluate({Id<AnimationTickId>(tick), {1}}).HasValue());
            REQUIRE(instance.Commit(Id<AnimationTickId>(tick)).HasValue());
        }
        std::array<GraphParameterInput, 4> output;
        output[0].value = 99.0F;
        CHECK(instance.CopyParameters(first.publication.currentPose, output).failure == GraphInstanceFailure::HandleStale);
        CHECK(std::get<float>(output[0].value) == 99.0F);
        CHECK(instance.CopyParameters({}, output).failure == GraphInstanceFailure::HandleMalformed);
        const auto current = instance.Publication().currentPose;
        CHECK(instance.CopyParameters(current, std::span{output}.first(3)).failure == GraphInstanceFailure::LimitExceeded);
        CHECK(std::get<float>(output[0].value) == 99.0F);
        REQUIRE(instance.QueueTrigger(Id<GraphParameterId>(4)).HasValue());
        auto invalid = fixture.Assets();
        invalid.skeletonGeneration = Id<SkeletonAssetGeneration>(99);
        auto replacement = fixture.context;
        replacement.instance.slot.generation = 2;
        CHECK(instance.Replace(invalid, replacement).HasError());
        CHECK(instance.CopyParameters(current, output).HasValue());
        REQUIRE(instance.Replace(fixture.Assets(), replacement).HasValue());
        CHECK(instance.CopyParameters(current, output).failure == GraphInstanceFailure::HandleStale);
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(1), {1}}).HasValue());
        const auto reloaded = instance.Commit(Id<AnimationTickId>(1));
        REQUIRE(reloaded.HasValue());
        REQUIRE(instance.CopyParameters(reloaded.publication.currentPose, output).HasValue());
        CHECK_FALSE(std::get<bool>(output[3].value));
        REQUIRE(instance.QueueTrigger(Id<GraphParameterId>(4)).HasValue());
        REQUIRE(instance.Shutdown().HasValue());
        CHECK(instance.QueueTrigger(Id<GraphParameterId>(4)).failure == GraphInstanceFailure::AdmissionRejected);
        CHECK(instance.Evaluate({Id<AnimationTickId>(2), {1}}).failure == GraphInstanceFailure::AdmissionRejected);
        REQUIRE(instance.CopyParameters(reloaded.publication.currentPose, output).HasValue());
        CHECK_FALSE(std::get<bool>(output[3].value));
    }

    TEST_CASE("Animation authored curves clamp sample and publish gameplay scalars without allocation", "[animation][parameters][curve]") {
        Prepared fixture(Test::Instance::Blended());
        const std::array curves{Curve()};
        auto assets = fixture.Assets();
        assets.curves = curves;
        auto prepared = AnimationGraphInstance::Create(assets, fixture.context);
        REQUIRE(prepared.HasValue());
        auto instance = std::move(prepared).Value();
        std::array<GraphParameterInput, 1> output;
        std::array<Math::Transform, 1> pose;
        const auto before = HoroAnimationTestAllocationCount();
        const auto evaluated = instance.Evaluate({Id<AnimationTickId>(1), {25}, {}, {}, {}, {25}});
        const auto committed = instance.Commit(Id<AnimationTickId>(1));
        const auto parameters = instance.CopyParameters(committed.publication.currentPose, output);
        const auto copied = instance.CopyPose(committed.publication.currentPose, pose);
        const auto low = curves[0]->Sample({std::numeric_limits<std::int64_t>::min()});
        const auto high = curves[0]->Sample({std::numeric_limits<std::int64_t>::max()});
        const auto after = HoroAnimationTestAllocationCount();
        CHECK(after == before);
        CHECK(evaluated.HasValue());
        REQUIRE(committed.HasValue());
        CHECK(parameters.HasValue());
        CHECK(copied.HasValue());
        CHECK(std::get<float>(output[0].value) == 0.25F);
        CHECK(pose[0].translation.x == 50.0F);
        CHECK(low == 0.0F);
        CHECK(high == 1.0F);
        const auto step = Curve(GraphCurveInterpolation::Step);
        CHECK(step->Sample({99}) == 0.0F);
        CHECK(step->Sample({100}) == 1.0F);
        const std::array<GraphParameterInput, 1> override{{{Id<GraphParameterId>(1), 0.75F}}};
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(2), {0}, {}, override, {}, {25}}).HasValue());
        const auto overridden = instance.Commit(Id<AnimationTickId>(2));
        REQUIRE(overridden.HasValue());
        REQUIRE(instance.CopyParameters(overridden.publication.currentPose, output).HasValue());
        CHECK(std::get<float>(output[0].value) == 0.75F);
    }

    TEST_CASE("Animation linear curve extrema and maximum key storage remain finite", "[animation][parameters][curve]") {
        const auto maximum = std::numeric_limits<float>::max();
        auto extremes = AnimationParameterCurve::Create(
            {1, Id<GraphParameterId>(1), GraphCurveInterpolation::Linear, {{{0}, -maximum}, {{100}, maximum}}});
        REQUIRE(extremes.HasValue());
        CHECK(extremes.Value().Sample({50}) == 0.0F);
        GraphParameterCurveData data{1, Id<GraphParameterId>(1), GraphCurveInterpolation::Linear, {}};
        data.keys.reserve(AnimationParameterCurve::MaximumKeys);
        for (std::uint32_t index = 0; index < AnimationParameterCurve::MaximumKeys; ++index)
            data.keys.push_back({{index}, static_cast<float>(index)});
        auto bounded = AnimationParameterCurve::Create(std::move(data));
        REQUIRE(bounded.HasValue());
        const auto before = HoroAnimationTestAllocationCount();
        const auto sampled = bounded.Value().Sample({2048});
        const auto after = HoroAnimationTestAllocationCount();
        CHECK(after == before);
        CHECK(sampled == 2048.0F);
    }

    TEST_CASE("Animation curve preparation rejects unsupported malformed oversized and cancelled data", "[animation][parameters][curve]") {
        GraphParameterCurveData data{1, Id<GraphParameterId>(1), GraphCurveInterpolation::Linear, {{{0}, 0.0F}, {{100}, 1.0F}}};
        SECTION("version") {
            data.version = 2;
        }
        SECTION("interpolation") {
            data.interpolation = static_cast<GraphCurveInterpolation>(99);
        }
        SECTION("missing binding") {
            data.parameter = {};
        }
        SECTION("empty") {
            data.keys.clear();
        }
        SECTION("duplicate times") {
            data.keys[1].time = {0};
        }
        SECTION("unsorted times") {
            data.keys[0].time = {101};
        }
        SECTION("negative time") {
            data.keys[0].time = {-1};
        }
        SECTION("nonfinite") {
            data.keys[1].value = std::numeric_limits<float>::infinity();
        }
        SECTION("capacity") {
            data.keys.resize(AnimationParameterCurve::MaximumKeys + 1);
        }
        SECTION("cancelled") {
            CancellationSource cancellation;
            cancellation.RequestCancellation();
            CHECK(AnimationParameterCurve::Create(data, cancellation.Token()).HasError());
            return;
        }
        CHECK(AnimationParameterCurve::Create(data).HasError());
    }

    TEST_CASE("Animation moved-from curves fail admission without retiring publication or queued triggers",
              "[animation][parameters][curve]") {
        Prepared fixture(Test::Instance::AllScalarTypes());
        auto instance = fixture.Create();
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(1), {1}}).HasValue());
        const auto committed = instance.Commit(Id<AnimationTickId>(1));
        REQUIRE(committed.HasValue());
        REQUIRE(instance.QueueTrigger(Id<GraphParameterId>(4)).HasValue());

        auto prepared =
            AnimationParameterCurve::Create({1, Id<GraphParameterId>(1), GraphCurveInterpolation::Linear, {{{0}, 0.0F}, {{100}, 1.0F}}});
        REQUIRE(prepared.HasValue());
        auto curve = std::move(prepared).Value();
        const auto retained = std::make_shared<const AnimationParameterCurve>(std::move(curve));
        REQUIRE(retained->Sample({50}) == 0.5F);
        REQUIRE(curve.Keys().empty());
        REQUIRE(curve.Parameter() == Id<GraphParameterId>(1));
        const std::array invalidCurves{std::make_shared<const AnimationParameterCurve>(std::move(curve))};
        auto invalid = fixture.Assets();
        invalid.curves = invalidCurves;
        auto rejected = AnimationGraphInstance::Create(invalid, fixture.context);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == AnimationErrors::GraphMalformed.code.Value());
        auto replacement = fixture.context;
        replacement.instance.slot.generation = 2;
        const auto failedReplacement = instance.Replace(invalid, replacement);
        REQUIRE(failedReplacement.HasError());
        CHECK(failedReplacement.ErrorValue().code.Value() == AnimationErrors::GraphMalformed.code.Value());
        CHECK(instance.Publication().currentPose == committed.publication.currentPose);
        std::array<GraphParameterInput, 4> output;
        REQUIRE(instance.CopyParameters(committed.publication.currentPose, output).HasValue());
        CHECK_FALSE(std::get<bool>(output[3].value));
        REQUIRE(instance.Evaluate({Id<AnimationTickId>(2), {1}}).HasValue());
        const auto next = instance.Commit(Id<AnimationTickId>(2));
        REQUIRE(next.HasValue());
        REQUIRE(instance.CopyParameters(next.publication.currentPose, output).HasValue());
        CHECK(std::get<bool>(output[3].value));
    }

    TEST_CASE("Animation curves admit exact unique float bindings and combined bounded work", "[animation][parameters][curve]") {
        Prepared fixture(Test::Instance::AllScalarTypes());
        auto assets = fixture.Assets();
        std::array<std::shared_ptr<const AnimationParameterCurve>, 2> curves{Curve(), Curve()};
        assets.curves = curves;
        CHECK(AnimationGraphInstance::Create(assets, fixture.context).HasError());
        assets.curves = std::span{curves}.first(1);
        auto limited = fixture.context;
        limited.maximumJointOperations = 3;
        CHECK(AnimationGraphInstance::Create(assets, limited).HasError());
        curves[0].reset();
        CHECK(AnimationGraphInstance::Create(assets, fixture.context).HasError());
        auto badType = AnimationParameterCurve::Create({1, Id<GraphParameterId>(4), {}, {{{0}, 1.0F}}});
        REQUIRE(badType.HasValue());
        curves[0] = std::make_shared<const AnimationParameterCurve>(std::move(badType).Value());
        CHECK(AnimationGraphInstance::Create(assets, fixture.context).HasError());
        auto missing = AnimationParameterCurve::Create({1, Id<GraphParameterId>(99), {}, {{{0}, 1.0F}}});
        REQUIRE(missing.HasValue());
        curves[0] = std::make_shared<const AnimationParameterCurve>(std::move(missing).Value());
        CHECK(AnimationGraphInstance::Create(assets, fixture.context).HasError());
    }
}  // namespace Horo::Animation
