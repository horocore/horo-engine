#include "AudioPreviewHostTestSupport.h"

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Editor::AudioPreviewTest;

    /** @brief Injects one control-driver failure after a real Null callback, without replacing Audio processing. */
    EditorAudioPreviewOutputFactory OneShotFailingOutput(const std::shared_ptr<OutputEvidence> &capture, const std::shared_ptr<bool> &armed,
                                                         const Error &failure) {
        return [capture, armed, failure](Audio::AudioRuntimeId owner) -> Result<EditorAudioPreviewBackend> {
            auto prepared = ObservedOutput(capture)(owner);
            if (prepared.HasError())
                return Result<EditorAudioPreviewBackend>::Failure(prepared.ErrorValue());
            auto output = std::move(prepared).Value();
            auto callback = std::move(output.advanceCallback);
            output.advanceCallback = [callback = std::move(callback), armed, failure]() -> Result<void> {
                auto result = callback();
                if (result.HasError())
                    return result;
                if (*armed) {
                    *armed = false;
                    return Result<void>::Failure(failure);
                }
                return result;
            };
            return Result<EditorAudioPreviewBackend>::Success(std::move(output));
        };
    }

    /** @brief Original error text and immutable cause identity survive asynchronous native cleanup. */
    void RequireOriginalError(const Error &actual, const Error &expected) {
        CHECK(actual.domain.Value() == expected.domain.Value());
        CHECK(actual.code.Value() == expected.code.Value());
        CHECK(actual.message == expected.message);
        CHECK(actual.cause.Get() == expected.cause.Get());
    }

    /** @brief Rejected admission cannot rewrite the active owner's copied status or native identity. */
    void RequireRejectedStartPreservesCapture(EditorAudioPreviewHost &host, const AudioComponentPreviewRequest &request,
                                              const ErrorCodeDescriptor &expected) {
        const auto before = host.Snapshot();
        const auto rejected = host.Start(request);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == expected.code.Value());
        CHECK(rejected.ErrorValue().domain.Value() == expected.domain.Value());
        const auto after = host.Snapshot();
        CHECK(after.workspace == before.workspace);
        CHECK(after.revision == before.revision);
        CHECK(after.object == before.object);
        REQUIRE(after.output.has_value() == before.output.has_value());
        REQUIRE(after.output);
        CHECK(after.output->phase == before.output->phase);
        CHECK(after.output->owner == before.output->owner);
        CHECK(after.output->voice == before.output->voice);
        CHECK(after.output->device == before.output->device);
        CHECK(after.output->pendingOperations == before.output->pendingOperations);
        CHECK(after.output->retainedOperationResults == before.output->retainedOperationResults);
        CHECK(after.output->failure.has_value() == before.output->failure.has_value());
        REQUIRE_FALSE(before.failure);
        CHECK_FALSE(after.failure);
    }

    /** @brief Requires callback processing to stay active while no native control request is pending. */
    void RequireSteadyPreview(EditorAudioPreviewHost &host, const OutputEvidence &capture) {
        const auto blocksBeforeIdle = capture.renderingBlocks;
        for (std::size_t i = 0; i < 20; ++i) {
            Pump(host, 1);
            REQUIRE(host.Snapshot().output->phase == Audio::AudioFrontendPhase::Active);
        }
        CHECK(capture.renderingBlocks > blocksBeforeIdle);
        CHECK(capture.finite);
        CHECK(capture.peak > 0.01F);
        CHECK(capture.peak <= 1.0F);
        CHECK(capture.idleControlRejections == 0);
    }

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
        RequireSteadyPreview(host, *capture);
        RequireRejectedStartPreservesCapture(host, request, AudioComponentPreviewErrors::Busy);
        auto rejected = request;
        rejected.workspace = project.clip;
        RequireRejectedStartPreservesCapture(host, rejected, AudioComponentPreviewErrors::Busy);
        rejected.workspace = {};
        RequireRejectedStartPreservesCapture(host, rejected, AudioComponentPreviewErrors::Stale);
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

    TEST_CASE("Inspector Pump returns current driver failure and preserves its original cause through clean retirement",
              "[audio][editor][preview][headless]") {
        ClipProject project;
        const auto capture = std::make_shared<OutputEvidence>();
        const auto armed = std::make_shared<bool>(false);
        const auto failure = WithCause(MakeError(Audio::AudioErrors::BackendFailed, "One-shot callback driver failure"),
                                       MakeError(Audio::AudioErrors::DeviceUnavailable, "Original device cause"));
        EditorAudioPreviewHost host{OneShotFailingOutput(capture, armed, failure)};
        REQUIRE(host.Start(project.Request()).HasValue());
        Pump(host);
        REQUIRE(host.Snapshot().output->phase == Audio::AudioFrontendPhase::Active);
        REQUIRE(capture->peak > 0.01F);
        *armed = true;
        const auto failed = host.Pump();
        REQUIRE(failed.HasError());
        RequireOriginalError(failed.ErrorValue(), failure);
        REQUIRE(host.Snapshot().failure);
        RequireOriginalError(*host.Snapshot().failure, failure);
        Pump(host);  // Past failure does not turn successful cleanup progress into endless failures.
        REQUIRE(host.Snapshot().output->phase == Audio::AudioFrontendPhase::Closed);
        REQUIRE(host.Snapshot().failure);
        RequireOriginalError(*host.Snapshot().failure, failure);
        CHECK(capture->destroyed);
        CHECK(capture->idleControlRejections == 0);
    }
}  // namespace
