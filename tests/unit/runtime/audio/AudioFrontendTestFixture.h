#pragma once

/** @file AudioFrontendTestFixture.h
 * @brief Shared production Null output and lifetime guards for frontend contract tests.
 */
#include "AudioVoiceRenderTestFixture.h"
#include "Horo/Audio/Internal/AudioFrontendComposition.h"

namespace Horo::Tests::VoiceRenderFixture {
    namespace {
        /** @brief Records actual selected Null output while delegating every lifecycle rule to the production peer. */
        class CapturingOutput final : public Audio::Backend::AudioBackend {
        public:
            std::unique_ptr<Audio::Backend::NullAudioBackend> null =
                std::move(Audio::Backend::CreateNullAudioBackend({MixerFixture::Owner, 1, 1, 1}).Value());
            std::array<float, 16> left{};
            Audio::Backend::RenderPort port;
            bool primingSilent{};
            bool quiescingSilent{};
            bool rejectOpenedFormat{};
            std::size_t callbackEntries{};

            AudioBackendKind Kind() const noexcept override {
                return null->Kind();
            }

            AudioRuntimeId Owner() const noexcept override {
                return null->Owner();
            }

            Result<Audio::Backend::OperationId> Begin(const Audio::Backend::Request &request,
                                                      const AudioMonotonicTimestamp &deadline) override {
                if (const auto *start = std::get_if<Audio::Backend::Start>(&request)) {
                    port = start->render;
                    return null->Begin(Audio::Backend::Start{start->epoch, {this, Process}}, deadline);
                }
                return null->Begin(request, deadline);
            }

            Result<void> CommitRendering(const AudioDeviceEpoch &epoch) override {
                return null->CommitRendering(epoch);
            }

            Result<Audio::Backend::CancelDisposition> Cancel(const Audio::Backend::OperationId &operation) override {
                return null->Cancel(operation);
            }

            Result<std::optional<Audio::Backend::Completion>> Poll(const Audio::Backend::OperationId &operation) override {
                auto completion = null->Poll(operation);
                if (!rejectOpenedFormat || completion.HasError())
                    return completion;
                auto captured = std::move(completion).Value();
                if (captured.has_value())
                    if (auto *opened = std::get_if<Audio::Backend::Opened>(&captured->outcome))
                        opened->format.callbackFrames = 32;
                return Result<std::optional<Audio::Backend::Completion>>::Success(std::move(captured));
            }

            Result<void> AcknowledgeCompletion(const Audio::Backend::OperationId &operation) override {
                return null->AcknowledgeCompletion(operation);
            }

            std::size_t DrainEvents(const std::span<Audio::Backend::Event> events) noexcept override {
                return null->DrainEvents(events);
            }

            Audio::Backend::AudioCallbackViolationDrain DrainSafetyViolations(
                const std::span<Audio::Backend::AudioCallbackViolation> records) noexcept override {
                return null->DrainSafetyViolations(records);
            }

            static Audio::Backend::RenderResult Process(void *context, const Audio::Backend::RenderInvocation &invocation) noexcept {
                auto &self = *static_cast<CapturingOutput *>(context);
                ++self.callbackEntries;
                const auto rendered = self.port.process(self.port.context, invocation);
                bool silent = true;
                for (std::size_t index = 0; index < self.left.size(); ++index) {
                    self.left[index] = invocation.output.planes[0][index];
                    silent = silent && self.left[index] == 0.0F;
                }
                if (invocation.phase == Audio::Backend::RenderPhase::Priming)
                    self.primingSilent = silent;
                if (invocation.phase == Audio::Backend::RenderPhase::Quiescing)
                    self.quiescingSilent = silent;
                return rendered;
            }
        };

