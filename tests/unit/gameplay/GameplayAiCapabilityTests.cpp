#include "../../support/AllocationProbe.h"
#include "GameplayAiTestSupport.h"

#include <limits>
#include <thread>

namespace Horo::Gameplay {
    using namespace AiTestSupport;

    TEST_CASE("Gameplay AI declarations freeze inert bounded stable task metadata", "[gameplay][ai][capability]") {
        AI::AiCapabilityDescriptorRegistry registry;
        ExpectError(registry.Register({}), AI::AIErrors::TaskContextInvalid);
        for (std::size_t i = 0; i < AI::MaximumAiCapabilityTasks; ++i)
            REQUIRE(registry.Register({MakeIdentity<AI::TaskId>(AI::MaximumAiCapabilityTasks - i), {}, AI::AiTaskAuthoring::Native})
                        .HasValue());
        ExpectError(registry.Register({MakeIdentity<AI::TaskId>(999)}), AI::AIErrors::DescriptorLimitExceeded);
        auto frozen = registry.Freeze();
        REQUIRE(frozen.Tasks().size() == AI::MaximumAiCapabilityTasks);
        CHECK(frozen.Tasks().front().task.Value() == 1);
        ExpectError(registry.Register({MakeIdentity<AI::TaskId>(999)}), AI::AIErrors::TaskContextInvalid);
        CHECK(registry.Freeze().Tasks().size() == frozen.Tasks().size());
    }

    TEST_CASE("Gameplay AI native and script task declarations use the same exact lifecycle", "[gameplay][ai][capability]") {
        Fixture fixture;
        for (const std::uint64_t definition : {41, 42}) {
            Telemetry::OperationContext diagnostics;
            diagnostics.operationId = 123;
            auto started = fixture.context->StartTask(fixture.agent, MakeIdentity<AI::TaskId>(definition), diagnostics);
            REQUIRE(started.HasValue());
            ExpectError(fixture.context->StartTask(fixture.agent, MakeIdentity<AI::TaskId>(definition)),
                        AI::AIErrors::TaskTransitionInvalid);
            auto running = fixture.context->InspectTask(fixture.agent, started.Value());
            REQUIRE(running.HasValue());
            CHECK(running.Value().state == AI::AiTaskState::Running);
            CHECK(running.Value().diagnostics.operationId == 123);
            REQUIRE(fixture.context->CompleteTask(fixture.agent, started.Value()).HasValue());
            CHECK(fixture.context->InspectTask(fixture.agent, started.Value()).Value().state == AI::AiTaskState::Succeeded);
            REQUIRE(fixture.context->ReleaseTask(fixture.agent, started.Value()).HasValue());
            ExpectError(fixture.context->InspectTask(fixture.agent, started.Value()), AI::AIErrors::HandleInvalid);
        }
        ExpectError(fixture.context->StartTask(fixture.agent, MakeIdentity<AI::TaskId>(99)), AI::AIErrors::CapabilityUnavailable);
    }

    TEST_CASE("Gameplay AI contexts cannot complete another module scope execution", "[gameplay][ai][capability]") {
        Fixture fixture;
        auto other =
            GameplayAiContext::Create({"com.other.module", true, true}, fixture.runtime.CapabilityAtSafePoint(), fixture.declarations);
        REQUIRE(other.HasValue());
        auto task = fixture.context->StartTask(fixture.agent, MakeIdentity<AI::TaskId>(41));
        REQUIRE(task.HasValue());
        ExpectError(other.Value()->CompleteTask(fixture.agent, task.Value()), AI::AIErrors::HandleInvalid);
        REQUIRE(fixture.context->CancelTask(fixture.agent, task.Value()).HasValue());
        auto terminal = fixture.context->InspectTask(fixture.agent, task.Value());
        REQUIRE(terminal.HasValue());
        CHECK(terminal.Value().state == AI::AiTaskState::Cancelled);
        CHECK(fixture.context->CompleteTask(fixture.agent, task.Value()).Value() == AI::AiTaskTransitionDisposition::AlreadyTerminal);
    }

    TEST_CASE("Gameplay AI revoked and missing capability remains recoverable before runtime access", "[gameplay][ai][capability]") {
        Fixture fixture;
        GameRuntimeContext absent;
        ExpectError(AcquireGameplayAi(absent), AI::AIErrors::CapabilityUnavailable);
        ExpectError(GameplayAiContext::Create({"com.example.module", true, false}, fixture.runtime.CapabilityAtSafePoint()),
                    AI::AIErrors::CapabilityUnavailable);
        ExpectError(GameplayAiContext::Create({"com.example.module", true, true}, {}), AI::AIErrors::RuntimeUnavailable);
        auto task = fixture.context->StartTask(fixture.agent, MakeIdentity<AI::TaskId>(41));
        REQUIRE(task.HasValue());
        fixture.context->Revoke();
        ExpectError(fixture.context->ResolveAgent(fixture.scene.agent.agent.agent), AI::AIErrors::CapabilityUnavailable);
        GameRuntimeContext revoked;
        revoked.ai = fixture.context;
        ExpectError(AcquireGameplayAi(revoked), AI::AIErrors::CapabilityUnavailable);
        auto observed = fixture.runtime.CapabilityAtSafePoint().InspectTask(fixture.agent, task.Value());
        REQUIRE(observed.HasValue());
        CHECK(observed.Value().state == AI::AiTaskState::Cancelled);
        CHECK(observed.Value().terminal->cancellationReason == AI::AiTaskCancellationReason::ContextCancelled);
    }

