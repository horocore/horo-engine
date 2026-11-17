#include "../../../support/AllocationProbe.h"
#include "AnimationActionTestFixtures.h"
#include "AnimationStateGraphTestFixtures.h"

#include <array>
#include <catch2/catch_template_test_macros.hpp>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace {
    using namespace Horo::Animation;
    using Horo::Tests::AllocationProbe::ScopedFailure;
    using Horo::Tests::AllocationProbe::ScopedMeasurement;

    /** @brief Supplies detached value contents; programs come from the real compiler. */
    template <typename T> T Value() {
        if constexpr (std::is_same_v<T, AnimationGraphProgram>) {
            auto graph = Test::Actions::Data();
            const auto stateGraph = Test::StateGraph::Data();
            graph.parameters = stateGraph.parameters;
            graph.definitions.front().nodes.front().payload = stateGraph.definitions.front().nodes.front().payload;
            const Test::Actions::Prepared prepared(std::move(graph));
            const auto &program = *prepared.program;
            REQUIRE_FALSE(program.Occurrences().empty());
            REQUIRE_FALSE(program.StateMachines().empty());
            REQUIRE_FALSE(program.ClipBindings().empty());
            REQUIRE_FALSE(program.Parameters().empty());
            REQUIRE_FALSE(program.ActionSlots().empty());
            REQUIRE_FALSE(program.Actions().empty());
            REQUIRE_FALSE(program.Definitions().empty());
            REQUIRE_FALSE(program.Clips().empty());
            return program;
        } else {
            T value;
            if constexpr (std::is_same_v<T, AnimationGraphData> || std::is_same_v<T, AnimationGraphCookedData>) {
                value.parameters.emplace_back();
                value.parameters.front().name = "a detached parameter exceeding small string storage";
                value.definitions.emplace_back();
                value.actionSlots.emplace_back();
                value.actions.emplace_back();
            } else if constexpr (std::is_same_v<T, GraphCompiledDefinition>) {
                value.inputs.emplace_back();
                value.instructions.emplace_back();
            } else if constexpr (std::is_same_v<T, GraphInstructionOccurrence>) {
                value.callPath.emplace_back();
                value.inputs.emplace_back();
                value.guards.emplace_back();
                value.jointMask = {0.25F, 0.75F};
                value.clipCoverage = {1, 0, 1};
                value.blendSamples.emplace_back();
            } else if constexpr (std::is_same_v<T, GraphBlendSampleBinding>) {
                value.coverage = {1, 0, 1};
            } else {
                value.branches.emplace_back();
            }
            return value;
        }
    }

    /** @brief Compares cooked declaration contents without adding a public equality contract. */
    template <typename T> bool SameValue(const T &left, const T &right) {
        if constexpr (std::is_same_v<T, AnimationGraphCookedData>) {
            return left.contractVersion == right.contractVersion && left.id == right.id && left.skeleton == right.skeleton &&
                   left.entry == right.entry && left.parameters == right.parameters && left.actionSlots == right.actionSlots &&
                   left.actions == right.actions && left.definitions == right.definitions;
        } else {
            return left == right;
        }
    }

    /** @brief Observes public buffer identity without allocating or exposing private storage. */
    template <typename T> auto Buffers(const T &value) {
        if constexpr (std::is_same_v<T, AnimationGraphData> || std::is_same_v<T, AnimationGraphCookedData>)
            return std::array<const void *, 4>{value.parameters.data(), value.definitions.data(), value.actionSlots.data(),
                                               value.actions.data()};
        else if constexpr (std::is_same_v<T, GraphCompiledDefinition>)
            return std::array<const void *, 2>{value.inputs.data(), value.instructions.data()};
        else if constexpr (std::is_same_v<T, GraphInstructionOccurrence>)
            return std::array<const void *, 6>{value.callPath.data(),  value.inputs.data(),       value.guards.data(),
                                               value.jointMask.data(), value.clipCoverage.data(), value.blendSamples.data()};
        else if constexpr (std::is_same_v<T, GraphBlendSampleBinding>)
            return std::array<const void *, 1>{value.coverage.data()};
        else if constexpr (std::is_same_v<T, GraphStateMachineBinding>)
            return std::array<const void *, 1>{value.branches.data()};
        else
            return std::array<const void *, 8>{value.Occurrences().data(), value.StateMachines().data(), value.ClipBindings().data(),
                                               value.Parameters().data(),  value.ActionSlots().data(),   value.Actions().data(),
                                               value.Definitions().data(), value.Clips().data()};
    }
}  // namespace

TEST_CASE("Graph source text result construction propagates every allocation failure", "[animation][allocation][ownership]") {
    const std::string source(4096, 'x');
    const auto *sourceBuffer = source.data();
    using R = Horo::Result<std::string>;
    const auto published = R::SuccessInPlace(source.data(), source.size());
    REQUIRE(published.HasValue());
    REQUIRE(published.Value() == source);
    REQUIRE(published.Value().data() != sourceBuffer);
    std::size_t requests{};
    {
        ScopedMeasurement capture;
        const auto measured = R::SuccessInPlace(source.data(), source.size());
        requests = capture.Snapshot().requests;
    }
    REQUIRE(requests > 0);
    for (std::size_t index = 0; index < requests; ++index) {
        INFO(index);
        bool failed{};
        bool completed{};
        {
            ScopedFailure injected(index);
            try {
                const auto result = R::SuccessInPlace(source.data(), source.size());
                completed = true;
            } catch (const std::bad_alloc &) {
                failed = true;
            }
        }
        REQUIRE(failed);
        CHECK_FALSE(completed);
        CHECK(source.data() == sourceBuffer);
        CHECK(source == published.Value());
    }
}

