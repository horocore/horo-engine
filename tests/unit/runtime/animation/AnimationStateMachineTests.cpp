#include "AnimationStateMachineInternal.h"
#include "AnimationTestFixtures.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace Horo::Animation::Test {
    namespace {
        using namespace Detail;

        /** @brief Real bounded kernel storage; committed state is copied only after a successful staged result. */
        struct Machine final {
            std::vector<StateMachineState> states{{{10}}, {{20}}, {{30}}};
            std::vector<GraphParameter> declarations{{Id<GraphParameterId>(1), "speed", GraphValueType::Float, 0.0F},
                                                     {Id<GraphParameterId>(2), "trigger", GraphValueType::Trigger, false}};
            std::vector<StateMachineCondition> conditions{{0, StateMachinePredicate::Above, 1.0F, 0.5F}};
            std::vector<StateMachineTransition> transitions{{{100}, 0, 1, 1, 0, 1, {100}, {}, StateMachineInterruption::Either}};
            std::array<GraphParameterValue, 2> parameters{0.0F, false};
            StateMachineRuntime committed;
            StateMachineRuntime candidate;
            std::vector<std::uint8_t> latches{0};
            std::vector<std::uint8_t> candidateLatches{0};
            std::array<StateMachineEvent, 262> events{};
            std::array<StateMachinePlayback, 2> playback{};

            StateMachinePlan Plan() const {
                return {states, transitions, conditions, 0};
            }

            StateMachineStepOutcome Stage(const std::int64_t advance = 0, const std::size_t capacity = 262,
                                          const CancellationToken cancellation = {}) {
                const auto count = committed.initialized ? (committed.destination ? 2U : 1U) : 0U;
                if (count != 0)
                    playback[0].activation = committed.source;
                if (count == 2)
                    playback[1].activation = *committed.destination;
                return StepStateMachine(Plan(), committed, latches, {parameters, std::span{playback}.first(count), {advance}, cancellation},
                                        candidate, candidateLatches, std::span{events}.first(capacity));
            }

            void Commit() {
                committed = candidate;
                latches = candidateLatches;
            }

            void Start() {
                REQUIRE(ValidateStateMachine(Plan(), declarations) == GraphInstanceFailure::None);
                const auto entered = Stage();
                REQUIRE(entered.failure == GraphInstanceFailure::None);
                REQUIRE(entered.events == 1);
                CHECK(events[0].kind == StateMachineEventKind::Entered);
                Commit();
            }
        };
    }  // namespace

    TEST_CASE("Animation state machine entries and transition curves stage without heap work", "[animation][state_machine]") {
        Machine machine;
        machine.Start();
        machine.parameters[0] = 2.0F;
        const auto before = HoroAnimationTestAllocationCount();
        const auto started = machine.Stage(20);
        const auto after = HoroAnimationTestAllocationCount();
        CHECK(after == before);
        REQUIRE(started.failure == GraphInstanceFailure::None);
        CHECK(started.resetDestination);
        CHECK_FALSE(started.captureSource);
        CHECK(started.weight == 0.0F);
        CHECK(started.events == 2);
        CHECK(machine.events[0].kind == StateMachineEventKind::TransitionStarted);
        CHECK(machine.events[1].kind == StateMachineEventKind::Entered);
        CHECK(machine.events[1].activation == 2);
        CHECK_FALSE(machine.committed.transition);
        machine.Commit();
        for (const auto curve : {StateMachineBlendCurve::Linear, StateMachineBlendCurve::SmoothStep, StateMachineBlendCurve::EaseIn,
                                 StateMachineBlendCurve::EaseOut}) {
            machine.transitions[0].curve = curve;
            const auto advanced = machine.Stage(25);
            REQUIRE(advanced.failure == GraphInstanceFailure::None);
            const float expected = curve == StateMachineBlendCurve::Linear       ? 0.25F
                                   : curve == StateMachineBlendCurve::SmoothStep ? 0.15625F
                                   : curve == StateMachineBlendCurve::EaseIn     ? 0.0625F
                                                                                 : 0.4375F;
            CHECK(advanced.weight == expected);
        }
        const auto complete = machine.Stage(std::numeric_limits<std::int64_t>::max());
        REQUIRE(complete.failure == GraphInstanceFailure::None);
        CHECK(complete.events == 2);
        CHECK(machine.events[0].kind == StateMachineEventKind::Exited);
        CHECK(machine.events[1].kind == StateMachineEventKind::TransitionCompleted);
        CHECK(machine.candidate.source.state == 1);
        CHECK_FALSE(machine.candidate.transition);
        CHECK(machine.committed.source.state == 0);
    }

    TEST_CASE("Animation state machine hysteresis and exact exit time survive retries", "[animation][state_machine]") {
        Machine machine;
        machine.transitions[0].exitTime = AnimationTime{100};
        machine.Start();
        machine.parameters[0] = 1.0F;
        REQUIRE(machine.Stage().events == 0);
        CHECK(machine.candidateLatches[0] == 0);
        machine.parameters[0] = 1.25F;
        REQUIRE(machine.Stage().events == 0);
        CHECK(machine.candidateLatches[0] == 1);
        CHECK(machine.latches[0] == 0);
        machine.Commit();
        machine.parameters[0] = 0.75F;
        machine.playback[0].elapsed = {99};
        REQUIRE(machine.Stage().events == 0);
        CHECK(machine.candidateLatches[0] == 1);
        machine.playback[0].elapsed = {100};
        const auto eligible = machine.Stage();
        REQUIRE(eligible.failure == GraphInstanceFailure::None);
        CHECK(eligible.events == 2);
        // Discarding the private candidate must not consume the transition or alter its committed latch.
        CHECK_FALSE(machine.committed.transition);
        const auto retry = machine.Stage();
        CHECK(retry.events == eligible.events);
        machine.parameters[0] = 0.5F;
        REQUIRE(machine.Stage().events == 0);
        CHECK(machine.candidateLatches[0] == 0);
    }

    TEST_CASE("Animation state machine priority ties are independent of transition table order", "[animation][state_machine]") {
        Machine machine;
        machine.conditions.clear();
        machine.latches.clear();
        machine.candidateLatches.clear();
        machine.transitions = {{{400}, 0, 1, 7}, {{200}, 0, 2, 7}, {{100}, 0, 1, 6}};
        REQUIRE(ValidateStateMachine(machine.Plan(), machine.declarations) == GraphInstanceFailure::None);
        const auto first = machine.Stage();
        REQUIRE(first.failure == GraphInstanceFailure::None);
        CHECK(machine.candidate.source.state == 2);
        CHECK(machine.events[1].transition.value == 200);
        std::ranges::reverse(machine.transitions);
        const auto reordered = machine.Stage();
        REQUIRE(reordered.failure == GraphInstanceFailure::None);
        CHECK(machine.candidate.source.state == 2);
        CHECK(machine.events[1].transition.value == 200);
    }

    TEST_CASE("Animation state machine interruption captures composed pose and retires exact entries", "[animation][state_machine]") {
        Machine machine;
        machine.Start();
        machine.parameters[0] = 2.0F;
        REQUIRE(machine.Stage().failure == GraphInstanceFailure::None);
        machine.Commit();
        machine.conditions.push_back({1, StateMachinePredicate::Trigger, true});
        machine.transitions.push_back({{200}, 1, 2, 2, 1, 1, {50}});
        machine.latches.push_back(0);
        machine.candidateLatches.push_back(0);
        REQUIRE(ValidateStateMachine(machine.Plan(), machine.declarations) == GraphInstanceFailure::None);
        machine.parameters[1] = true;
        const auto interrupted = machine.Stage(10);
        REQUIRE(interrupted.failure == GraphInstanceFailure::None);
        CHECK(interrupted.captureSource);
        CHECK(interrupted.resetDestination);
        CHECK(interrupted.events == 4);
        CHECK(machine.events[0].kind == StateMachineEventKind::TransitionInterrupted);
        CHECK(machine.events[1].kind == StateMachineEventKind::Exited);
        CHECK(machine.events[1].activation == 1);
        CHECK(machine.events[3].activation == 3);
        CHECK(machine.candidate.source.state == 1);
        CHECK(machine.candidate.frozenSource);
        CHECK(machine.candidate.elapsed.ticks == 0);
        machine.transitions[1].priority = 1;
        CHECK_FALSE(machine.Stage().captureSource);
        machine.transitions[1].priority = 2;
        machine.transitions[0].interruption = StateMachineInterruption::None;
        CHECK_FALSE(machine.Stage().captureSource);
    }

    TEST_CASE("Animation state machine loop events preserve directed ordinals and capacity atomically", "[animation][state_machine]") {
        Machine machine;
        machine.Start();
        machine.playback[0] = {machine.committed.source, {100}, 3, 7, AnimationTraversalDirection::Reverse};
        const auto original = machine.committed;
        CHECK(machine.Stage(0, 2).failure == GraphInstanceFailure::LimitExceeded);
        CHECK(machine.committed == original);
        const auto bounded = machine.Stage(0, 3);
        REQUIRE(bounded.failure == GraphInstanceFailure::None);
        REQUIRE(bounded.events == 3);
        for (std::size_t index = 0; index < 3; ++index) {
            CHECK(machine.events[index].kind == StateMachineEventKind::Loop);
            CHECK(machine.events[index].loopOrdinal == 7 + index);
            CHECK(machine.events[index].direction == AnimationTraversalDirection::Reverse);
        }
        machine.playback[0].loops = StateMachinePlan::MaximumLoopEvents + 1;
        CHECK(machine.Stage().failure == GraphInstanceFailure::LimitExceeded);
        machine.playback[0].loops = 2;
        machine.playback[0].firstLoopOrdinal = std::numeric_limits<std::uint64_t>::max();
        CHECK(machine.Stage().failure == GraphInstanceFailure::LimitExceeded);
    }

    TEST_CASE("Animation state machine self transitions preserve distinct entry generations", "[animation][state_machine]") {
        Machine machine;
        machine.transitions[0].destination = 0;
        machine.transitions[0].duration = {0};
        machine.parameters[0] = 2.0F;
        REQUIRE(ValidateStateMachine(machine.Plan(), machine.declarations) == GraphInstanceFailure::None);
        const auto entered = machine.Stage();
        REQUIRE(entered.failure == GraphInstanceFailure::None);
        CHECK(entered.events == 5);
        CHECK(machine.events[2].activation == 2);
        CHECK(machine.events[3].activation == 1);
        CHECK(machine.candidate.source.generation == 2);
        machine.Commit();
        REQUIRE(machine.Stage().failure == GraphInstanceFailure::None);
        CHECK(machine.candidate.source.generation == 3);
        CHECK(machine.committed.source.generation == 2);
    }

    TEST_CASE("Animation state machine rejects malformed candidates and cancellation without committed mutation",
              "[animation][state_machine]") {
        Machine machine;
        machine.Start();
        const auto original = machine.committed;
        SECTION("typed parameter") {
            machine.parameters[0] = false;
            CHECK(machine.Stage().failure == GraphInstanceFailure::TypeMismatch);
        }
        SECTION("finite parameter") {
            machine.parameters[0] = std::numeric_limits<float>::infinity();
            CHECK(machine.Stage().failure == GraphInstanceFailure::Malformed);
        }
        SECTION("negative clock") {
            CHECK(machine.Stage(-1).failure == GraphInstanceFailure::Malformed);
        }
        SECTION("latch representation") {
            machine.latches[0] = 2;
            CHECK(machine.Stage().failure == GraphInstanceFailure::Malformed);
        }
        SECTION("generation exhaustion") {
            machine.committed.nextActivation = std::numeric_limits<std::uint64_t>::max();
            machine.parameters[0] = 2.0F;
            CHECK(machine.Stage().failure == GraphInstanceFailure::LimitExceeded);
            machine.committed.nextActivation = original.nextActivation;
        }
        SECTION("cancelled") {
            CancellationSource source;
            source.RequestCancellation();
            CHECK(machine.Stage(0, 262, source.Token()).failure == GraphInstanceFailure::Cancelled);
        }
        CHECK(machine.committed == original);
    }

    TEST_CASE("Animation state machine admission rejects invalid identities bindings policies and condition ranges",
              "[animation][state_machine]") {
        Machine machine;
        SECTION("zero state") {
            machine.states[0].id = {};
        }
        SECTION("duplicate state") {
            machine.states[1].id = machine.states[0].id;
        }
        SECTION("zero transition") {
            machine.transitions[0].id = {};
        }
        SECTION("missing source") {
            machine.transitions[0].source = 99;
        }
        SECTION("negative duration") {
            machine.transitions[0].duration = {-1};
        }
        SECTION("negative exit time") {
            machine.transitions[0].exitTime = AnimationTime{-1};
        }
        SECTION("interruption policy") {
            machine.transitions[0].interruption = static_cast<StateMachineInterruption>(99);
        }
        SECTION("blend curve") {
            machine.transitions[0].curve = static_cast<StateMachineBlendCurve>(99);
        }
        SECTION("condition range") {
            machine.transitions[0].firstCondition = 1;
        }
        SECTION("hysteresis") {
            machine.conditions[0].releaseThreshold = 2.0F;
        }
        SECTION("missing parameter") {
            machine.conditions[0].parameter = 99;
        }
        SECTION("predicate") {
            machine.conditions[0].predicate = static_cast<StateMachinePredicate>(99);
        }
        CHECK(ValidateStateMachine(machine.Plan(), machine.declarations) != GraphInstanceFailure::None);
    }

    TEST_CASE("Animation state machine admission preserves ordered failures and bounded remaining condition storage",
              "[animation][state_machine]") {
        Machine machine;
        SECTION("transition binding precedes malformed policy") {
            machine.transitions[0].source = 99;
            machine.transitions[0].duration = {-1};
            CHECK(ValidateStateMachine(machine.Plan(), machine.declarations) == GraphInstanceFailure::BindingMismatch);
        }
        SECTION("condition type precedes nonfinite release threshold") {
            machine.conditions[0].threshold = true;
            machine.conditions[0].releaseThreshold = std::numeric_limits<float>::quiet_NaN();
            CHECK(ValidateStateMachine(machine.Plan(), machine.declarations) == GraphInstanceFailure::TypeMismatch);
        }
        SECTION("later condition interval cannot exceed the admitted remaining table") {
            auto next = machine.transitions[0];
            next.id = {101};
            next.firstCondition = 1;
            next.conditions = std::numeric_limits<std::uint32_t>::max();
            machine.transitions.push_back(next);
            CHECK(ValidateStateMachine(machine.Plan(), machine.declarations) == GraphInstanceFailure::Malformed);
        }
    }
}  // namespace Horo::Animation::Test
