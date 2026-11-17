#include "AudioVoiceRenderTestFixture.h"
#include "Horo/Audio/Internal/AudioFrontendComposition.h"
#include "Horo/Audio/Internal/NativeAudioBackend.h"

#include <bit>

namespace Horo::Tests::VoiceRenderFixture {
    namespace {
        namespace Backend = Audio::Backend;
        constexpr AudioMonotonicTimestamp Deadline{1, std::numeric_limits<std::uint64_t>::max()};
        const AudioDeviceEpoch Epoch{{Owner, 1, 1}, 1, Scope.epoch};

        /** @brief Synchronous packet transport exercises the real adapter/core without qualifying an OS endpoint. */
        class PacketDriver final : public Backend::NativeAudioDriver {
        public:
            explicit PacketDriver(const AudioBackendKind peer) : peer_(peer) {}

            AudioBackendKind Kind() const noexcept override {
                return peer_;
            }

            Backend::ResourceDisposition Resources() const noexcept override {
                return running  ? Backend::ResourceDisposition::Retained
                       : opened ? Backend::ResourceDisposition::CallbackDetached
                                : Backend::ResourceDisposition::Closed;
            }

            bool HasUndrainedRequiredFacts() const noexcept override {
                return false;
            }

            Result<AudioBackendProbe> Probe() override {
                AudioBackendProbe probe{.backend = peer_,
                                        .compiled = true,
                                        .hostSupported = true,
                                        .availability = AudioBackendAvailability::Available,
                                        .revision = 1,
                                        .backendVersion = "packet fixture"};
                probe.features.fill(AudioCapabilitySupport::Unsupported);
                return Result<AudioBackendProbe>::Success(std::move(probe));
            }

            Result<AudioDeviceSnapshot> Enumerate() override {
                return Result<AudioDeviceSnapshot>::Success({.owner = Owner,
                                                             .backend = peer_,
                                                             .revision = 1,
                                                             .devices = {{Epoch.device, "Packet fixture", AudioDeviceClass::Virtual}},
                                                             .defaults = {Epoch.device, Epoch.device, Epoch.device}});
            }

            Result<Backend::Opened> Open(const Backend::Open &request) override {
                auto format = Backend::NegotiateNativeFloatOutput(request, request.format.preferred, 1, 16);
                if (format.HasError())
                    return Result<Backend::Opened>::Failure(format.ErrorValue());
                opened = true;
                return Result<Backend::Opened>::Success(
                    {std::move(format).Value(), {.epoch = request.plannedEpoch, .capturedAt = {1, 0}}, Backend::AccessMode::Shared});
            }

            Result<void> Start(const Backend::NativeAudioRenderPort render) override {
                if (!opened || running || !render.process)
                    return Result<void>::Failure(MakeError(AudioErrors::RuntimeInactive));
                port = render;
                running = true;
                return Result<void>::Success();
            }

            Backend::NativeRetirementResult Stop(AudioMonotonicTimestamp) override {
                // All invocations finish synchronously on this owner; clearing the port prevents future entry.
                ++stops;
                port = {};
                running = false;
                return Backend::NativeRetirementResult::Success(Backend::NativeRetirementProgress::Complete);
            }

            Backend::NativeRetirementResult Close() override {
                ++closes;
                if (running)
                    return Backend::NativeRetirementResult::Failure(MakeError(AudioErrors::RuntimeInactive));
                opened = false;
                return Backend::NativeRetirementResult::Success(Backend::NativeRetirementProgress::Complete);
            }

            std::size_t DrainEvents(std::span<Backend::Event>) noexcept override {
                return 0;
            }

            void Emit(const std::span<float> samples) noexcept {
                if (port.process) {
                    port.process(port.context, samples, static_cast<std::uint32_t>(samples.size() / 2));
                    packetFrames += samples.size() / 2;
                }
            }

            Backend::NativeAudioRenderPort port;
            bool opened{}, running{};
            std::uint32_t stops{}, closes{};
            std::uint64_t packetFrames{};  // Fixture transfer count, not native API submission or device consumption.

        private:
            AudioBackendKind peer_;
        };

        /** @brief Retire never-attached preparation if frontend construction rejects or a fatal assertion unwinds. */
        struct DetachedPreparation final {
            Internal::AudioFrontendResources resources;

            ~DetachedPreparation() {
                if (!resources.voice)
                    return;  // Successful composition transferred the complete island.
                resources.voice->Close();
                (void)resources.voice->CompleteShutdown(true);
                resources.mixer->Close();
                (void)resources.mixer->CompleteShutdown(resources.scope, true);
                (void)resources.staging.Close();
                AudioCommandRecord record;
                for (std::size_t step = 0; step <= MaximumAudioCommandSlots && !resources.staging.IsDrained(); ++step) {
                    (void)resources.staging.Pump(64);
                    while (resources.staging.TryConsume(record)) {
                    }
                }
            }
        };

        /** @brief Real PCM, voice, mixer, frontend and shared native adapter retain all borrows through actual fixture Stop. */
        struct NativeFrontendRig final {
            std::shared_ptr<AudioVoiceStateMachine> registry = Registry();
            Audio::PlaybackTest::SampleBuffers samples;
            PacketDriver *driver{};
            Backend::NativeAudioBackend *native{};
            std::unique_ptr<AudioFrontend> frontend;

