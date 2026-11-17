#include "AudioCommandTestProbe.h"
#include "Horo/Audio/AudioCommandStaging.h"
#include "Horo/Audio/Internal/AudioPlaybackEpoch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <optional>
#include <ranges>
#include <thread>
#include <utility>

namespace Horo::Audio {
    namespace {
        AudioRuntimeId Owner() {
            return AudioRuntimeId::Create(17).Value();
        }

        AudioSceneContextHandle Scene(const std::uint32_t slot = 1, const std::uint32_t generation = 1) {
            return {.owner = Owner(), .slot = slot, .generation = generation};
        }

        AudioCommandStagingDescriptor Descriptor() {
            return {.callback = {.owner = Owner(),
                                 .storageIdentity = AudioMemoryPoolId::Create(21).Value(),
                                 .epoch = 5,
                                 .slots = 4,
                                 .criticalSlots = 2,
                                 .budgetBytes = 4096},
                    .ingressIdentity = AudioMemoryPoolId::Create(22).Value(),
                    .ingressSlots = 4,
                    .criticalSlots = 2,
                    .sceneSlots = 4,
                    .ingressBudgetBytes = 4096};
        }

        AudioCommandStaging Staging() {
            auto result = AudioCommandStaging::Create(Descriptor());
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        AudioCommand Start(const AudioSceneContextHandle scene = Scene(), const std::uint32_t voiceSlot = 2) {
            return {.scope = {.owner = Owner(), .epoch = 5, .scene = scene},
                    .payload = AudioStartVoiceCommand{.voice = {.owner = Owner(), .slot = voiceSlot, .generation = 1}}};
        }

        AudioCommand Stop(const AudioSceneContextHandle scene = Scene()) {
            auto command = Start(scene);
            command.payload = AudioStopVoiceCommand{.voice = {.owner = Owner(), .slot = 2, .generation = 1}};
            return command;
        }

        AudioCommand Parameter(const float value) {
            auto command = Start();
            command.payload = AudioSetParameterCommand{.voice = {.owner = Owner(), .slot = 2, .generation = 1},
                                                       .parameter = AudioParameterId::Create(3).Value(),
                                                       .value = value};
            return command;
        }

        /** @brief Explicit fresh pool reservations retain dimensions while changing only the callback epoch and storage identities. */
        AudioCommandStagingDescriptor ReplacementDescriptor() {
            auto descriptor = Descriptor();
            descriptor.callback.epoch = 6;
            descriptor.callback.storageIdentity = AudioMemoryPoolId::Create(31).Value();
            descriptor.ingressIdentity = AudioMemoryPoolId::Create(32).Value();
            return descriptor;
        }

        /** @brief This fixture never attaches participants or external resources; drain normalized records as explicit fixture
         * cancellation. */
        struct RetireDetachedTransport final {
            AudioCommandStaging &transport;

            ~RetireDetachedTransport() {
                (void)transport.Close();
                AudioCommandRecord record;
                for (std::size_t step = 0; step <= MaximumAudioCommandSlots && !transport.IsDrained(); ++step) {
                    (void)transport.Pump(64);
                    while (transport.TryConsume(record)) {
                    }
                }
            }
        };

        struct JoinWorkers {
            std::span<std::thread> workers;
            std::atomic<bool> &stop;

            ~JoinWorkers() {
                stop.store(true);
                std::ranges::for_each(workers, [](std::thread &worker) {
                    if (worker.joinable()) {
                        worker.join();
                    }
                });
            }
        };

        struct ConcurrentRun {
            static constexpr std::size_t ProducerCount = 4;
            static constexpr std::uint32_t PerProducer = 3000;
            AudioCommandStaging &staging;
            std::atomic<bool> stop{};
            std::atomic<bool> consumerDone{};
            std::atomic<std::uint32_t> producersDone{};
            std::array<std::uint32_t, ProducerCount> sent{};
            std::array<std::uint32_t, ProducerCount> received{};
            bool incorrect{};
            std::size_t callbackAllocations{};
            std::size_t callbackFrees{};
            const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            bool closed{};

            void Produce(const std::size_t producer) {
                const auto command = Start(Scene(), static_cast<std::uint32_t>(producer + 10));
                while (
                    std::ranges::all_of(std::array{!stop.load(), sent[producer] < PerProducer, std::chrono::steady_clock::now() < deadline},
                                        std::identity{})) {
                    sent[producer] += static_cast<std::uint32_t>(staging.Submit(command).status == AudioCommandStagingStatus::Ok);
                    std::this_thread::yield();
                }
                producersDone.fetch_add(1);
            }

            void Consume() {
                const auto allocated = Test::allocationCount;
                const auto freed = Test::deallocationCount;
                std::uint64_t sequence{};
                AudioCommandRecord record;
                while (std::ranges::all_of(std::array{!stop.load(), !staging.IsDrained(), std::chrono::steady_clock::now() < deadline},
                                           std::identity{})) {
                    if (!staging.TryConsume(record)) {
                        std::this_thread::yield();
                        continue;
                    }
                    incorrect |= record.sequence != ++sequence;
                    const auto *command = std::get_if<AudioStartVoiceCommand>(&record.command.payload);
                    if (!command) {
                        incorrect = true;
                        continue;
                    }
                    const std::array validSlot{command->voice.slot >= 10, command->voice.slot < 10 + ProducerCount};
                    if (!std::ranges::all_of(validSlot, std::identity{})) {
                        incorrect = true;
                        continue;
                    }
                    ++received[command->voice.slot - 10];
                }
                callbackAllocations = Test::allocationCount - allocated;
                callbackFrees = Test::deallocationCount - freed;
                consumerDone.store(true);
            }

            void Pump() {
                while (
                    std::ranges::all_of(std::array{!consumerDone.load(), std::chrono::steady_clock::now() < deadline}, std::identity{})) {
                    static_cast<void>(staging.Pump(8));
                    const std::array readyToClose{!closed, producersDone.load() == ProducerCount};
                    if (std::ranges::all_of(readyToClose, std::identity{})) {
                        closed = staging.Close() == AudioCommandStagingStatus::Ok;
                    }
                    std::this_thread::yield();
                }
            }

            void Run() {
                std::array<std::thread, ProducerCount + 1> workers;
                const JoinWorkers join{workers, stop};
                std::ranges::for_each(std::views::iota(std::size_t{0}, ProducerCount), [&](const std::size_t producer) {
                    workers[producer] = std::thread([this, producer] {
                        Produce(producer);
                    });
                });
                workers.back() = std::thread([this] {
                    Consume();
                });
                Pump();
            }
        };

        TEST_CASE("Detached epoch handoff prepares separate transport and rejects old-epoch commands",
                  "[unit][audio][commands][device-handoff]") {
            auto old = Staging();
            const RetireDetachedTransport retireOld{old};
            REQUIRE(old.RegisterScene(Scene()) == AudioCommandStagingStatus::Ok);
            REQUIRE(old.Close() == AudioCommandStagingStatus::Ok);
            REQUIRE(old.IsDrained());
            const AudioCommandScope previous{Owner(), 5, Scene()};
            const AudioCommandScope replacement{Owner(), 6, Scene()};
            auto prepared = Internal::AudioPlaybackEpoch::PrepareTransport(old, previous, replacement, ReplacementDescriptor());
            REQUIRE(prepared.HasValue());
            auto next = std::move(prepared).Value();
            const RetireDetachedTransport retireNext{next};
            REQUIRE_FALSE(next.IsDrained());
            REQUIRE(next.Stats().has_value());
            REQUIRE(next.Stats()->ingressDepth == 0);
            REQUIRE(next.Stats()->callbackDepth == 0);
            REQUIRE(next.Submit(Start()).status == AudioCommandStagingStatus::InvalidCommand);
            auto command = Start();
            command.scope = replacement;
            auto foreignContext = command;
            ++foreignContext.scope.scene.generation;
            REQUIRE(next.Submit(foreignContext).status == AudioCommandStagingStatus::InvalidScene);
            const auto admitted = next.Submit(command);
            REQUIRE(admitted.status == AudioCommandStagingStatus::Ok);
            REQUIRE(admitted.sequence == 1);
            REQUIRE(next.Pump(4).published == 1);
            AudioCommandRecord record;
            REQUIRE(next.TryConsume(record));
            REQUIRE(record.sequence == 1);
            REQUIRE(record.command.scope == replacement);
            REQUIRE(old.IsDrained());
            REQUIRE(old.Submit(Start()).status == AudioCommandStagingStatus::Closed);
        }

        TEST_CASE("Detached transport preparation waits for actual old admission closure and queue draining",
                  "[unit][audio][commands][device-handoff]") {
            auto old = Staging();
            const RetireDetachedTransport retire{old};
            REQUIRE(old.RegisterScene(Scene()) == AudioCommandStagingStatus::Ok);
            std::optional<AudioCommandStaging> moved;
            SECTION("open even when empty") {}
            SECTION("moved-from owner") {
                REQUIRE(old.Close() == AudioCommandStagingStatus::Ok);
                moved.emplace(std::move(old));
            }
            SECTION("accepted ingress") {
                REQUIRE(old.Submit(Start()).status == AudioCommandStagingStatus::Ok);
                REQUIRE(old.Close() == AudioCommandStagingStatus::Ok);
            }
            SECTION("published callback record") {
                REQUIRE(old.Submit(Start()).status == AudioCommandStagingStatus::Ok);
                REQUIRE(old.Pump(4).published == 1);
                REQUIRE(old.Close() == AudioCommandStagingStatus::Ok);
            }
            const auto prepared =
                Internal::AudioPlaybackEpoch::PrepareTransport(old, {Owner(), 5, Scene()}, {Owner(), 6, Scene()}, ReplacementDescriptor());
            REQUIRE(prepared.HasError());
            REQUIRE(prepared.ErrorValue().code.Value() == AudioErrors::RuntimeInactive.code.Value());
        }

        TEST_CASE("Detached transport preparation rejects stale context scope and cross-lane storage aliasing",
                  "[unit][audio][commands][device-handoff]") {
            auto old = Staging();
            const RetireDetachedTransport retire{old};
            REQUIRE(old.RegisterScene(Scene()) == AudioCommandStagingStatus::Ok);
            REQUIRE(old.Close() == AudioCommandStagingStatus::Ok);
            AudioCommandScope previous{Owner(), 5, Scene()};
            AudioCommandScope replacement{Owner(), 6, Scene()};
            auto descriptor = ReplacementDescriptor();
            SECTION("stale old epoch") {
                previous.epoch = 4;
            }
            SECTION("same new epoch") {
                replacement.epoch = 5;
                descriptor.callback.epoch = 5;
            }
            SECTION("different context") {
                replacement.scene = Scene(2);
            }
            SECTION("stale context generation") {
                previous.scene.generation = 2;
                replacement.scene = previous.scene;
            }
            SECTION("invalid context slot") {
                previous.scene.slot = 0;
                replacement.scene = previous.scene;
            }
            SECTION("context outside old reservation") {
                previous.scene.slot = Descriptor().sceneSlots + 1;
                replacement.scene = previous.scene;
            }
            SECTION("foreign runtime") {
                replacement.owner = AudioRuntimeId::Create(18).Value();
                replacement.scene.owner = replacement.owner;
                descriptor.callback.owner = replacement.owner;
            }
            SECTION("descriptor epoch") {
                descriptor.callback.epoch = 7;
            }
            SECTION("callback aliases old callback") {
                descriptor.callback.storageIdentity = Descriptor().callback.storageIdentity;
            }
            SECTION("callback aliases old ingress") {
                descriptor.callback.storageIdentity = Descriptor().ingressIdentity;
            }
            SECTION("ingress aliases old callback") {
                descriptor.ingressIdentity = Descriptor().callback.storageIdentity;
            }
            SECTION("ingress aliases old ingress") {
                descriptor.ingressIdentity = Descriptor().ingressIdentity;
            }
            REQUIRE(Internal::AudioPlaybackEpoch::PrepareTransport(old, previous, replacement, descriptor).HasError());
            REQUIRE(old.IsDrained());
            REQUIRE(old.Submit(Start()).status == AudioCommandStagingStatus::Closed);
        }

        TEST_CASE("Detached transport preparation preserves a non-default context and requires its new reservation",
                  "[unit][audio][commands][device-handoff]") {
            auto old = Staging();
            const RetireDetachedTransport retireOld{old};
            const auto scene = Scene(3, 7);
            REQUIRE(old.RegisterScene(scene) == AudioCommandStagingStatus::Ok);
            REQUIRE(old.Close() == AudioCommandStagingStatus::Ok);
            const AudioCommandScope previous{Owner(), 5, scene};
            const AudioCommandScope replacement{Owner(), 6, scene};
            auto descriptor = ReplacementDescriptor();
            descriptor.sceneSlots = 2;
            REQUIRE(Internal::AudioPlaybackEpoch::PrepareTransport(old, previous, replacement, descriptor).HasError());
            REQUIRE(old.IsDrained());
            descriptor.sceneSlots = 3;
            auto prepared = Internal::AudioPlaybackEpoch::PrepareTransport(old, previous, replacement, descriptor);
            REQUIRE(prepared.HasValue());
            auto next = std::move(prepared).Value();
            const RetireDetachedTransport retireNext{next};
            auto command = Start(scene);
            command.scope = replacement;
            REQUIRE(next.Submit(command).status == AudioCommandStagingStatus::Ok);
            REQUIRE(next.Pump(4).published == 1);
            AudioCommandRecord record;
            REQUIRE(next.TryConsume(record));
            REQUIRE(record.command.scope == replacement);
            REQUIRE(record.sequence == 1);
        }

        TEST_CASE("Detached transport preparation cannot revive an unregistered or unloading context",
                  "[unit][audio][commands][device-handoff]") {
            auto old = Staging();
            const RetireDetachedTransport retire{old};
            SECTION("unregistered context") {}
            SECTION("queued unload") {
                REQUIRE(old.RegisterScene(Scene()) == AudioCommandStagingStatus::Ok);
                REQUIRE(old.StageSceneUnload(Scene()).status == AudioCommandStagingStatus::Ok);
            }
            REQUIRE(old.Close() == AudioCommandStagingStatus::Ok);
            (void)old.Pump(4);
            AudioCommandRecord cancelled;
            while (old.TryConsume(cancelled)) {
                // This never-attached fixture has no external resources; unload remains unacknowledged in its gate.
            }
            REQUIRE(old.IsDrained());
            REQUIRE(
                Internal::AudioPlaybackEpoch::PrepareTransport(old, {Owner(), 5, Scene()}, {Owner(), 6, Scene()}, ReplacementDescriptor())
                    .HasError());
        }

        TEST_CASE("Detached transport preparation propagates budget failure without consuming the retired owner",
                  "[unit][audio][commands][device-handoff]") {
            auto old = Staging();
            const RetireDetachedTransport retire{old};
            REQUIRE(old.RegisterScene(Scene()) == AudioCommandStagingStatus::Ok);
            REQUIRE(old.Close() == AudioCommandStagingStatus::Ok);
            auto descriptor = ReplacementDescriptor();
            descriptor.callback.budgetBytes = 1;
            const auto rejected =
                Internal::AudioPlaybackEpoch::PrepareTransport(old, {Owner(), 5, Scene()}, {Owner(), 6, Scene()}, descriptor);
            REQUIRE(rejected.HasError());
            REQUIRE(rejected.ErrorValue().code.Value() == AudioErrors::MemoryBudgetExceeded.code.Value());
            REQUIRE(old.IsDrained());
            auto prepared =
                Internal::AudioPlaybackEpoch::PrepareTransport(old, {Owner(), 5, Scene()}, {Owner(), 6, Scene()}, ReplacementDescriptor());
            REQUIRE(prepared.HasValue());
            auto next = std::move(prepared).Value();
            const RetireDetachedTransport retireNext{next};
        }

        TEST_CASE("Audio staging rejects invalid dimensions and allocation failure", "[unit][audio][commands]") {
            auto descriptor = Descriptor();
            SECTION("duplicate pools") {
                descriptor.ingressIdentity = descriptor.callback.storageIdentity;
            }
            SECTION("invalid pool") {
                descriptor.ingressIdentity = {};
            }
            SECTION("empty queue") {
                descriptor.ingressSlots = 0;
            }
            SECTION("queue bound") {
                descriptor.ingressSlots = MaximumAudioCommandSlots + 1;
            }
            SECTION("queue power") {
                descriptor.ingressSlots = 3;
            }
            SECTION("reserve empty") {
                descriptor.criticalSlots = 0;
            }
            SECTION("reserve bound") {
                descriptor.criticalSlots = descriptor.ingressSlots;
            }
            SECTION("scenes empty") {
                descriptor.sceneSlots = 0;
            }
            SECTION("scenes bound") {
                descriptor.sceneSlots = MaximumAudioHandleSlots + 1;
            }
            SECTION("budget bound") {
                descriptor.ingressBudgetBytes = MaximumAudioMemoryBytes + 1;
            }
            SECTION("gate budget") {
                descriptor.ingressBudgetBytes = 1;
            }
            SECTION("payload budget") {
                descriptor.ingressBudgetBytes = 100;
            }
            SECTION("callback invalid") {
                descriptor.callback.epoch = 0;
            }
            REQUIRE(AudioCommandStaging::Create(descriptor).HasError());
        }

        TEST_CASE("Audio staging rolls back all preparation allocations", "[unit][audio][commands]") {
            const auto descriptor = Descriptor();
            for (std::size_t failure = 1; failure <= 9; ++failure) {
                Test::failCountdown = failure;
                const auto result = AudioCommandStaging::Create(descriptor);
                Test::failCountdown = 0;
                REQUIRE(result.HasError());
                REQUIRE(result.ErrorValue().code.Value() == AudioErrors::MemoryAllocationFailed.code.Value());
            }
        }

        TEST_CASE("Audio staging preserves stop release unload and reset through both full queues", "[unit][audio][commands]") {
            auto staging = Staging();
            REQUIRE(staging.RegisterScene(Scene()) == AudioCommandStagingStatus::Ok);
            REQUIRE(staging.RegisterScene(Scene(2)) == AudioCommandStagingStatus::Ok);
            REQUIRE(staging.Submit(Start()).sequence == 1);
            REQUIRE(staging.Submit(Start(Scene(2))).sequence == 2);
            REQUIRE(staging.Submit(Start()).status == AudioCommandStagingStatus::OrdinaryFull);
            REQUIRE(staging.Submit(Stop()).sequence == 3);
            auto release = Start();
            release.payload = AudioReleaseResourceCommand{
                .storage = {.owner = Owner(), .pool = AudioMemoryPoolId::Create(99).Value(), .slot = 1, .generation = 1}};
            REQUIRE(staging.Submit(release).sequence == 4);
            REQUIRE(staging.StageSceneUnload(Scene()).status == AudioCommandStagingStatus::CriticalRetry);
            REQUIRE(staging.Submit(Start()).status == AudioCommandStagingStatus::InvalidScene);
            REQUIRE(staging.Submit(Stop()).status == AudioCommandStagingStatus::CriticalRetry);
            REQUIRE(staging.Pump(2).published == 2);
            const auto unload = staging.StageSceneUnload(Scene());
            REQUIRE(unload.sequence == 5);
            REQUIRE(staging.StageSceneUnload(Scene()).status == AudioCommandStagingStatus::AlreadyStaged);
            REQUIRE(staging.AcknowledgeScene(Scene(), 5) == AudioCommandStagingStatus::InvalidScene);
            REQUIRE(staging.StageReset().sequence == 6);
            REQUIRE(staging.StageReset().status == AudioCommandStagingStatus::AlreadyStaged);
            REQUIRE(staging.Submit(Stop()).status == AudioCommandStagingStatus::Closed);
            const auto blocked = staging.Pump(4);
            REQUIRE(blocked.status == AudioCommandStagingStatus::OutputFull);
            REQUIRE(blocked.published == 2);
            AudioCommandRecord output;
            for (std::uint64_t sequence = 1; sequence <= 4; ++sequence) {
                REQUIRE(staging.TryConsume(output));
                REQUIRE(output.sequence == sequence);
            }
            REQUIRE(staging.Pump(4).published == 2);
            REQUIRE(staging.TryConsume(output));
            REQUIRE(output.sequence == 5);
            REQUIRE(std::holds_alternative<AudioSceneUnloadCommand>(output.command.payload));
            REQUIRE(staging.AcknowledgeScene(Scene(), 5) == AudioCommandStagingStatus::Ok);
            REQUIRE(staging.RegisterScene(Scene(1, 2)) == AudioCommandStagingStatus::Closed);
            REQUIRE(staging.TryConsume(output));
            REQUIRE(output.sequence == 6);
            REQUIRE(std::holds_alternative<AudioResetCommand>(output.command.payload));
            REQUIRE(staging.IsDrained());
        }

        TEST_CASE("Audio staging coalesces adjacent parameters without bypassing intervening commands", "[unit][audio][commands]") {
            auto staging = Staging();
            REQUIRE(staging.RegisterScene(Scene()) == AudioCommandStagingStatus::Ok);
            REQUIRE(staging.Submit(Parameter(1.0F)).sequence == 1);
            const auto replacement = staging.Submit(Parameter(2.0F));
            REQUIRE(replacement.status == AudioCommandStagingStatus::Coalesced);
            REQUIRE(replacement.sequence == 2);
            REQUIRE(replacement.replacedSequence == 1);
            REQUIRE(staging.Submit(Start()).sequence == 3);
            REQUIRE(staging.Submit(Parameter(3.0F)).status == AudioCommandStagingStatus::OrdinaryFull);
            REQUIRE(staging.Pump(1).published == 1);
            REQUIRE(staging.Submit(Parameter(4.0F)).sequence == 4);
            AudioCommandRecord output;
            REQUIRE(staging.TryConsume(output));
            REQUIRE(output.sequence == 2);
            REQUIRE(std::get<AudioSetParameterCommand>(output.command.payload).value == 2.0F);
            REQUIRE(staging.Pump(2).published == 2);
            REQUIRE(staging.TryConsume(output));
            REQUIRE(output.sequence == 3);
            REQUIRE(staging.TryConsume(output));
            REQUIRE(output.sequence == 4);
            REQUIRE(std::get<AudioSetParameterCommand>(output.command.payload).value == 4.0F);
            REQUIRE(staging.Close() == AudioCommandStagingStatus::Ok);
            REQUIRE(staging.IsDrained());
        }

        TEST_CASE("Audio staging requires acknowledged retirement before context generation reuse", "[unit][audio][commands]") {
            auto staging = Staging();
            REQUIRE(staging.Submit(Start()).status == AudioCommandStagingStatus::InvalidScene);
            REQUIRE(staging.RegisterScene({}) == AudioCommandStagingStatus::InvalidScene);
            REQUIRE(staging.RegisterScene(Scene(5)) == AudioCommandStagingStatus::InvalidScene);
            REQUIRE(staging.RegisterScene(Scene()) == AudioCommandStagingStatus::Ok);
            REQUIRE(staging.RegisterScene(Scene(1, 2)) == AudioCommandStagingStatus::InvalidScene);
            REQUIRE(staging.StageSceneUnload(Scene()).sequence == 1);
            REQUIRE(staging.Pump(1).published == 1);
            REQUIRE(staging.AcknowledgeScene(Scene(), 2) == AudioCommandStagingStatus::InvalidScene);
            AudioCommandRecord output;
            REQUIRE(staging.TryConsume(output));
            REQUIRE(staging.AcknowledgeScene(Scene(), output.sequence) == AudioCommandStagingStatus::Ok);
            REQUIRE(staging.StageSceneUnload(Scene()).status == AudioCommandStagingStatus::InvalidScene);
            REQUIRE(staging.RegisterScene(Scene()) == AudioCommandStagingStatus::InvalidScene);
            REQUIRE(staging.RegisterScene(Scene(1, 2)) == AudioCommandStagingStatus::Ok);
            REQUIRE(staging.Submit(Start()).status == AudioCommandStagingStatus::InvalidScene);
            REQUIRE(staging.Submit(Start(Scene(1, 2))).status == AudioCommandStagingStatus::Ok);
        }

        TEST_CASE("Audio staging rejects malformed and producer-owned barrier requests", "[unit][audio][commands]") {
            auto staging = Staging();
            REQUIRE(staging.RegisterScene(Scene()) == AudioCommandStagingStatus::Ok);
            auto command = Start();
            SECTION("runtime") {
                command.scope.owner = AudioRuntimeId::Create(18).Value();
            }
            SECTION("epoch") {
                command.scope.epoch = 6;
            }
            SECTION("payload") {
                command.payload = AudioStartVoiceCommand{};
            }
            SECTION("unload") {
                command.payload = AudioSceneUnloadCommand{};
            }
            SECTION("reset") {
                command.scope.scene = {};
                command.payload = AudioResetCommand{};
            }
            REQUIRE(staging.Submit(command).status == AudioCommandStagingStatus::InvalidCommand);
            REQUIRE(staging.Submit(Start()).sequence == 1);
        }

        TEST_CASE("Audio staging reset retry closes ordinary admission without dropping critical work", "[unit][audio][commands]") {
            auto staging = Staging();
            REQUIRE(staging.RegisterScene(Scene()) == AudioCommandStagingStatus::Ok);
            for (std::uint64_t sequence = 1; sequence <= 4; ++sequence) {
                REQUIRE(staging.Submit(Stop()).sequence == sequence);
            }
            REQUIRE(staging.StageReset().status == AudioCommandStagingStatus::CriticalRetry);
            REQUIRE(staging.Submit(Start()).status == AudioCommandStagingStatus::Closed);
            REQUIRE(staging.RegisterScene(Scene(2)) == AudioCommandStagingStatus::Closed);
            REQUIRE(staging.Pump(1).published == 1);
            REQUIRE(staging.Submit(Stop()).sequence == 5);
            REQUIRE(staging.StageReset().status == AudioCommandStagingStatus::CriticalRetry);
            REQUIRE(staging.Pump(1).published == 1);
            REQUIRE(staging.StageReset().sequence == 6);
            REQUIRE(staging.StageSceneUnload(Scene()).status == AudioCommandStagingStatus::Closed);
            AudioCommandRecord output;
            for (std::uint64_t sequence = 1; sequence <= 6; ++sequence) {
                static_cast<void>(staging.Pump(1));
                REQUIRE(staging.TryConsume(output));
                REQUIRE(output.sequence == sequence);
            }
            REQUIRE(staging.IsDrained());
        }

        TEST_CASE("Audio staging close drains accepted records and moves leave inert owners", "[unit][audio][commands]") {
            auto staging = Staging();
            REQUIRE(staging.RegisterScene(Scene()) == AudioCommandStagingStatus::Ok);
            REQUIRE(staging.Submit(Start()).sequence == 1);
            REQUIRE(staging.Close() == AudioCommandStagingStatus::Ok);
            REQUIRE(staging.StageReset().status == AudioCommandStagingStatus::Closed);
            REQUIRE(staging.Submit(Stop()).status == AudioCommandStagingStatus::Closed);
            REQUIRE_FALSE(staging.IsDrained());
            auto moved = std::move(staging);
            AudioCommandRecord output;
            REQUIRE(staging.RegisterScene(Scene()) == AudioCommandStagingStatus::Inactive);
            REQUIRE(staging.Submit(Start()).status == AudioCommandStagingStatus::Inactive);
            REQUIRE(staging.StageSceneUnload(Scene()).status == AudioCommandStagingStatus::Inactive);
            REQUIRE(staging.StageReset().status == AudioCommandStagingStatus::Inactive);
            REQUIRE(staging.AcknowledgeScene(Scene(), 1) == AudioCommandStagingStatus::Inactive);
            REQUIRE(staging.Pump(1).status == AudioCommandStagingStatus::Inactive);
            REQUIRE(staging.Close() == AudioCommandStagingStatus::Inactive);
            REQUIRE_FALSE(staging.TryConsume(output));
            REQUIRE(staging.IsDrained());
            REQUIRE(moved.Pump(0).published == 0);
            REQUIRE(moved.Pump(1).published == 1);
            REQUIRE(moved.TryConsume(output));
            REQUIRE(output.sequence == 1);
            REQUIRE(moved.IsDrained());
            staging = std::move(moved);
            REQUIRE(moved.IsDrained());
            REQUIRE(staging.IsDrained());
        }

        TEST_CASE("Audio staging admission pump and callback paths allocate and free no heap", "[unit][audio][commands]") {
            auto staging = Staging();
            const auto scene = Scene();
            const auto start = Start();
            AudioCommandRecord output;
            const auto allocated = Test::allocationCount;
            const auto freed = Test::deallocationCount;
            const auto registered = staging.RegisterScene(scene);
            const auto accepted = staging.Submit(start);
            const auto unload = staging.StageSceneUnload(scene);
            const auto pumped = staging.Pump(4);
            const auto first = staging.TryConsume(output);
            const auto second = staging.TryConsume(output);
            const auto acknowledged = staging.AcknowledgeScene(scene, output.sequence);
            const auto closed = staging.Close();
            const auto drained = staging.IsDrained();
            const auto allocationDelta = Test::allocationCount - allocated;
            const auto freeDelta = Test::deallocationCount - freed;
            REQUIRE(allocationDelta == 0);
            REQUIRE(freeDelta == 0);
            REQUIRE(registered == AudioCommandStagingStatus::Ok);
            REQUIRE(accepted.sequence == 1);
            REQUIRE(unload.sequence == 2);
            REQUIRE(pumped.published == 2);
            REQUIRE(first);
            REQUIRE(second);
            REQUIRE(acknowledged == AudioCommandStagingStatus::Ok);
            REQUIRE(closed == AudioCommandStagingStatus::Ok);
            REQUIRE(drained);
        }

        TEST_CASE("Audio staging transfers four concurrent producers through control to callback", "[unit][audio][commands]") {
            auto staging = Staging();
            REQUIRE(staging.RegisterScene(Scene()) == AudioCommandStagingStatus::Ok);
            ConcurrentRun run{.staging = staging};
            run.Run();
            std::ranges::for_each(std::views::iota(std::size_t{0}, ConcurrentRun::ProducerCount), [&](const std::size_t producer) {
                REQUIRE(run.sent[producer] == ConcurrentRun::PerProducer);
                REQUIRE(run.received[producer] == ConcurrentRun::PerProducer);
            });
            REQUIRE(run.closed);
            REQUIRE_FALSE(run.incorrect);
            REQUIRE(run.callbackAllocations == 0);
            REQUIRE(run.callbackFrees == 0);
        }
    }  // namespace
}  // namespace Horo::Audio
