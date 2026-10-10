#include "AudioStreamingTestFixture.h"
#include "AudioVoiceRenderTestFixture.h"
#include "Horo/Audio/Internal/AudioFrontendComposition.h"
#include "Horo/Editor/AudioEditorDocument.h"

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

        Editor::DocumentIdentity Document() {
            return {{Editor::DocumentKind::Asset, Editor::SourceDocumentId::Parse("audio/preview.horoaudio").Value()},
                    Editor::DocumentInstanceId::Create(1).Value()};
        }

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

        TEST_CASE("AudioFrontend closes partial startup without activating a hidden replacement", "[audio][frontend][rollback]") {
            PreviewRig rig;
            SECTION("never opened") {
                CHECK(rig.frontend->Start({2, 1'000'000'000}).HasError());
                CHECK(rig.output->null->State() == Audio::Backend::NullAudioBackendState::Closed);
                rig.frontend->Close();
                REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
            }
            SECTION("open is still pending") {
                REQUIRE(rig.frontend->Start({1, 1'000'000'000}).HasValue());
                rig.frontend->Close();
                REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
                CHECK(rig.frontend->Snapshot().phase == AudioFrontendPhase::Closing);
                REQUIRE(rig.output->null->AdvanceControl().HasValue());
                REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
                REQUIRE(rig.output->null->AdvanceControl().HasValue());
                REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
                CHECK_FALSE(rig.output->primingSilent);  // No callback or voice was ever started.
            }
            SECTION("start is still pending") {
                REQUIRE(rig.frontend->Start({1, 1'000'000'000}).HasValue());
                REQUIRE(rig.output->null->AdvanceControl().HasValue());
                REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
                rig.frontend->Close();
                REQUIRE(rig.output->null->AdvanceControl().HasValue());
                REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
                REQUIRE(rig.output->null->AdvanceCallback().HasValue());
                REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
                REQUIRE(rig.output->null->AdvanceControl().HasValue());
                REQUIRE(rig.output->null->AdvanceCallback().HasValue());
                REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
                REQUIRE(rig.output->null->AdvanceControl().HasValue());
                REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
                REQUIRE(rig.output->null->AdvanceControl().HasValue());
                REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
                CHECK(rig.output->primingSilent);
                CHECK(rig.output->quiescingSilent);
            }
            CHECK(rig.frontend->Snapshot().phase == AudioFrontendPhase::Closed);
            CHECK(rig.output->null->State() == Audio::Backend::NullAudioBackendState::Closed);
            CHECK(rig.registry->Snapshot(rig.frontend->Snapshot().voice).HasError());
            CHECK(rig.frontend->Snapshot().pendingOperations == 0);
            CHECK(rig.frontend->Snapshot().retainedOperationResults == 0);
        }

        TEST_CASE("AudioFrontend retains negotiation failure through output retirement", "[audio][frontend][rollback]") {
            PreviewRig rig;
            rig.output->rejectOpenedFormat = true;
            REQUIRE(rig.frontend->Start({1, 1'000'000'000}).HasValue());
            REQUIRE(rig.output->null->AdvanceControl().HasValue());
            REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasError());
            CHECK(rig.frontend->Snapshot().phase == AudioFrontendPhase::Closing);
            CHECK_FALSE(rig.output->primingSilent);
            REQUIRE(rig.output->null->AdvanceControl().HasValue());
            REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasError());
            CHECK(rig.frontend->Snapshot().phase == AudioFrontendPhase::Closed);
            CHECK(rig.frontend->Snapshot().failure.has_value());
            CHECK(rig.output->null->State() == Audio::Backend::NullAudioBackendState::Closed);
        }

        TEST_CASE("AudioFrontend preserves callback allocation safety and explicit transport retry", "[audio][frontend][realtime-safety]") {
            PreviewRig rig;
            rig.Activate();
            auto foreign = rig.frontend->Snapshot().voice;
            ++foreign.generation;
            CHECK(rig.frontend->Transport({foreign, AudioVoiceControl::Start}).HasError());
            rig.Control(AudioVoiceControl::Start);
            for (std::size_t count = 0; count < 3; ++count)
                REQUIRE(rig.frontend->Transport({rig.frontend->Snapshot().voice, AudioVoiceControl::Pause}).Value().status ==
                        AudioCommandStagingStatus::Ok);
            CHECK(rig.frontend->Transport({rig.frontend->Snapshot().voice, AudioVoiceControl::Pause}).Value().status ==
                  AudioCommandStagingStatus::OrdinaryFull);
            REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
            const auto allocations = AllocationProbe::Count();
            const auto frees = AllocationProbe::FreeCount();
            const auto rendered = rig.output->null->AdvanceCallback();
            const auto allocationEnd = AllocationProbe::Count();
            const auto freeEnd = AllocationProbe::FreeCount();
            REQUIRE(rendered.HasValue());
            CHECK(allocationEnd == allocations);
            CHECK(freeEnd == frees);
            std::array<AudioFrontendOperationResult, 4> results;
            REQUIRE(rig.frontend->DrainTransportResults(results) == 4);
            CHECK(results[0].disposition == AudioFrontendOperationDisposition::Applied);
            CHECK(results[1].disposition == AudioFrontendOperationDisposition::Applied);
            CHECK(results[2].disposition == AudioFrontendOperationDisposition::Rejected);
            CHECK(results[3].disposition == AudioFrontendOperationDisposition::Rejected);
            CHECK(results[2].error != nullptr);
            CHECK(results[2].acceptedSequence != results[3].acceptedSequence);
        }

        TEST_CASE("AudioFrontend retains terminal results until consumer acknowledgement", "[audio][frontend][backpressure]") {
            PreviewRig rig;
            rig.Activate();
            rig.Control(AudioVoiceControl::Start);
            std::array<AudioFrontendOperationResult, 1> result;
            REQUIRE(rig.frontend->DrainTransportResults(result) == 1);
            for (std::size_t index = 0; index < MaximumAudioFrontendOperations; ++index)
                rig.Control(index % 2 == 0 ? AudioVoiceControl::Pause : AudioVoiceControl::Resume);
            CHECK(rig.frontend->Snapshot().pendingOperations == 0);
            CHECK(rig.frontend->Snapshot().retainedOperationResults == MaximumAudioFrontendOperations);
            const auto rejected = rig.frontend->Transport({rig.frontend->Snapshot().voice, AudioVoiceControl::Pause});
            REQUIRE(rejected.HasValue());
            CHECK(rejected.Value().status == AudioCommandStagingStatus::Busy);
            CHECK(rejected.Value().sequence == 0);
            REQUIRE(rig.frontend->DrainTransportResults(result) == 1);
            CHECK(result[0].disposition == AudioFrontendOperationDisposition::Applied);
            rig.Control(AudioVoiceControl::Pause);
            CHECK(rig.frontend->Snapshot().retainedOperationResults == MaximumAudioFrontendOperations);
        }

        TEST_CASE("Audio document validates identity and fences independent authoring revisions", "[editor][audio][document]") {
            CHECK(Editor::AudioEditorDocument::Open({}, 1).HasError());
            CHECK(Editor::AudioEditorDocument::Open(Document(), 0).HasError());
            auto opened = Editor::AudioEditorDocument::Open(Document(), 3);
            REQUIRE(opened.HasValue());
            auto document = std::move(opened).Value();
            CHECK(document.Reload(2, 4).HasError());
            CHECK(document.Reload(3, 3).HasError());
            REQUIRE(document.SetFocused(3, true).HasValue());
            REQUIRE(document.Reload(3, 4).HasValue());
            CHECK(document.Snapshot().sourceRevision == 4);
            CHECK(document.Snapshot().identity == Document());
            CHECK(document.StartPreview(3, {1, 1'000'000'000}).HasError());
            CHECK(document.StartPreview(4, {1, 1'000'000'000}).HasError());
            auto moved = std::move(document);
            CHECK(document.Snapshot().phase == Editor::AudioEditorDocumentPhase::Closed);
            CHECK(moved.Snapshot().sourceRevision == 4);
            moved.Close();
            CHECK(moved.Snapshot().phase == Editor::AudioEditorDocumentPhase::Closed);
            CHECK(moved.Reload(4, 5).HasError());
        }

        /** @brief Pins real job/provider dependencies through every stream port and worker join. */
        struct StreamHost final {
            JobSystem jobs{{.workerCount = 1}};
            Audio::StreamingTests::PackageFixture package;
            std::shared_ptr<AudioStreamingService> service{Audio::StreamingTests::Service(jobs, package)};

            ~StreamHost() {
                (void)service->Shutdown();
            }
        };

        TEST_CASE("AudioFrontend retires stream ports and workers only after native detachment", "[audio][frontend][stream]") {
            auto host = std::make_shared<StreamHost>();
            auto admitted = host->service->Admit(Audio::StreamingTests::Request());
            REQUIRE(admitted.HasValue());
            const auto stream = admitted.Value();
            auto conversion = Playback().plan.Descriptor();
            conversion.channels = 2;
            auto registry = Registry();
            auto createdVoice =
                AudioVoiceRenderRuntime::CreateStream(registry, host->service, stream, conversion, CoefficientBytes, Descriptor());
            REQUIRE(createdVoice.HasValue());
            auto backend = std::make_unique<CapturingOutput>();
            auto *output = backend.get();
            auto candidate = Output(std::move(backend));
            Internal::AudioFrontendResources resources{Scope,         Staging(),     std::move(createdVoice).Value(),
                                                       Runtime(),     Plan(Asset()), Request(),
                                                       host->service, stream,        host};
            auto created = Internal::AudioFrontendComposition::Create(resources, candidate);
            REQUIRE(created.HasValue());
            auto frontend = std::move(created).Value();
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
            frontend->Close();
            REQUIRE(frontend->Pump({1, 1'000'000'000}).HasValue());
            REQUIRE(output->null->AdvanceControl().HasValue());
            CHECK(host->service->Retire(stream).HasError());  // Retained voice port forbids early worker/ring retirement.
            REQUIRE(output->null->AdvanceCallback().HasValue());
            REQUIRE(frontend->Pump({1, 1'000'000'000}).HasValue());
            REQUIRE(output->null->AdvanceControl().HasValue());
            REQUIRE(frontend->Pump({1, 1'000'000'000}).HasValue());
            REQUIRE(output->null->AdvanceControl().HasValue());
            REQUIRE(frontend->Pump({1, 1'000'000'000}).HasValue());
            CHECK(frontend->Snapshot().phase == AudioFrontendPhase::Closed);
            CHECK(host->service->Snapshot(stream).HasError());
            CHECK(registry->Snapshot(frontend->Snapshot().voice).HasError());
            CHECK(host->package.opens.load() == 1);
            CHECK(host->package.releases.load() == 1);
            const std::weak_ptr<StreamHost> lease = host;
            host.reset();
            CHECK(lease.expired());
        }

        TEST_CASE("AudioFrontend performs production PCM transport and native proven teardown", "[audio][frontend][preview]") {
            PreviewRig rig;
            rig.Activate();
            CHECK(rig.output->primingSilent);
            rig.samples.pcm.fill(0.0F);  // Prepared playback owns the immutable source, never producer buffers.
            rig.Control(AudioVoiceControl::Start);
            CHECK(std::abs(rig.output->left[8] - std::sqrt(0.5F)) < 2e-6F);
            rig.Control(AudioVoiceControl::Pause);
            CHECK(rig.output->left[8] == 0.0F);
            rig.Control(AudioVoiceControl::Resume);
            CHECK(rig.output->left[8] > 0.0F);
            rig.frontend->Close();
            CHECK(rig.frontend->Transport({rig.frontend->Snapshot().voice, AudioVoiceControl::Start}).HasError());
            REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
            REQUIRE(rig.output->null->AdvanceControl().HasValue());
            CHECK(rig.frontend->Snapshot().phase == AudioFrontendPhase::Closing);
            REQUIRE(rig.output->null->AdvanceCallback().HasValue());
            REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
            CHECK(rig.output->quiescingSilent);
            CHECK(rig.frontend->Snapshot().phase == AudioFrontendPhase::Closing);  // Silence is not detachment.
            REQUIRE(rig.output->null->AdvanceControl().HasValue());                // Native Stop.
            REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
            REQUIRE(rig.output->null->AdvanceControl().HasValue());  // Device Close.
            REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
            CHECK(rig.frontend->Snapshot().phase == AudioFrontendPhase::Closed);
            CHECK(rig.output->null->State() == Audio::Backend::NullAudioBackendState::Closed);
            CHECK(rig.registry->Snapshot(rig.frontend->Snapshot().voice).HasError());
            CHECK(rig.frontend->Snapshot().pendingOperations == 0);
            CHECK(rig.frontend->Snapshot().retainedOperationResults == 3);
            std::array<AudioFrontendOperationResult, 3> results;
            REQUIRE(rig.frontend->DrainTransportResults(results) == 3);
            for (const auto &result : results) {
                CHECK(result.owner == MixerFixture::Owner);
                CHECK(result.acceptedSequence != 0);
                CHECK(result.disposition == AudioFrontendOperationDisposition::Applied);
            }
            CHECK(rig.frontend->DrainTransportResults(results) == 0);
        }

        TEST_CASE("Audio document fences source changes and retains asynchronous preview until close", "[editor][audio][document]") {
            PreviewRig rig;
            auto opened = Editor::AudioEditorDocument::Open(Document(), 1);
            REQUIRE(opened.HasValue());
            auto document = std::move(opened).Value();
            CHECK(document.AttachPreview(2, rig.frontend).HasError());
            RetireOnExit retirement{document, *rig.output, Editor::AudioEditorDocumentPhase::Closed};
            REQUIRE(rig.frontend != nullptr);
            REQUIRE(document.AttachPreview(1, rig.frontend).HasValue());
            CHECK(document.StartPreview(1, {1, 1'000'000'000}).HasError());
            REQUIRE(document.SetFocused(1, true).HasValue());
            REQUIRE(document.StartPreview(1, {1, 1'000'000'000}).HasValue());
            CompleteOutputStart(document, *rig.output);
            const auto voice = document.Snapshot().preview->voice;
            CHECK(document.Transport(2, {voice, AudioVoiceControl::Start}).HasError());
            REQUIRE(document.Transport(1, {voice, AudioVoiceControl::Start}).Value().status == AudioCommandStagingStatus::Ok);
            REQUIRE(document.Pump({1, 1'000'000'000}).HasValue());
            REQUIRE(rig.output->null->AdvanceCallback().HasValue());
            CHECK(rig.output->left[8] > 0.0F);
            REQUIRE(document.SetFocused(1, false).HasValue());
            CHECK(document.Transport(1, {voice, AudioVoiceControl::Resume}).HasError());
            REQUIRE(document.Transport(1, {voice, AudioVoiceControl::Pause}).HasValue());
            REQUIRE(document.Reload(1, 2).HasValue());
            CHECK(document.Snapshot().previewRevision == 0);
            CHECK(document.Transport(1, {voice, AudioVoiceControl::Start}).HasError());
            CHECK(document.Transport(2, {voice, AudioVoiceControl::Start}).HasError());
            document.Close();
            CHECK(document.Snapshot().phase == Editor::AudioEditorDocumentPhase::Closing);
            REQUIRE(document.Pump({1, 1'000'000'000}).HasValue());
            REQUIRE(rig.output->null->AdvanceControl().HasValue());
            REQUIRE(rig.output->null->AdvanceCallback().HasValue());
            REQUIRE(document.Pump({1, 1'000'000'000}).HasValue());
            REQUIRE(rig.output->null->AdvanceControl().HasValue());
            REQUIRE(document.Pump({1, 1'000'000'000}).HasValue());
            REQUIRE(rig.output->null->AdvanceControl().HasValue());
            REQUIRE(document.Pump({1, 1'000'000'000}).HasValue());
            CHECK_FALSE(document.Snapshot().preview.has_value());
            CHECK(document.Snapshot().phase == Editor::AudioEditorDocumentPhase::Closed);
            CHECK(document.Snapshot().retainedTransportResults == 2);
            std::array<AudioFrontendOperationResult, 1> terminal;
            REQUIRE(document.DrainTransportResults(terminal) == 1);
            CHECK(terminal[0].disposition == AudioFrontendOperationDisposition::Applied);
            REQUIRE(document.DrainTransportResults(terminal) == 1);
            CHECK(terminal[0].disposition == AudioFrontendOperationDisposition::Cancelled);
            CHECK(document.DrainTransportResults(terminal) == 0);
            document.Close();
            CHECK(document.Pump({1, 1'000'000'000}).HasValue());
        }
    }  // namespace
}  // namespace Horo::Tests::VoiceRenderFixture