TEMPLATE_TEST_CASE("Graph owned value move failure preserves source contents and buffer identity", "[animation][allocation][ownership]",
                   Horo::Animation::AnimationGraphData, Horo::Animation::GraphCompiledDefinition, Horo::Animation::AnimationGraphCookedData,
                   Horo::Animation::AnimationGraphProgram, Horo::Animation::GraphInstructionOccurrence,
                   Horo::Animation::GraphBlendSampleBinding, Horo::Animation::GraphStateMachineBinding) {
    static_assert(!std::is_nothrow_move_constructible_v<TestType>);
    const auto reference = Value<TestType>();
    auto measuredSource = reference;
    const auto expectedBuffers = Buffers(measuredSource);
    std::optional<TestType> moved;
    std::size_t requests{};
    {
        ScopedMeasurement capture;
        moved.emplace(std::move(measuredSource));
        requests = capture.Snapshot().requests;
    }
    REQUIRE(moved.has_value());
    CHECK(SameValue(*moved, reference));
    CHECK(Buffers(*moved) == expectedBuffers);
#if defined(_MSVC_STL_VERSION) && _ITERATOR_DEBUG_LEVEL != 0
    REQUIRE(requests > 0);
#endif
    for (std::size_t index = 0; index < requests; ++index) {
        INFO(index);
        auto source = reference;
        const auto sourceBuffers = Buffers(source);
        std::optional<TestType> destination;
        bool failed{};
        {
            ScopedFailure injected(index);
            try {
                destination.emplace(std::move(source));
            } catch (const std::bad_alloc &) {
                failed = true;
            }
        }
        REQUIRE(failed);
        CHECK_FALSE(destination.has_value());
        CHECK(SameValue(source, reference));
        CHECK(Buffers(source) == sourceBuffers);
    }
}

TEMPLATE_TEST_CASE("Graph result transfer propagates checked allocation failures without changing the retained value",
                   "[animation][allocation][ownership]", Horo::Animation::AnimationGraphData, Horo::Animation::GraphCompiledDefinition,
                   Horo::Animation::AnimationGraphCookedData, Horo::Animation::AnimationGraphProgram,
                   Horo::Animation::GraphInstructionOccurrence, Horo::Animation::GraphBlendSampleBinding,
                   Horo::Animation::GraphStateMachineBinding) {
    using R = Horo::Result<TestType>;
    const auto retained = Value<TestType>();
    const auto retainedBuffers = Buffers(retained);
    auto source = retained;
    const auto expectedBuffers = Buffers(source);
    std::optional<R> measured;
    std::size_t requests{};
    {
        ScopedMeasurement capture;
        measured.emplace(R::Success(std::move(source)));
        requests = capture.Snapshot().requests;
    }
    REQUIRE(measured.has_value());
    REQUIRE(measured->HasValue());
    CHECK(SameValue(measured->Value(), retained));
    CHECK(Buffers(measured->Value()) == expectedBuffers);
#if defined(_MSVC_STL_VERSION) && _ITERATOR_DEBUG_LEVEL != 0
    REQUIRE(requests > 0);
#endif
    for (std::size_t index = 0; index < requests; ++index) {
        INFO(index);
        auto detached = retained;
        std::optional<R> destination;
        bool failed{};
        {
            ScopedFailure injected(index);
            try {
                destination.emplace(R::Success(std::move(detached)));
            } catch (const std::bad_alloc &) {
                failed = true;
            }
        }
        REQUIRE(failed);
        CHECK_FALSE(destination.has_value());
        CHECK(SameValue(retained, measured->Value()));
        CHECK(Buffers(retained) == retainedBuffers);
    }
}

TEMPLATE_TEST_CASE("Graph owned value move assignment transfers existing buffers without allocation", "[animation][allocation][ownership]",
                   Horo::Animation::AnimationGraphData, Horo::Animation::GraphCompiledDefinition, Horo::Animation::AnimationGraphCookedData,
                   Horo::Animation::AnimationGraphProgram, Horo::Animation::GraphInstructionOccurrence,
                   Horo::Animation::GraphBlendSampleBinding, Horo::Animation::GraphStateMachineBinding) {
    static_assert(std::is_nothrow_move_assignable_v<TestType>);
    auto source = Value<TestType>();
    const auto expected = source;
    const auto sourceBuffers = Buffers(source);
    auto destination = Value<TestType>();
    {
        ScopedFailure denied(0);
        destination = std::move(source);
    }
    CHECK(SameValue(destination, expected));
    CHECK(Buffers(destination) == sourceBuffers);
}