            explicit NativeFrontendRig(const AudioBackendKind peer) {
                samples.pcm.fill(0.25F);  // Keep the complete fade below the native safety clamp.
                auto primitive = std::make_unique<PacketDriver>(peer);
                driver = primitive.get();
                auto createdBackend = Backend::CreateNativeAudioBackend({Owner, 1}, std::move(primitive));
                REQUIRE(createdBackend.HasValue());
                auto backend = std::move(createdBackend).Value();
                native = backend.get();
                const auto enumerate = native->Begin(Backend::Enumerate{}, Deadline).Value();
                REQUIRE(native->AdvanceControl().HasValue());
                const auto completion = native->Poll(enumerate);
                REQUIRE(completion.HasValue());
                REQUIRE(completion.Value());
                const auto *catalog = std::get_if<AudioDeviceSnapshot>(&completion.Value()->outcome);
                REQUIRE(catalog != nullptr);
                Internal::AudioFrontendOutput output{std::move(backend),
                                                     {Epoch,
                                                      {Epoch.device, {48'000, MixerFixture::Profile().outputLayout}, {}, {16, 16, 16}}},
                                                     *catalog,
                                                     1};
                REQUIRE(native->AcknowledgeCompletion(enumerate).HasValue());
                auto voice = AudioVoiceRenderRuntime::CreateResident(registry, samples.Source(), Playback(), Descriptor());
                REQUIRE(voice.HasValue());
                DetachedPreparation prepared{{Scope, Staging(), std::move(voice).Value(), Runtime(), Plan(Asset()), Request()}};
                auto created = Internal::AudioFrontendComposition::Create(prepared.resources, output);
                REQUIRE(created.HasValue());
                frontend = std::move(created).Value();
            }

            ~NativeFrontendRig() {
                if (!frontend)
                    return;
                frontend->Close();
                for (std::size_t step = 0; step < 24 && frontend->Snapshot().phase != AudioFrontendPhase::Closed; ++step) {
                    (void)frontend->Pump(Deadline);
                    (void)native->AdvanceControl();
                    std::array<float, 32> output{};
                    driver->Emit(output);
                }
            }

            void Activate() {
                REQUIRE(frontend->Start(Deadline).HasValue());
                Pump();
                Pump();
                std::array<float, 32> output{};
                driver->Emit(output);
                REQUIRE(frontend->Pump(Deadline).HasValue());
                REQUIRE(frontend->Snapshot().phase == AudioFrontendPhase::Active);
                const auto admitted = frontend->Transport({frontend->Snapshot().voice, AudioVoiceControl::Start});
                REQUIRE(admitted.HasValue());
                REQUIRE(admitted.Value().status == AudioCommandStagingStatus::Ok);
                REQUIRE(frontend->Pump(Deadline).HasValue());
                driver->Emit(output);
                REQUIRE(frontend->Pump(Deadline).HasValue());
            }

            void Pump() {
                REQUIRE(native->AdvanceControl().HasValue());
                REQUIRE(frontend->Pump(Deadline).HasValue());
            }
        };

        TEST_CASE("Real frontend hold survives smaller native packets through quiescence and actual fixture retirement",
                  "[audio][frontend][native-contract][device-handoff]") {
            for (const auto peer : {AudioBackendKind::WASAPI, AudioBackendKind::CoreAudio, AudioBackendKind::PipeWire}) {
                NativeFrontendRig rig(peer);
                rig.Activate();
                const auto hold = Internal::AudioFrontendComposition::RequestPlaybackHold(*rig.frontend);
                REQUIRE(hold.HasValue());
                std::array<float, 10> prefix{};
                const auto allocations = AllocationProbe::Count();
                const auto frees = AllocationProbe::FreeCount();
                rig.driver->Emit(prefix);
                const auto allocationEnd = AllocationProbe::Count();
                const auto freeEnd = AllocationProbe::FreeCount();
                REQUIRE(allocationEnd == allocations);
                REQUIRE(freeEnd == frees);
                const auto held = Internal::AudioFrontendComposition::PollPlaybackHold(*rig.frontend, hold.Value());
                REQUIRE(held.HasValue());
                REQUIRE(held.Value());
                REQUIRE(held.Value()->cursor.frame == 32);
                REQUIRE(held.Value()->sampleFrame == 48);
                REQUIRE(rig.driver->packetFrames == 37);
                REQUIRE(prefix.front() > 0.0F);
                rig.frontend->Close();
                REQUIRE(rig.frontend->Pump(Deadline).HasValue());
                REQUIRE(rig.native->AdvanceControl().HasValue());
                std::array<float, 22> tail{};
                rig.driver->Emit(tail);
                for (std::uint32_t frame = 0; frame < 11; ++frame) {
                    const auto gain = 1.0F - static_cast<float>(frame + 5) / 15.0F;
                    REQUIRE(tail[frame * 2] == prefix[0] * gain);
                    REQUIRE(tail[frame * 2 + 1] == prefix[1] * gain);
                }
                REQUIRE(std::bit_cast<std::uint32_t>(tail.back()) == 0);
                REQUIRE(rig.driver->packetFrames == held.Value()->sampleFrame);
                rig.Pump();
                REQUIRE(rig.driver->stops == 0);
                REQUIRE(rig.driver->closes == 0);
                REQUIRE(rig.frontend->Snapshot().phase == AudioFrontendPhase::Closing);
                std::array<float, 2> acknowledgement{1.0F, 1.0F};
                rig.driver->Emit(acknowledgement);
                REQUIRE(acknowledgement[0] == 0.0F);
                REQUIRE(acknowledgement[1] == 0.0F);
                for (std::size_t step = 0; step < 8 && rig.frontend->Snapshot().phase != AudioFrontendPhase::Closed; ++step)
                    rig.Pump();
                REQUIRE(rig.frontend->Snapshot().phase == AudioFrontendPhase::Closed);
                REQUIRE(rig.driver->stops == 1);
                REQUIRE(rig.driver->closes == 1);
                REQUIRE_FALSE(rig.driver->running);
                REQUIRE(rig.driver->port.process == nullptr);
            }
        }
    }  // namespace
}  // namespace Horo::Tests::VoiceRenderFixture
