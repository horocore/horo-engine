#include "AudioFrontendTestFixture.h"
#include "Horo/Editor/AudioEditorDocument.h"

namespace Horo::Tests::VoiceRenderFixture {
    namespace {
        Editor::DocumentIdentity Document() {
            return {{Editor::DocumentKind::Asset, Editor::SourceDocumentId::Parse("audio/preview.horoaudio").Value()},
                    Editor::DocumentInstanceId::Create(1).Value()};
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

        TEST_CASE("Null device loss closes active playback and reconciles pending controls before releasing voice ownership",
                  "[audio][frontend][lifecycle][qualification]") {
            PreviewRig rig;
            rig.Activate();
            rig.Control(AudioVoiceControl::Start);
            const auto handle = rig.frontend->Snapshot().voice;
            const auto pending = rig.frontend->Transport({handle, AudioVoiceControl::Pause});
            REQUIRE(pending.HasValue());
            REQUIRE(pending.Value().status == AudioCommandStagingStatus::Ok);
            REQUIRE(rig.output->null->InjectDeviceLoss(Audio::Backend::DeviceLossCause::Disconnected).HasValue());
            const auto loss = rig.frontend->Pump({1, 1'000'000'000});
            REQUIRE(loss.HasError());
            CHECK(loss.ErrorValue().code.Value() == AudioErrors::DeviceLost.code.Value());
            CHECK(rig.frontend->Snapshot().phase == AudioFrontendPhase::Closing);
            CHECK_FALSE(rig.frontend->Snapshot().terminalVoice.has_value());
            CHECK(rig.registry->Snapshot(handle).HasValue());
            CHECK(rig.frontend->Transport({handle, AudioVoiceControl::Resume}).HasError());
            REQUIRE(rig.output->null->AdvanceControl().HasValue());
            REQUIRE(rig.output->null->AdvanceCallback().HasValue());
            CHECK(rig.frontend->Pump({1, 1'000'000'000}).HasError());
            CHECK(rig.registry->Snapshot(handle).HasValue());  // Silence/quiescence alone cannot release a voice.
            CHECK_FALSE(rig.frontend->Snapshot().terminalVoice.has_value());
            REQUIRE(rig.output->null->AdvanceControl().HasValue());
            CHECK(rig.frontend->Pump({1, 1'000'000'000}).HasError());
            REQUIRE(rig.output->null->AdvanceControl().HasValue());
            CHECK(rig.frontend->Pump({1, 1'000'000'000}).HasError());
            REQUIRE(rig.frontend->Snapshot().phase == AudioFrontendPhase::Closed);
            REQUIRE(rig.frontend->Snapshot().terminalVoice.has_value());
            CHECK(rig.frontend->Snapshot().terminalVoice->voice == handle);
            CHECK(rig.frontend->Snapshot().terminalVoice->state == AudioVoiceState::Cancelled);
            CHECK(rig.frontend->Snapshot().terminalVoice->terminalReason == AudioVoiceTerminalReason::Cancelled);
            CHECK(rig.registry->Snapshot(handle).HasError());
            CHECK(rig.frontend->Snapshot().pendingOperations == 0);
            std::array<AudioFrontendOperationResult, 2> results;
            REQUIRE(rig.frontend->DrainTransportResults(results) == 2);
            CHECK(results[0].disposition == AudioFrontendOperationDisposition::Applied);
            CHECK(results[1].acceptedSequence == pending.Value().sequence);
            CHECK(results[1].disposition == AudioFrontendOperationDisposition::Cancelled);
            CHECK(rig.frontend->DrainTransportResults(results) == 0);
            CHECK(rig.output->null->AdvanceCallback().HasError());
            CHECK(rig.frontend->Pump({1, 1'000'000'000}).HasValue());
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
