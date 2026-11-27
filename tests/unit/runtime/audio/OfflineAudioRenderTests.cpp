#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/Internal/OfflineAudioRender.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace Horo::Audio;
using namespace Horo::Audio::Backend;

namespace {
    struct Core final {
        std::uint64_t frames{};
        std::uint32_t primed{};
        std::uint32_t quiesced{};
        std::uint64_t quiescedAt{};
        std::stop_source *cancel{};  // Test-only cancellation injection; no stop callbacks are registered.
        std::uint64_t cancelAt{};
        bool fail{};
        bool nonfinite{};
        bool malformed{};

        static RenderResult Process(void *context, const RenderInvocation &invocation) noexcept {
            auto &core = *static_cast<Core *>(context);
            const auto &block = invocation.output;
            if (invocation.phase == RenderPhase::Priming) {
                ++core.primed;
                return {RenderDisposition::Ready, AudioCallbackFaultCode::None};
            }
            if (invocation.phase == RenderPhase::Quiescing) {
                ++core.quiesced;
                core.quiescedAt = invocation.sampleFrame;
                return {RenderDisposition::Quiesced, AudioCallbackFaultCode::None};
            }
            if (invocation.sampleFrame != core.frames || invocation.startedAt.nanoseconds != core.frames * 1'000'000'000 / 48'000)
                core.malformed = true;
            for (std::size_t channel = 0; channel < block.planes.size(); ++channel)
                for (std::uint32_t frame = 0; frame < block.validFrames; ++frame)
                    block.planes[channel][frame] =
                        core.nonfinite ? std::numeric_limits<float>::infinity() : static_cast<float>((core.frames + frame) * 2 + channel);
            core.frames += block.validFrames;
            if (core.cancel && core.frames >= core.cancelAt)
                core.cancel->request_stop();
            return core.fail ? RenderResult{} : RenderResult{RenderDisposition::Rendered, AudioCallbackFaultCode::None};
        }
    };

    OfflineAudioRenderRequest Request() {
        return {.epoch = {{AudioRuntimeId::Create(4).Value(), 1, 1}, 1, 1},
                .format = {48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)},
                .blockFrames = 16,
                .contentFrames = 35,
                .tailFrames = 7,
                .clockDomain = 7};
    }

    TEST_CASE("Offline produces exact PCM including partial blocks and an explicit tail", "[audio][offline]") {
        Core core;
        const auto output = RenderOfflineAudio(Request(), {&core, Core::Process});
        REQUIRE(output.HasValue());
        REQUIRE(output.Value().interleaved.size() == 84);
        for (std::size_t index = 0; index < output.Value().interleaved.size(); ++index)
            REQUIRE(output.Value().interleaved[index] == static_cast<float>(index));
        REQUIRE(core.primed == 1);
        REQUIRE(core.quiesced == 1);
        REQUIRE_FALSE(core.malformed);
        auto request = Request();
        request.blockFrames = 7;
        Core split;
        const auto partitioned = RenderOfflineAudio(request, {&split, Core::Process});
        REQUIRE(partitioned.HasValue());
        REQUIRE(partitioned.Value().interleaved == output.Value().interleaved);
        REQUIRE_FALSE(split.malformed);
    }

    TEST_CASE("Offline rejects overflow and budget excess before invoking a render core", "[audio][offline]") {
        Core core;
        auto request = Request();
        request.maximumOutputBytes = 32;
        REQUIRE(RenderOfflineAudio(request, {&core, Core::Process}).HasError());
        request = Request();
        request.contentFrames = std::numeric_limits<std::uint64_t>::max();
        REQUIRE(RenderOfflineAudio(request, {&core, Core::Process}).HasError());
        request = Request();
        request.blockFrames = 0;
        REQUIRE(RenderOfflineAudio(request, {&core, Core::Process}).HasError());
        REQUIRE(core.primed == 0);
    }

    TEST_CASE("Offline faults return no partial export and synchronously quiesce", "[audio][offline]") {
        Core core;
        core.fail = true;
        REQUIRE(RenderOfflineAudio(Request(), {&core, Core::Process}).HasError());
        REQUIRE(core.quiesced == 1);
        core = {};
        core.nonfinite = true;
        REQUIRE(RenderOfflineAudio(Request(), {&core, Core::Process}).HasError());
        REQUIRE(core.quiesced == 1);
    }

    TEST_CASE("Cancelled offline jobs never enter the prepared render core", "[audio][offline]") {
        std::stop_source cancellation;
        cancellation.request_stop();
        auto request = Request();
        request.cancellation = cancellation.get_token();
        Core core;
        REQUIRE(RenderOfflineAudio(request, {&core, Core::Process}).HasError());
        REQUIRE(core.primed == 0);
        REQUIRE(core.frames == 0);
    }

    TEST_CASE("Offline cancellation after a processed block quiesces at that cursor without publishing partial PCM", "[audio][offline]") {
        std::stop_source cancellation;
        auto request = Request();
        request.cancellation = cancellation.get_token();
        Core core;
        core.cancel = &cancellation;
        core.cancelAt = request.blockFrames;
        const auto output = RenderOfflineAudio(request, {&core, Core::Process});
        REQUIRE(output.HasError());
        REQUIRE(output.ErrorValue().code.Value() == Horo::Audio::AudioErrors::OperationCancelled.code.Value());
        REQUIRE(core.frames == request.blockFrames);
        REQUIRE(core.quiesced == 1);
        REQUIRE(core.quiescedAt == core.frames);
        REQUIRE_FALSE(core.malformed);
    }
}  // namespace