    TEST_CASE("Gameplay AI blackboard writes preserve atomic schema and revision authority", "[gameplay][ai][capability]") {
        Fixture fixture;
        const auto key = MakeIdentity<AI::BlackboardKeyId>(71);
        auto firstResult = fixture.context->BeginBlackboardWrite(fixture.agent);
        auto staleResult = fixture.context->BeginBlackboardWrite(fixture.agent);
        REQUIRE(firstResult.HasValue());
        REQUIRE(staleResult.HasValue());
        auto first = std::move(firstResult).Value();
        auto stale = std::move(staleResult).Value();
        REQUIRE(first.Stage({key, AI::BlackboardScalarValue{true}, {}}).HasValue());
        REQUIRE(fixture.context->CommitBlackboardWrite(fixture.agent, std::move(first)).HasValue());
        auto value = fixture.context->ReadBlackboard(fixture.agent, key);
        REQUIRE(value.HasValue());
        REQUIRE(value.Value());
        CHECK(std::get<bool>(std::get<AI::BlackboardScalarValue>(*value.Value())));
        REQUIRE(stale.Stage({key, AI::BlackboardScalarValue{false}, {}}).HasValue());
        ExpectError(fixture.context->CommitBlackboardWrite(fixture.agent, std::move(stale)), AI::AIErrors::BlackboardInstanceStale);
        auto wrongResult = fixture.context->BeginBlackboardWrite(fixture.agent);
        REQUIRE(wrongResult.HasValue());
        auto wrong = std::move(wrongResult).Value();
        REQUIRE(wrong.Stage({key, AI::BlackboardScalarValue{std::int64_t{3}}, {}}).HasValue());
        CHECK(fixture.context->CommitBlackboardWrite(fixture.agent, std::move(wrong)).HasError());
        CHECK(std::get<bool>(std::get<AI::BlackboardScalarValue>(*fixture.context->ReadBlackboard(fixture.agent, key).Value())));
    }

    TEST_CASE("Gameplay AI publication replacement disable and foreign threads fence retained clients", "[gameplay][ai][capability]") {
        Fixture fixture;
        auto old = fixture.runtime.CapabilityAtSafePoint();
        bool denied{};
        std::thread foreign([&] {
            denied = fixture.context->ResolveAgent(fixture.scene.agent.agent.agent).HasError();
        });
        foreign.join();
        CHECK(denied);
        auto replacement = AI::TestSupport::MakeFixture(1, 11, 8, 0);
        auto published = AI::TestSupport::Publish(fixture.runtime, replacement);
        ExpectError(old.ResolveAgent(fixture.scene.agent.agent.agent), AI::AIErrors::RuntimeUnavailable);
        auto current = fixture.runtime.CapabilityAtSafePoint().ResolveAgent(replacement.agent.agent.agent);
        REQUIRE(current.HasValue());
        REQUIRE(fixture.runtime.DisableAtSafePoint(current.Value()).HasValue());
        ExpectError(fixture.runtime.CapabilityAtSafePoint().ResolveAgent(replacement.agent.agent.agent), AI::AIErrors::HandleInvalid);
        fixture.runtime.BeginShutdown();
        ExpectError(fixture.runtime.CapabilityAtSafePoint().ValidateAtSafePoint(), AI::AIErrors::RuntimeUnavailable);
    }

    TEST_CASE("Gameplay AI failed task allocation publishes neither execution nor module ownership",
              "[gameplay][ai][capability][allocation]") {
        Fixture fixture;
        const auto definition = MakeIdentity<AI::TaskId>(41);
        Telemetry::OperationContext diagnostics;
        std::size_t allocations{};
        std::optional<AI::TaskHandle> measuredTask;
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            auto started = fixture.context->StartTask(fixture.agent, definition, diagnostics);
            if (started.HasValue())
                measuredTask = started.Value();
            allocations = measurement.Snapshot().requests;
        }
        REQUIRE(measuredTask);
        REQUIRE(allocations > 0);
        REQUIRE(fixture.context->CancelTask(fixture.agent, *measuredTask).HasValue());
        REQUIRE(fixture.context->ReleaseTask(fixture.agent, *measuredTask).HasValue());
        for (std::size_t offset = 0; offset < allocations; ++offset) {
            bool rejected{};
            {
                Tests::AllocationProbe::ScopedFailure failure{offset};
                auto started = fixture.context->StartTask(fixture.agent, definition, diagnostics);
                rejected = started.HasError();
            }
            CHECK(rejected);
            CHECK_FALSE(fixture.runtime.Find(fixture.agent).Value().hasRunningTask);
            auto retry = fixture.context->StartTask(fixture.agent, definition, diagnostics);
            REQUIRE(retry.HasValue());
            REQUIRE(fixture.context->CancelTask(fixture.agent, retry.Value()).HasValue());
            REQUIRE(fixture.context->ReleaseTask(fixture.agent, retry.Value()).HasValue());
        }
    }
}  // namespace Horo::Gameplay
