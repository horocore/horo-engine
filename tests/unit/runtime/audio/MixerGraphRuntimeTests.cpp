#include "MixerTestFixture.h"

using namespace Horo::Tests::MixerFixture;

TEST_CASE("Mixer stale queued or malformed swaps preserve last good complete generation", "[audio][mixer]") {
    auto runtime = Runtime();
    auto staging = Staging();
    auto first = Plan(Asset());
    Block input, output;
    input.Fill(0.25F);
    std::array<MixerVoiceInput, 1> voices{Voice(*first, input)};
    AudioCommandRecord old = Publish(*runtime, staging, first);
    REQUIRE(runtime->Render(Scope, &old, voices, output.View()).generation == 1);
    REQUIRE(runtime->Reconcile().generation == 1);
    auto candidate = Plan(Asset(), 2);
    MixerPlanIdentity stale = candidate->Identity();
    ++stale.assetRevision;
    REQUIRE(runtime->Publish(candidate, stale, staging).status == AudioCommandStagingStatus::InvalidCommand);
    REQUIRE(candidate != nullptr);
    const AudioCommandRecord next = Publish(*runtime, staging, candidate);
    auto third = Plan(Asset(), 3);
    REQUIRE(runtime->Publish(third, third->Identity(), staging).status == AudioCommandStagingStatus::Busy);
    REQUIRE(runtime->Reconcile().generation == 1);
    AudioCommandRecord malformed = next;
    ++std::get<AudioSwapGraphCommand>(malformed.command.payload).storage.generation;
    REQUIRE(runtime->Render(Scope, &malformed, voices, output.View()).status == MixerRenderStatus::InvalidCommand);
    REQUIRE(output.left.front() == 0.25F);
    voices[0].graphGeneration = 2;
    REQUIRE(runtime->Render(Scope, &next, voices, output.View()).generation == 2);
    REQUIRE(runtime->Reconcile().generation == 2);
    REQUIRE(runtime->Render(Scope, &old, voices, output.View()).status == MixerRenderStatus::InvalidCommand);
    auto foreign = Scope;
    ++foreign.epoch;
    REQUIRE(runtime->Render(foreign, nullptr, voices, output.View()).status == MixerRenderStatus::InvalidEpoch);
    REQUIRE(output.left.front() == 0);
    ShutDown(*runtime, output);
}

TEST_CASE("Mixer saturated publication retains candidate ownership for an ordered retry", "[audio][mixer]") {
    auto staging = Staging();
    const AudioVoiceHandle voice{Owner, 1, 1};
    for (std::uint32_t i = 0; i < 3; ++i)
        REQUIRE(staging.Submit({Scope, AudioStartVoiceCommand{voice}}).status == AudioCommandStagingStatus::Ok);
    auto runtime = Runtime();
    auto plan = Plan(Asset());
    REQUIRE(runtime->Publish(plan, plan->Identity(), staging).status == AudioCommandStagingStatus::OrdinaryFull);
    REQUIRE(plan != nullptr);
    REQUIRE(staging.Pump(3).published == 3);
    AudioCommandRecord record;
    for (std::uint32_t i = 0; i < 3; ++i)
        REQUIRE(staging.TryConsume(record));
    const AudioCommandRecord swap = Publish(*runtime, staging, plan);
    Block output;
    REQUIRE(runtime->Render(Scope, &swap, {}, output.View()).status == MixerRenderStatus::Rendered);
    ShutDown(*runtime, output);
}