        /** @brief Exact discovery facts belong to explicit host composition, not the editor document. */
        Internal::AudioFrontendOutput Output(std::unique_ptr<CapturingOutput> backend) {
            const AudioDeviceEpoch epoch{{MixerFixture::Owner, 1, 1}, 1, Scope.epoch};
            AudioDeviceSnapshot devices{.owner = MixerFixture::Owner,
                                        .backend = AudioBackendKind::NullAudio,
                                        .revision = 1,
                                        .devices = {{epoch.device, "Null Audio", AudioDeviceClass::Headless}},
                                        .defaults = {epoch.device, epoch.device, epoch.device}};
            Audio::Backend::Open open{epoch, {epoch.device, {48'000, MixerFixture::Profile().outputLayout}, {}, {16, 16, 16}}};
            return {std::move(backend), std::move(open), std::move(devices), 1};
        }

        /** @brief Completes exact Open/Start/Ready progression for a host-owned preview or document. */
        template <typename Owner> void CompleteOutputStart(Owner &owner, CapturingOutput &output) {
            REQUIRE(output.null->AdvanceControl().HasValue());
            REQUIRE(owner.Pump({1, 1'000'000'000}).HasValue());
            REQUIRE(output.null->AdvanceControl().HasValue());
            REQUIRE(owner.Pump({1, 1'000'000'000}).HasValue());
            REQUIRE(output.null->AdvanceCallback().HasValue());
            REQUIRE(owner.Pump({1, 1'000'000'000}).HasValue());
        }

        /** @brief Keeps production PCM preparation and all callback/storage owners in normal dependency order. */
        struct PreviewRig final {
            std::shared_ptr<AudioVoiceStateMachine> registry = Registry();
            Audio::PlaybackTest::SampleBuffers samples;
            CapturingOutput *output{};
            std::unique_ptr<AudioFrontend> frontend;

            PreviewRig() {
                auto backend = std::make_unique<CapturingOutput>();
                output = backend.get();
                auto candidate = Output(std::move(backend));
                auto voice = AudioVoiceRenderRuntime::CreateResident(registry, samples.Source(), Playback(), Descriptor());
                REQUIRE(voice.HasValue());
                Internal::AudioFrontendResources resources{Scope, Staging(), std::move(voice).Value(), Runtime(), Plan(Asset()), Request()};
                auto created = Internal::AudioFrontendComposition::Create(resources, candidate);
                REQUIRE(created.HasValue());
                frontend = std::move(created).Value();
            }

            ~PreviewRig() {
                if (!frontend)
                    return;
                frontend->Close();
                for (std::size_t step = 0; step < 16 && frontend->Snapshot().phase != AudioFrontendPhase::Closed; ++step) {
                    (void)frontend->Pump({1, 1'000'000'000});
                    (void)output->null->AdvanceControl();
                    const auto phase = output->null->State();
                    if (phase == Audio::Backend::NullAudioBackendState::Priming ||
                        phase == Audio::Backend::NullAudioBackendState::Quiescing)
                        (void)output->null->AdvanceCallback();
                }
            }

            void Activate() {
                REQUIRE(frontend->Start({1, 1'000'000'000}).HasValue());
                REQUIRE(output->null->AdvanceControl().HasValue());
                REQUIRE(frontend->Pump({1, 1'000'000'000}).HasValue());
                REQUIRE(output->null->AdvanceControl().HasValue());
                REQUIRE(frontend->Pump({1, 1'000'000'000}).HasValue());
                CHECK(frontend->Snapshot().phase == AudioFrontendPhase::Starting);
                REQUIRE(output->null->AdvanceCallback().HasValue());
                REQUIRE(frontend->Pump({1, 1'000'000'000}).HasValue());
                REQUIRE(frontend->Snapshot().phase == AudioFrontendPhase::Active);
            }

            void Control(const AudioVoiceControl operation) {
                const auto admitted = frontend->Transport({frontend->Snapshot().voice, operation});
                REQUIRE(admitted.HasValue());
                REQUIRE(admitted.Value().status == AudioCommandStagingStatus::Ok);
                REQUIRE(frontend->Pump({1, 1'000'000'000}).HasValue());
                REQUIRE(output->null->AdvanceCallback().HasValue());
            }
        };

        /** @brief Completes asynchronous fixture teardown even when a fatal test assertion unwinds. */
        template <typename Owner, typename Phase> struct RetireOnExit final {
            Owner &owner;
            CapturingOutput &output;
            Phase closed;

            ~RetireOnExit() {
                owner.Close();
                for (std::size_t step = 0; step < 16 && owner.Snapshot().phase != closed; ++step) {
                    (void)owner.Pump({1, 1'000'000'000});
                    if (owner.Snapshot().phase == closed)
                        break;  // Document pumping may already have destroyed the output owner.
                    (void)output.null->AdvanceControl();
                    const auto phase = output.null->State();
                    if (phase == Audio::Backend::NullAudioBackendState::Priming ||
                        phase == Audio::Backend::NullAudioBackendState::Quiescing)
                        (void)output.null->AdvanceCallback();
                }
            }
        };

    }  // namespace
}  // namespace Horo::Tests::VoiceRenderFixture
