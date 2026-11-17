#include "AudioFrontendTestFixture.h"
#include "AudioStreamingTestFixture.h"

namespace Horo::Tests::VoiceRenderFixture {
    namespace {
        /** @brief Pins real job/provider dependencies through every stream port and worker join. */
        struct StreamHost final {
            JobSystem jobs{{.workerCount = 1}};
            Audio::StreamingTests::PackageFixture package;
            std::shared_ptr<AudioStreamingService> service{Audio::StreamingTests::Service(jobs, package)};

            ~StreamHost() {
                (void)service->Shutdown();
            }
        };

        /** @brief Compose a real stream and explicit Null output before exercising lifecycle transitions. */
        struct StreamFrontendRig final {
            std::shared_ptr<StreamHost> host = std::make_shared<StreamHost>();
            std::shared_ptr<AudioVoiceStateMachine> registry = Registry();
            AudioStreamHandle stream;
            CapturingOutput *output{};
            std::unique_ptr<AudioFrontend> frontend;

            StreamFrontendRig() {
                auto admitted = host->service->Admit(Audio::StreamingTests::Request());
                REQUIRE(admitted.HasValue());
                stream = admitted.Value();
                auto conversion = Playback().plan.Descriptor();
                conversion.channels = 2;
                auto createdVoice =
                    AudioVoiceRenderRuntime::CreateStream(registry, host->service, stream, conversion, CoefficientBytes, Descriptor());
                REQUIRE(createdVoice.HasValue());
                auto backend = std::make_unique<CapturingOutput>();
                output = backend.get();
                auto candidate = Output(std::move(backend));
                Internal::AudioFrontendResources resources{Scope,         Staging(),     std::move(createdVoice).Value(),
                                                           Runtime(),     Plan(Asset()), Request(),
                                                           host->service, stream,        host};
                auto created = Internal::AudioFrontendComposition::Create(resources, candidate);
                REQUIRE(created.HasValue());
                frontend = std::move(created).Value();
            }
        };

        /** @brief Preserve the original device failure during every asynchronous teardown step. */
        void PumpStreamClose(AudioFrontend &frontend, const bool deviceLost) {
            const auto result = frontend.Pump({1, 1'000'000'000});
            if (deviceLost) {
                REQUIRE(result.HasError());
                CHECK(result.ErrorValue().code.Value() == AudioErrors::DeviceLost.code.Value());
            } else {
                REQUIRE(result.HasValue());
            }
        }

        /** @brief Assert released ownership and one drained outcome for each admitted operation. */
        void CheckRetiredStream(StreamFrontendRig &rig, const bool deviceLost) {
            CHECK(rig.frontend->Snapshot().phase == AudioFrontendPhase::Closed);
            CHECK(rig.host->service->Snapshot(rig.stream).HasError());
            CHECK(rig.registry->Snapshot(rig.frontend->Snapshot().voice).HasError());
            CHECK(rig.host->package.opens.load() == 1);
            CHECK(rig.host->package.releases.load() == 1);
            REQUIRE(rig.frontend->Snapshot().terminalVoice.has_value());
            CHECK(IsTerminalAudioVoiceState(rig.frontend->Snapshot().terminalVoice->state));
            CHECK(rig.frontend->Snapshot().terminalVoice->voice == rig.frontend->Snapshot().voice);
            CHECK(rig.frontend->Snapshot().pendingOperations == 0);
            std::array<AudioFrontendOperationResult, 2> results;
            REQUIRE(rig.frontend->DrainTransportResults(results) == (deviceLost ? 2 : 1));
            CHECK(results[0].disposition == AudioFrontendOperationDisposition::Applied);
            if (deviceLost)
                CHECK(results[1].disposition == AudioFrontendOperationDisposition::Cancelled);
            CHECK(rig.frontend->DrainTransportResults(results) == 0);
        }

        TEST_CASE("AudioFrontend retires stream ports and workers only after native detachment", "[audio][frontend][stream]") {
            StreamFrontendRig rig;
            auto &[host, registry, stream, output, frontend] = rig;
            RetireOnExit retirement{*frontend, *output, AudioFrontendPhase::Closed};
            REQUIRE(Audio::StreamingTests::PumpUntil(*host->service, stream, 4));
            REQUIRE(frontend->Start({1, 1'000'000'000}).HasValue());
            CompleteOutputStart(*frontend, *output);
            REQUIRE(frontend->Transport({frontend->Snapshot().voice, AudioVoiceControl::Start}).Value().status ==
                    AudioCommandStagingStatus::Ok);
            REQUIRE(frontend->Pump({1, 1'000'000'000}).HasValue());
            REQUIRE(output->null->AdvanceCallback().HasValue());
            CHECK(std::any_of(output->left.begin(), output->left.end(), [](const float sample) {
                return sample > 0.0F;
            }));
            bool deviceLost{};
            SECTION("explicit close") {
                frontend->Close();
            }
            SECTION("device loss with a pending control") {
                REQUIRE(frontend->Transport({frontend->Snapshot().voice, AudioVoiceControl::Pause}).Value().status ==
                        AudioCommandStagingStatus::Ok);
                REQUIRE(output->null->InjectDeviceLoss(Audio::Backend::DeviceLossCause::Disconnected).HasValue());
                deviceLost = true;
            }
            PumpStreamClose(*frontend, deviceLost);
            REQUIRE(output->null->AdvanceControl().HasValue());
            CHECK(host->service->Retire(stream).HasError());  // Retained voice port forbids early worker/ring retirement.
            REQUIRE(output->null->AdvanceCallback().HasValue());
            PumpStreamClose(*frontend, deviceLost);
            REQUIRE(output->null->AdvanceControl().HasValue());
            PumpStreamClose(*frontend, deviceLost);
            REQUIRE(output->null->AdvanceControl().HasValue());
            PumpStreamClose(*frontend, deviceLost);
            CheckRetiredStream(rig, deviceLost);
            const std::weak_ptr<StreamHost> lease = host;
            host.reset();
            CHECK(lease.expired());
        }

    }  // namespace
}  // namespace Horo::Tests::VoiceRenderFixture