TEST_CASE("Mixer replacement cannot reclaim a DSP lease while its completed-block acknowledgement is absent", "[audio][mixer]") {
    Probe firstProbe, secondProbe;
    Factory firstFactory(firstProbe), secondFactory(secondProbe);
    auto first = Plan(Asset(true), 1, &firstFactory);
    auto second = Plan(Asset(true), 2, &secondFactory);
    auto runtime = Runtime();
    auto staging = Staging();
    Block output;
    const AudioCommandRecord initial = Publish(*runtime, staging, first);
    REQUIRE(runtime->Render(Scope, &initial, {}, output.View()).status == MixerRenderStatus::Rendered);
    REQUIRE(runtime->Reconcile().generation == 1);
    const AudioCommandRecord replacement = Publish(*runtime, staging, second);
    std::latch entered(1), leave(1);
    secondProbe.entered = &entered;
    secondProbe.leave = &leave;
    std::thread callback([&] {
        secondProbe.callback = true;
        (void)runtime->Render(Scope, &replacement, {}, output.View());
        secondProbe.callback = false;
    });
    entered.wait();
    REQUIRE(runtime->Reconcile().generation == 1);
    REQUIRE(firstProbe.destroyed.load() == 0);
    REQUIRE(secondProbe.destroyed.load() == 0);
    runtime->Close();
    REQUIRE(runtime->CompleteShutdown(Scope, false) == MixerRenderStatus::InvalidCommand);
    REQUIRE(firstProbe.destroyed.load() == 0);
    REQUIRE(secondProbe.destroyed.load() == 0);
    leave.count_down();
    callback.join();
    secondProbe.entered = nullptr;
    REQUIRE(runtime->Reconcile().generation == 2);
    REQUIRE(firstProbe.destroyed.load() == 1);
    REQUIRE(secondProbe.destroyedOnCallback.load() == 0);
    runtime->Close();
    REQUIRE(runtime->CompleteShutdown(Scope, false) == MixerRenderStatus::InvalidCommand);
    REQUIRE(runtime->Render(Scope, nullptr, {}, output.View()).status == MixerRenderStatus::Quiesced);
    REQUIRE(runtime->Reconcile().status == MixerRenderStatus::Quiesced);
    REQUIRE(secondProbe.destroyed.load() == 0);
    REQUIRE(runtime->CompleteShutdown(Scope, false) == MixerRenderStatus::InvalidCommand);
    REQUIRE(runtime->CompleteShutdown(Scope, true) == MixerRenderStatus::Quiesced);
    REQUIRE(runtime->CompleteShutdown(Scope, true) == MixerRenderStatus::Quiesced);
    REQUIRE(secondProbe.destroyed.load() == 1);
}

TEST_CASE("Mixer runtime rejects mismatched profiles exhausted retained budgets and closed publication", "[audio][mixer]") {
    auto candidate = Plan(Asset());
    auto staging = Staging();
    auto runtime = Runtime(Profile(), candidate->StorageBytes());
    const AudioCommandRecord first = Publish(*runtime, staging, candidate);
    Block output;
    REQUIRE(runtime->Render(Scope, &first, {}, output.View()).status == MixerRenderStatus::Rendered);
    REQUIRE(runtime->Reconcile().generation == 1);
    candidate = Plan(Asset(), 2);
    REQUIRE(runtime->Publish(candidate, candidate->Identity(), staging).status == AudioCommandStagingStatus::OrdinaryFull);
    REQUIRE(candidate != nullptr);
    MixerCompileProfile changed = Profile();
    ++changed.maximumVoices;
    candidate = Plan(Asset(), 3, nullptr, changed);
    REQUIRE(runtime->Publish(candidate, candidate->Identity(), staging).status == AudioCommandStagingStatus::InvalidCommand);
    auto malformedOutput = output.View();
    malformedOutput.planes = {};
    REQUIRE(runtime->Render(Scope, nullptr, {}, malformedOutput).status == MixerRenderStatus::InvalidBuffer);
    auto foreign = Scope;
    ++foreign.epoch;
    runtime->Close();
    REQUIRE(runtime->Publish(candidate, candidate->Identity(), staging).status == AudioCommandStagingStatus::Closed);
    REQUIRE(runtime->CompleteShutdown(foreign, true) == MixerRenderStatus::InvalidEpoch);
    REQUIRE(runtime->CompleteShutdown(Scope, false) == MixerRenderStatus::InvalidCommand);
    ShutDown(*runtime, output);
}

TEST_CASE("Mixer zero graph partial startup and cancelled queued generation shut down with detached proof", "[audio][mixer]") {
    Probe probe;
    Factory factory(probe);
    auto runtime = Runtime();
    auto staging = Staging();
    Block output;
    REQUIRE(runtime->Render(Scope, nullptr, {}, output.View()).status == MixerRenderStatus::Silence);
    auto plan = Plan(Asset(true), 1, &factory);
    const auto identity = plan->Identity();
    REQUIRE(runtime->Publish(plan, identity, staging).status == AudioCommandStagingStatus::Ok);
    runtime->Close();
    REQUIRE(runtime->Render(Scope, nullptr, {}, output.View()).status == MixerRenderStatus::Quiesced);
    REQUIRE(probe.destroyed.load() == 0);
    REQUIRE(runtime->CompleteShutdown(Scope, false) == MixerRenderStatus::InvalidCommand);
    REQUIRE(ConsumePublished(staging).command.scope == Scope);
    REQUIRE(runtime->CompleteShutdown(Scope, true) == MixerRenderStatus::Quiesced);
    REQUIRE(probe.destroyed.load() == 1);
    auto neverAttached = Runtime();
    neverAttached->Close();
    REQUIRE(neverAttached->CompleteShutdown(Scope, true) == MixerRenderStatus::Quiesced);
}

