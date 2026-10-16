#include "AudioPreviewHostTestSupport.h"

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Editor::AudioPreviewTest;

    TEST_CASE("Inspector preview decodes registered WAV through real Null frontend and retires output",
              "[audio][editor][preview][headless]") {
        ClipProject project;
        const auto capture = std::make_shared<OutputEvidence>();
        EditorAudioPreviewHost host{ObservedOutput(capture)};
        auto request = project.Request();
        REQUIRE(host.Start(request).HasValue());
        Pump(host);
        REQUIRE(host.Snapshot().output);
        CHECK(host.Snapshot().output->phase == Audio::AudioFrontendPhase::Active);
        CHECK(capture->renderingBlocks > 0);
        CHECK(capture->finite);
        CHECK(capture->peak > 0.01F);
        CHECK(capture->peak <= 1.0F);
        const auto firstVoice = host.Snapshot().output->voice;
        host.Stop(Assets::AssetId{});
        Pump(host, 1);
        CHECK(host.Snapshot().output->phase == Audio::AudioFrontendPhase::Active);
        host.Stop(project.workspace);
        REQUIRE(host.Start(request).HasError());
        Pump(host);
        CHECK(host.Snapshot().output->phase == Audio::AudioFrontendPhase::Closed);
        CHECK(capture->destroyed);
        const auto finalBlocks = capture->renderingBlocks;
        Pump(host);
        CHECK(capture->renderingBlocks == finalBlocks);
        REQUIRE(host.Start(request).HasValue());
        Pump(host);
        CHECK(host.Snapshot().output->voice.owner != firstVoice.owner);
        host.Stop(project.workspace);
        Pump(host);
    }

    TEST_CASE("Inspector preview reports missing assets listeners providers and buses explicitly", "[audio][editor][preview][headless]") {
        ClipProject project;
        EditorAudioPreviewHost host{MakeNullAudioPreviewOutput};
        auto request = project.Request();
        const ErrorCodeDescriptor *expected = nullptr;
        SECTION("missing registered asset") {
            request.assets = {};
            expected = &AudioComponentPreviewErrors::MissingAsset;
        }
        SECTION("missing authored listener") {
            request.source.playback.spatialMode = Audio::AudioSpatialMode::ThreeD;
            expected = &AudioComponentPreviewErrors::MissingListener;
        }
        SECTION("missing provider family") {
            request.source.sound = Audio::AudioSoundReference::ForStream(Audio::AudioSoundId::Create(project.clip).Value()).Value();
            expected = &AudioComponentPreviewErrors::ProviderUnavailable;
        }
        SECTION("missing bus is never rerouted") {
            request.source.playback.bus = Audio::AudioBusId::Create(999).Value();
            expected = &AudioComponentPreviewErrors::MissingBus;
        }
        REQUIRE(expected);
        auto result = host.Start(request);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == expected->code.Value());
        CHECK_FALSE(host.Snapshot().output);
    }

    TEST_CASE("Inspector preview keeps active output owned until application shutdown proves detachment",
              "[audio][editor][preview][headless]") {
        ClipProject project;
        const auto capture = std::make_shared<OutputEvidence>();
        {
            EditorAudioPreviewHost host{ObservedOutput(capture)};
            REQUIRE(host.Start(project.Request()).HasValue());
            Pump(host);
            CHECK(capture->peak > 0.01F);
            CHECK_FALSE(capture->destroyed);
        }
        CHECK(capture->destroyed);
    }

    TEST_CASE("Unavailable explicit preview output selection never becomes Null implicitly", "[audio][editor][preview][headless]") {
        ClipProject project;
        EditorAudioPreviewHost host{EditorAudioPreviewOutputFactory{}};
        auto result = host.Start(project.Request());
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == AudioComponentPreviewErrors::ProviderUnavailable.code.Value());
        CHECK_FALSE(host.Snapshot().output);
    }

    TEST_CASE("Inspector discovery reconciles failed and incomplete real Null operations before destruction",
              "[audio][editor][preview][headless]") {
        ClipProject project;
        const auto capture = std::make_shared<OutputEvidence>();
        bool failAdvance = false;
        SECTION("original advance failure survives cancellation") {
            failAdvance = true;
        }
        SECTION("incomplete discovery is cancelled instead of discarded") {}
        EditorAudioPreviewHost host{[capture, failAdvance](Audio::AudioRuntimeId owner) -> Result<EditorAudioPreviewBackend> {
            auto backend = std::make_unique<ObservedNull>(owner, capture);
            // Leave actual accepted Enumerate pending; the host must reconcile the real operation.
            return Result<EditorAudioPreviewBackend>::Success({std::move(backend), [failAdvance] {
                return failAdvance ? Result<void>::Failure(MakeError(Audio::AudioErrors::BackendFailed)) : Result<void>::Success();
            }});
        }};
        const auto started = host.Start(project.Request());
        REQUIRE(started.HasError());
        const auto expected =
            failAdvance ? Audio::AudioErrors::BackendFailed.code.Value() : AudioComponentPreviewErrors::ProviderUnavailable.code.Value();
        CHECK(started.ErrorValue().code.Value() == expected);
        REQUIRE(host.Snapshot().failure);
        CHECK(host.Snapshot().failure->code.Value() == expected);
        CHECK_FALSE(host.Snapshot().output);
        CHECK(capture->cancellations == 1);
        CHECK(capture->acknowledgements == 1);
        CHECK(capture->closedAtDestruction);
        CHECK(capture->destroyed);
        CHECK(capture->renderingBlocks == 0);
    }
}  // namespace
