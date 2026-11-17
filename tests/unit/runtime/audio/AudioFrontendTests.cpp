#include "AudioStreamingTestFixture.h"
#include "AudioVoiceRenderTestFixture.h"
#include "Horo/Audio/Internal/AudioFrontendComposition.h"
#include "Horo/Editor/AudioEditorDocument.h"

#include <bit>

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
            bool foreignOpenedClock{};
            bool rejectAcknowledgement{};
            std::size_t callbackEntries{};
            std::size_t renderingCommits{}, renderingEntries{};

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
                ++renderingCommits;
                return null->CommitRendering(epoch);
            }

            Result<Audio::Backend::CancelDisposition> Cancel(const Audio::Backend::OperationId &operation) override {
                return null->Cancel(operation);
            }

            Result<std::optional<Audio::Backend::Completion>> Poll(const Audio::Backend::OperationId &operation) override {
                auto completion = null->Poll(operation);
                if ((!rejectOpenedFormat && !foreignOpenedClock) || completion.HasError())
                    return completion;
                auto captured = std::move(completion).Value();
                if (captured.has_value())
                    if (auto *opened = std::get_if<Audio::Backend::Opened>(&captured->outcome)) {
                        if (rejectOpenedFormat)
                            opened->format.callbackFrames = 32;
                        if (foreignOpenedClock)
                            ++opened->timing.capturedAt.clockDomain;
                    }
                return Result<std::optional<Audio::Backend::Completion>>::Success(std::move(captured));
            }

            Result<void> AcknowledgeCompletion(const Audio::Backend::OperationId &operation) override {
                if (rejectAcknowledgement)
                    return Result<void>::Failure(MakeError(AudioErrors::BackendFailed));
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
                if (invocation.phase == Audio::Backend::RenderPhase::Rendering)
                    ++self.renderingEntries;
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
            enum class Composition {
                Closed,
                Opened,
                DeferOpened,
                DeferPendingOpen,
                DeferProbe
            };
            std::shared_ptr<AudioVoiceStateMachine> registry = Registry();
            Audio::PlaybackTest::SampleBuffers samples;
            CapturingOutput *output{};
            std::unique_ptr<AudioFrontend> frontend;
            std::optional<Audio::Backend::OperationId> preparationOpen;
            std::optional<Internal::AudioFrontendResources> deferredResources;
            std::optional<Internal::AudioFrontendOutput> deferredOutput;

            explicit PreviewRig(const Composition composition = Composition::Closed) {
                auto backend = std::make_unique<CapturingOutput>();
                output = backend.get();
                auto candidate = Output(std::move(backend));
                if (composition != Composition::Closed) {
                    const Audio::Backend::Request request = composition == Composition::DeferProbe
                                                                ? Audio::Backend::Request{Audio::Backend::Probe{}}
                                                                : Audio::Backend::Request{candidate.open};
                    preparationOpen = output->Begin(request, {1, 1'000'000'000}).Value();
                    if (composition != Composition::DeferPendingOpen)
                        REQUIRE(output->null->AdvanceControl().HasValue());
                    candidate.openOperation = preparationOpen;
                }
                auto voice = AudioVoiceRenderRuntime::CreateResident(registry, samples.Source(), Playback(), Descriptor());
                REQUIRE(voice.HasValue());
                Internal::AudioFrontendResources resources{Scope, Staging(), std::move(voice).Value(), Runtime(), Plan(Asset()), Request()};
                if (composition != Composition::Closed && composition != Composition::Opened) {
                    deferredResources.emplace(std::move(resources));
                    deferredOutput.emplace(std::move(candidate));
                    return;
                }
                auto created = Internal::AudioFrontendComposition::Create(resources, candidate);
                REQUIRE(created.HasValue());
                frontend = std::move(created).Value();
            }

            ~PreviewRig() {
                if (!frontend) {
                    RetireDeferred();
                    return;
                }
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

            /** @brief Reconcile rejected, never-attached test candidates even when a fatal assertion unwinds. */
            void RetireDeferred() {
                if (!deferredOutput || !deferredOutput->backend)
                    return;
                output->rejectAcknowledgement = false;
                (void)output->Cancel(*preparationOpen);
                const auto polled = output->Poll(*preparationOpen);
                if (polled.HasValue() && polled.Value())
                    (void)output->AcknowledgeCompletion(*preparationOpen);
                const auto close = output->Begin(Audio::Backend::Close{}, {1, 1'000'000'000});
                if (close.HasValue()) {
                    (void)output->null->AdvanceControl();
                    (void)output->AcknowledgeCompletion(close.Value());
                }
                auto &owned = *deferredResources;
                owned.voice->Close();
                (void)owned.voice->CompleteShutdown(true);  // Deferred fixture preparation never attached a render port.
                owned.mixer->Close();
                (void)owned.mixer->CompleteShutdown(owned.scope, true);
                (void)owned.staging.Close();
                AudioCommandRecord record;
                for (std::size_t step = 0; step <= MaximumAudioCommandSlots && !owned.staging.IsDrained(); ++step) {
                    (void)owned.staging.Pump(64);
                    while (owned.staging.TryConsume(record)) {
                    }
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

        TEST_CASE("AudioFrontend rejects invalid retained Open admission without consuming caller owners or receipts",
                  "[audio][frontend][opened-composition]") {
            auto mode = PreviewRig::Composition::DeferOpened;
            const ErrorCodeDescriptor *expected = &AudioErrors::RuntimeInactive;
            bool foreign{}, stale{}, invalidFormat{}, foreignClock{}, failedAcknowledgement{};
            SECTION("pending Open is not an admission proof") {
                mode = PreviewRig::Composition::DeferPendingOpen;
            }
            SECTION("foreign runtime receipt is not polled or consumed") {
                foreign = true;
                expected = &AudioErrors::IdentityInvalid;
            }
            SECTION("acknowledged stale Open cannot be replayed") {
                stale = true;
                expected = &AudioErrors::HandleStale;
            }
            SECTION("completed Probe cannot stand in for Open") {
                mode = PreviewRig::Composition::DeferProbe;
            }
            SECTION("actual period must match the preserved request and prepared graph") {
                invalidFormat = true;
                expected = &AudioErrors::DeviceFormatUnsupported;
            }
            SECTION("failed acknowledgement retains caller ownership after detached publication") {
                failedAcknowledgement = true;
                expected = &AudioErrors::BackendFailed;
            }
            SECTION("actual timing from another clock domain cannot be adopted") {
                foreignClock = true;
                expected = &AudioErrors::DeviceFormatUnsupported;
            }
            PreviewRig rig(mode);
            auto &candidate = *rig.deferredOutput;
            auto &resources = *rig.deferredResources;
            if (foreign)
                candidate.openOperation->owner = AudioRuntimeId::Create(9999).Value();
            if (stale)
                REQUIRE(rig.output->AcknowledgeCompletion(*rig.preparationOpen).HasValue());
            rig.output->rejectOpenedFormat = invalidFormat;
            rig.output->foreignOpenedClock = foreignClock;
            rig.output->rejectAcknowledgement = failedAcknowledgement;
            const auto offered = candidate.openOperation;
            auto created = Internal::AudioFrontendComposition::Create(resources, candidate);
            const bool rejected = created.HasError();
            if (!rejected)
                rig.frontend = std::move(created).Value();  // Preserve explicit teardown even if the regression unexpectedly admits it.
            REQUIRE(rejected);
            CHECK(created.ErrorValue().code.Value() == expected->code.Value());
            CHECK(candidate.backend != nullptr);
            CHECK(candidate.openOperation == offered);
            CHECK(resources.voice != nullptr);
            CHECK(resources.mixer != nullptr);
            CHECK(rig.output->callbackEntries == 0);
            if (!failedAcknowledgement)
                CHECK(resources.plan != nullptr);
            const auto original = rig.output->Poll(*rig.preparationOpen);
            if (stale)
                CHECK(original.HasError());
            else {
                REQUIRE(original.HasValue());
                CHECK(original.Value().has_value() == (mode != PreviewRig::Composition::DeferPendingOpen));
            }
        }

        TEST_CASE("AudioFrontend adopts a real retained Open receipt without opening again or starting implicitly",
                  "[audio][frontend][opened-composition]") {
            PreviewRig rig(PreviewRig::Composition::Opened);
            REQUIRE(rig.preparationOpen);
            CHECK(rig.output->Poll(*rig.preparationOpen).HasError());
            REQUIRE(rig.frontend->Snapshot().phase == AudioFrontendPhase::Prepared);
            REQUIRE(rig.output->null->State() == Audio::Backend::NullAudioBackendState::Opened);
            REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
            CHECK(rig.output->null->State() == Audio::Backend::NullAudioBackendState::Opened);
            CHECK(rig.output->callbackEntries == 0);
            SECTION("explicit Start attaches the already-open candidate") {
                REQUIRE(rig.frontend->Start({1, 1'000'000'000}).HasValue());
                REQUIRE(rig.output->null->AdvanceControl().HasValue());
                REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
                REQUIRE(rig.output->null->AdvanceCallback().HasValue());
                REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
                CHECK(rig.frontend->Snapshot().phase == AudioFrontendPhase::Active);
                CHECK(rig.output->primingSilent);
            }
            SECTION("Close before Start retires the open output without a callback") {
                rig.frontend->Close();
                REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
                REQUIRE(rig.output->null->AdvanceControl().HasValue());
                REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
                CHECK(rig.frontend->Snapshot().phase == AudioFrontendPhase::Closed);
                CHECK(rig.output->null->State() == Audio::Backend::NullAudioBackendState::Closed);
                CHECK(rig.output->callbackEntries == 0);
            }
        }

        TEST_CASE("AudioFrontend retires priming without waiting for Ready or committing rendering during Close",
                  "[audio][frontend][rollback]") {
            PreviewRig rig(PreviewRig::Composition::Opened);
            REQUIRE(rig.frontend->Start({1, 1'000'000'000}).HasValue());
            REQUIRE(rig.output->null->AdvanceControl().HasValue());
            REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
            bool loss{};
            SECTION("no Ready was ever produced") {}
            SECTION("Ready is queued when Close is requested") {
                REQUIRE(rig.output->null->AdvanceCallback().HasValue());
            }
            SECTION("device is lost before any Ready") {
                loss = true;
                REQUIRE(rig.output->null->InjectDeviceLoss(Audio::Backend::DeviceLossCause::Disconnected).HasValue());
            }
            if (!loss)
                rig.frontend->Close();
            const auto closing = rig.frontend->Pump({1, 1'000'000'000});
            CHECK(closing.HasError() == loss);
            CHECK(rig.output->renderingCommits == 0);
            REQUIRE(rig.output->null->AdvanceControl().HasValue());
            REQUIRE(rig.output->null->State() == Audio::Backend::NullAudioBackendState::Quiescing);
            REQUIRE(rig.output->null->AdvanceCallback().HasValue());
            CHECK(rig.frontend->Pump({1, 1'000'000'000}).HasError() == loss);
            REQUIRE(rig.output->null->AdvanceControl().HasValue());
            CHECK(rig.frontend->Pump({1, 1'000'000'000}).HasError() == loss);
            REQUIRE(rig.output->null->AdvanceControl().HasValue());
            CHECK(rig.frontend->Pump({1, 1'000'000'000}).HasError() == loss);
            CHECK(rig.frontend->Snapshot().phase == AudioFrontendPhase::Closed);
            CHECK(rig.output->null->State() == Audio::Backend::NullAudioBackendState::Closed);
            CHECK(rig.output->quiescingSilent);
            CHECK(rig.output->renderingCommits == 0);
            CHECK(rig.output->renderingEntries == 0);
            if (loss) {
                REQUIRE(rig.frontend->Snapshot().failure);
                CHECK(rig.frontend->Snapshot().failure->code.Value() == AudioErrors::DeviceLost.code.Value());
            }
        }

        TEST_CASE("AudioFrontend closes partial startup without activating a hidden replacement", "[audio][frontend][rollback]") {
            PreviewRig rig;
            SECTION("never opened") {
                CHECK(rig.frontend->Start({2, 1'000'000'000}).HasError());
                CHECK(rig.output->null->State() == Audio::Backend::NullAudioBackendState::Closed);
                rig.frontend->Close();
                const auto retired = rig.frontend->Pump({1, 1'000'000'000});
                REQUIRE(retired.HasError());
                CHECK(retired.ErrorValue().code.Value() == AudioErrors::IdentityInvalid.code.Value());
                REQUIRE(rig.frontend->Snapshot().failure.has_value());
                CHECK(rig.frontend->Snapshot().failure->code.Value() == AudioErrors::IdentityInvalid.code.Value());
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

        TEST_CASE("AudioFrontend holds the actual completed playback cursor while its output clock keeps running",
                  "[audio][frontend][device-handoff]") {
            using Composition = Internal::AudioFrontendComposition;
            PreviewRig rig;
            rig.Activate();
            rig.Control(AudioVoiceControl::Start);
            const auto voice = rig.frontend->Snapshot().voice;
            const auto hold = Composition::RequestPlaybackHold(*rig.frontend);
            REQUIRE(hold.HasValue());
            const auto pending = Composition::PollPlaybackHold(*rig.frontend, hold.Value());
            REQUIRE(pending.HasValue());
            CHECK_FALSE(pending.Value());
            CHECK(Composition::ReleasePlaybackHold(*rig.frontend, hold.Value()).HasError());
            CHECK(rig.frontend->Transport({voice, AudioVoiceControl::Pause}).HasError());
            CHECK(Composition::RequestPlaybackHold(*rig.frontend).HasError());
            auto foreign = hold.Value();
            foreign.owner = AudioRuntimeId::Create(9999).Value();
            CHECK(Composition::PollPlaybackHold(*rig.frontend, foreign).HasError());
            CHECK(Composition::ReleasePlaybackHold(*rig.frontend, foreign).HasError());
            const auto allocations = AllocationProbe::Count();
            const auto frees = AllocationProbe::FreeCount();
            const auto faded = rig.output->null->AdvanceCallback();
            const auto allocationEnd = AllocationProbe::Count();
            const auto freeEnd = AllocationProbe::FreeCount();
            REQUIRE(faded.HasValue());
            CHECK(allocationEnd == allocations);
            CHECK(freeEnd == frees);
            const auto completed = Composition::PollPlaybackHold(*rig.frontend, hold.Value());
            REQUIRE(completed.HasValue());
            REQUIRE(completed.Value());
            const auto boundary = *completed.Value();
            CHECK(boundary.operation == hold.Value());
            CHECK(boundary.epoch == rig.frontend->Snapshot().device);
            CHECK(boundary.cursor.frame == 32);
            CHECK(boundary.cursor.fraction == 0.0);
            CHECK(boundary.sampleFrame == faded.Value().clock.sampleFrame);
            CHECK(rig.output->left.front() > 0.0F);
            CHECK(std::bit_cast<std::uint32_t>(rig.output->left.back()) == 0);
            CHECK(std::is_sorted(rig.output->left.rbegin(), rig.output->left.rend()));
            for (std::size_t block = 0; block < 4; ++block) {
                REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
                const auto silent = rig.output->null->AdvanceCallback();
                REQUIRE(silent.HasValue());
                CHECK(silent.Value().clock.sampleFrame > boundary.sampleFrame);
                CHECK(std::ranges::all_of(rig.output->left, [](const float sample) {
                    return std::bit_cast<std::uint32_t>(sample) == 0;
                }));
                const auto retained = Composition::PollPlaybackHold(*rig.frontend, hold.Value());
                REQUIRE(retained.HasValue());
                REQUIRE(retained.Value());
                CHECK(retained.Value()->cursor.frame == boundary.cursor.frame);
                CHECK(retained.Value()->cursor.fraction == boundary.cursor.fraction);
                CHECK(retained.Value()->sampleFrame == boundary.sampleFrame);
            }
            CHECK(rig.frontend->Snapshot().voice == voice);
            CHECK(rig.frontend->Snapshot().retainedOperationResults == 1);  // Start remains acknowledged, not silently cancelled.
            REQUIRE(Composition::ReleasePlaybackHold(*rig.frontend, hold.Value()).HasValue());
            CHECK(Composition::PollPlaybackHold(*rig.frontend, hold.Value()).HasError());
            REQUIRE(rig.output->null->AdvanceCallback().HasValue());
            CHECK(std::bit_cast<std::uint32_t>(rig.output->left.front()) == 0);
            CHECK(rig.output->left.back() > 0.0F);
            CHECK(std::is_sorted(rig.output->left.begin(), rig.output->left.end()));
            const auto again = Composition::RequestPlaybackHold(*rig.frontend);
            REQUIRE(again.HasValue());
            CHECK(again.Value().sequence > hold.Value().sequence);
            CHECK(Composition::ReleasePlaybackHold(*rig.frontend, hold.Value()).HasError());
            REQUIRE(rig.output->null->AdvanceCallback().HasValue());
            const auto nextBoundary = Composition::PollPlaybackHold(*rig.frontend, again.Value());
            REQUIRE(nextBoundary.HasValue());
            REQUIRE(nextBoundary.Value());
            CHECK(nextBoundary.Value()->cursor.frame == boundary.cursor.frame + 32);
        }

        TEST_CASE("AudioFrontend hold admission preserves ordered accepted transport and never fabricates a missing callback",
                  "[audio][frontend][device-handoff]") {
            using Composition = Internal::AudioFrontendComposition;
            PreviewRig rig;
            CHECK(Composition::RequestPlaybackHold(*rig.frontend).HasError());
            rig.Activate();
            const auto command = rig.frontend->Transport({rig.frontend->Snapshot().voice, AudioVoiceControl::Start});
            REQUIRE(command.HasValue());
            REQUIRE(command.Value().status == AudioCommandStagingStatus::Ok);
            const auto rejected = Composition::RequestPlaybackHold(*rig.frontend);
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == AudioErrors::QueueSaturated.code.Value());
            REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
            REQUIRE(rig.output->null->AdvanceCallback().HasValue());
            std::array<AudioFrontendOperationResult, 1> results;
            REQUIRE(rig.frontend->DrainTransportResults(results) == 1);
            CHECK(results[0].acceptedSequence == command.Value().sequence);
            CHECK(results[0].disposition == AudioFrontendOperationDisposition::Applied);
            const auto hold = Composition::RequestPlaybackHold(*rig.frontend);
            REQUIRE(hold.HasValue());
            SECTION("Control pumping without a callback never acknowledges playback hold") {
                for (std::size_t pump = 0; pump < 4; ++pump) {
                    REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
                    const auto pending = Composition::PollPlaybackHold(*rig.frontend, hold.Value());
                    REQUIRE(pending.HasValue());
                    CHECK_FALSE(pending.Value());
                }
            }
            SECTION("A new hold before any resumed callback retains the same completed source boundary") {
                REQUIRE(rig.output->null->AdvanceCallback().HasValue());
                const auto prior = Composition::PollPlaybackHold(*rig.frontend, hold.Value()).Value();
                REQUIRE(prior);
                REQUIRE(Composition::ReleasePlaybackHold(*rig.frontend, hold.Value()).HasValue());
                const auto next = Composition::RequestPlaybackHold(*rig.frontend);
                REQUIRE(next.HasValue());
                REQUIRE(rig.output->null->AdvanceCallback().HasValue());
                const auto held = Composition::PollPlaybackHold(*rig.frontend, next.Value()).Value();
                REQUIRE(held);
                CHECK(held->cursor.frame == prior->cursor.frame);
                CHECK(held->sampleFrame == prior->sampleFrame);
                CHECK(held->operation == next.Value());
                CHECK(Composition::PollPlaybackHold(*rig.frontend, hold.Value()).HasError());
            }
            rig.frontend->Close();
            CHECK(Composition::PollPlaybackHold(*rig.frontend, hold.Value()).HasError());
            CHECK(Composition::ReleasePlaybackHold(*rig.frontend, hold.Value()).HasError());
            // Fixture teardown must still obtain actual native Quiesce/Stop/Close; a hold is no retirement proof.
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

        TEST_CASE("AudioFrontend operation identities are independent of startup records and survive receipt reuse",
                  "[audio][frontend][device-handoff]") {
            PreviewRig rig;
            rig.Activate();
            const std::array controls{AudioVoiceControl::Start, AudioVoiceControl::Pause, AudioVoiceControl::Pause,
                                      AudioVoiceControl::Resume};
            for (std::size_t index = 0; index < controls.size(); ++index) {
                const auto admitted = rig.frontend->Transport({rig.frontend->Snapshot().voice, controls[index]});
                REQUIRE(admitted.HasValue());
                REQUIRE(admitted.Value().status == AudioCommandStagingStatus::Ok);
                CHECK(admitted.Value().sequence == index + 1);
            }
            const auto full = rig.frontend->Transport({rig.frontend->Snapshot().voice, AudioVoiceControl::Pause});
            REQUIRE(full.HasValue());
            CHECK(full.Value().status == AudioCommandStagingStatus::OrdinaryFull);
            CHECK(full.Value().sequence == 0);
            REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
            REQUIRE(rig.output->null->AdvanceCallback().HasValue());
            std::array<AudioFrontendOperationResult, 4> results;
            REQUIRE(rig.frontend->DrainTransportResults(results) == results.size());
            for (std::size_t index = 0; index < results.size(); ++index)
                CHECK(results[index].acceptedSequence == index + 1);
            CHECK(results[0].disposition == AudioFrontendOperationDisposition::Applied);
            CHECK(results[1].disposition == AudioFrontendOperationDisposition::Applied);
            CHECK(results[2].disposition == AudioFrontendOperationDisposition::Rejected);
            CHECK(results[3].disposition == AudioFrontendOperationDisposition::Applied);
            REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
            const auto next = rig.frontend->Transport({rig.frontend->Snapshot().voice, AudioVoiceControl::Pause});
            REQUIRE(next.HasValue());
            REQUIRE(next.Value().status == AudioCommandStagingStatus::Ok);
            CHECK(next.Value().sequence == 5);
            REQUIRE(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
            REQUIRE(rig.output->null->AdvanceCallback().HasValue());
            REQUIRE(rig.frontend->DrainTransportResults(results) == 1);
            CHECK(results[0].acceptedSequence == next.Value().sequence);
            CHECK(results[0].disposition == AudioFrontendOperationDisposition::Applied);
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
            const auto callbacks = rig.output->callbackEntries;
            CHECK(rig.output->null->AdvanceCallback().HasError());
            CHECK(rig.output->callbackEntries == callbacks);
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