TEST_CASE("Mixer real staging consumer exchanges repeated generations while control reclaims completed blocks", "[audio][mixer]") {
    auto runtime = Runtime();
    auto staging = Staging();
    Probe probe;
    Factory factory(probe);
    std::atomic<bool> failed{};
    std::jthread callback([&](const std::stop_token stop) {
        Block output;
        AudioCommandRecord record;
        std::uint64_t previous{};
        while (!stop.stop_requested()) {
            if (staging.TryConsume(record)) {
                const MixerRenderResult result = runtime->Render(Scope, &record, {}, output.View());
                if (result.status != MixerRenderStatus::Rendered || result.generation <= previous)
                    failed = true;
                previous = result.generation;
            } else {
                (void)runtime->Render(Scope, nullptr, {}, output.View());
                std::this_thread::yield();
            }
        }
    });
    constexpr std::uint64_t Generations = 500;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (std::uint64_t generation = 1; generation <= Generations; ++generation) {
        auto plan = Plan(Asset(true), generation, &factory);
        const auto identity = plan->Identity();
        REQUIRE(runtime->Publish(plan, identity, staging).status == AudioCommandStagingStatus::Ok);
        REQUIRE(staging.Pump(1).published == 1);
        while (runtime->Reconcile().generation != generation && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        REQUIRE(runtime->Reconcile().generation == generation);
    }
    runtime->Close();
    while (runtime->Reconcile().status != MixerRenderStatus::Quiesced && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    callback.request_stop();
    callback.join();
    REQUIRE_FALSE(failed.load());
    REQUIRE(probe.destroyed.load() == Generations - 1);
    REQUIRE(runtime->CompleteShutdown(Scope, true) == MixerRenderStatus::Quiesced);
    REQUIRE(probe.destroyed.load() == Generations);
}

TEST_CASE("Mixer joined native callback permits detached cleanup without a final graph render", "[audio][mixer]") {
    Probe probe;
    Factory factory(probe);
    auto active = Plan(Asset(true), 1, &factory);
    auto runtime = Runtime();
    auto staging = Staging();
    const AudioCommandRecord first = Publish(*runtime, staging, active);
    Block output;
    MixerRenderResult completed;
    std::thread callback([&] {
        probe.callback = true;
        completed = runtime->Render(Scope, &first, {}, output.View());
        probe.callback = false;
    });
    callback.join();  // Actual stopped reader proof precedes the host's backendDetached=true handoff.
    REQUIRE(completed.status == MixerRenderStatus::Rendered);
    REQUIRE(runtime->Reconcile().generation == 1);
    auto pending = Plan(Asset(true), 2, &factory);
    const auto pendingIdentity = pending->Identity();
    REQUIRE(runtime->Publish(pending, pendingIdentity, staging).status == AudioCommandStagingStatus::Ok);
    REQUIRE(runtime->CompleteShutdown(Scope, true) == MixerRenderStatus::InvalidCommand);
    REQUIRE(probe.destroyed.load() == 0);
    runtime->Close();
    REQUIRE(runtime->CompleteShutdown(Scope, false) == MixerRenderStatus::InvalidCommand);
    auto foreign = Scope;
    ++foreign.epoch;
    REQUIRE(runtime->CompleteShutdown(foreign, true) == MixerRenderStatus::InvalidEpoch);
    REQUIRE(probe.destroyed.load() == 0);
    REQUIRE(ConsumePublished(staging).command.scope == Scope);
    REQUIRE(runtime->CompleteShutdown(Scope, true) == MixerRenderStatus::Quiesced);
    REQUIRE(probe.destroyed.load() == 2);
    REQUIRE(probe.destroyedOnCallback.load() == 0);
    REQUIRE(runtime->Reconcile().status == MixerRenderStatus::Quiesced);
    REQUIRE(runtime->CompleteShutdown(Scope, true) == MixerRenderStatus::Quiesced);
}
