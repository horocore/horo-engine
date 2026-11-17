#include "Horo/Audio/CoreAudioDSPNode.h"
#include "Horo/Audio/Internal/OfflineAudioRender.h"
#include "MixerTestFixture.h"

#include <numbers>

using namespace Horo::Audio::Backend;
namespace MF = Horo::Tests::MixerFixture;

namespace {
    /** @brief Real prepared one-pole DSP; the authored default equals the core descriptor's 1000 Hz default. */
    class FilterFactory final : public IMixerDSPFactory {
    public:
        Horo::Result<MixerDSPStrategy> Create(const MixerEffectDescriptor &, const AudioProcessingFormat &format) override {
            return Horo::Result<MixerDSPStrategy>::Success({std::make_unique<CoreAudioDSPNode>(CoreAudioDSPKind::LowPass, format, 16), 64});
        }
    };

    AudioSampleClock Clock(const std::uint64_t frame = 0) {
        return {.owner = MF::Owner,
                .epoch = MF::Scope.epoch,
                .generation = 4,
                .discontinuityRevision = 5,
                .sampleFrame = frame,
                .sampleRate = 48'000};
    }

    /** @brief Tooling-owned aggregate using the shipping mixer, parameter engine and DSP, with no hardware backend. */
    struct OfflineMixer final {
        FilterFactory factory;
        std::unique_ptr<MixerGraphRuntime> mixer = MF::Runtime();
        AudioCommandStaging staging = MF::Staging();
        AudioParameterAutomation automation{Clock(), MF::Scope.scene};
        MF::Block input;
        std::array<MixerVoiceInput, 1> voices;
        AudioCommandRecord publication;
        bool adopt{true};
        std::uint64_t tailFrames{};

        OfflineMixer() {
            auto asset = MF::Asset();
            asset.buses[1].effects.push_back(
                {MF::IdOf<AudioEffectId>(20), MixerEffectKind::LowPass, false, MixerFilterEffectParameters{1'000, 1}});
            auto plan = MF::Plan(asset, 1, &factory);
            const AudioAutomationParameter binding{.address = {.kind = AudioParameterTargetKind::Bus,
                                                               .owner = MF::Owner,
                                                               .bus = MF::IdOf<AudioBusId>(2),
                                                               .bindingGeneration = 1,
                                                               .parameter = MF::IdOf<AudioParameterId>(1)},
                                                   .minimum = 0,
                                                   .maximum = 1,
                                                   .initialValue = 1,
                                                   .maximumSampleDelta = 0.25F};
            REQUIRE(automation.Bind(binding) == AudioAutomationStatus::Ok);
            REQUIRE(automation.Seal() == AudioAutomationStatus::Ok);
            REQUIRE(automation.Schedule({binding.address, 1, 4, 5, 4, 8, 0.5F, AudioAutomationCurve::Linear}) == AudioAutomationStatus::Ok);
            REQUIRE(plan->BindAutomation(std::span(&binding, 1)).HasValue());
            voices[0] = MF::Voice(*plan, input);
            tailFrames = CoreAudioDSPNode(CoreAudioDSPKind::LowPass, {48'000, MF::Profile().outputLayout}, 16).Descriptor().tailFrames;
            publication = MF::Publish(*mixer, staging, plan);
        }

        ~OfflineMixer() {
            mixer->Close();
            (void)mixer->CompleteShutdown(MF::Scope, true);  // RenderOfflineAudio has returned; no asynchronous reader exists.
        }

        static RenderResult Process(void *context, const RenderInvocation &invocation) noexcept {
            auto &self = *static_cast<OfflineMixer *>(context);
            if (invocation.phase == RenderPhase::Priming)
                return {RenderDisposition::Ready, AudioCallbackFaultCode::None};
            if (invocation.phase == RenderPhase::Quiescing)
                return {RenderDisposition::Quiesced, AudioCallbackFaultCode::None};
            const auto frames = invocation.output.validFrames;
            for (std::uint32_t frame = 0; frame < frames; ++frame)
                self.input.left[frame] = self.input.right[frame] = invocation.sampleFrame + frame < 35 ? 0.25F : 0.0F;
            self.voices[0].samples = self.input.View(frames);
            const auto result = self.mixer->RenderAutomated(MF::Scope, self.adopt ? &self.publication : nullptr, self.voices,
                                                            invocation.output, {self.automation, Clock(invocation.sampleFrame)});
            self.adopt = false;
            return result.status == MixerRenderStatus::Rendered ? RenderResult{RenderDisposition::Rendered, AudioCallbackFaultCode::None}
                                                                : RenderResult{};
        }

        Horo::Result<OfflineAudioRenderOutput> Export(const std::uint32_t blockFrames) {
            return RenderOfflineAudio({.epoch = {{MF::Owner, 1, 1}, 1, 1},
                                       .format = {48'000, MF::Profile().outputLayout},
                                       .blockFrames = blockFrames,
                                       .contentFrames = 35,
                                       .tailFrames = tailFrames,
                                       .clockDomain = 7},
                                      {this, Process});
        }
    };

    TEST_CASE("Offline exports shipping mixer automation and complete real DSP tails independently of block partition",
              "[audio][offline][mixer]") {
        OfflineMixer first, second;
        const auto whole = first.Export(16);
        const auto split = second.Export(7);
        REQUIRE(whole.HasValue());
        REQUIRE(split.HasValue());
        REQUIRE(whole.Value().interleaved.size() == (35 + first.tailFrames) * 2);
        REQUIRE(whole.Value().interleaved == split.Value().interleaved);
        REQUIRE(whole.Value().interleaved[35 * 2] > 0.0F);
        REQUIRE(whole.Value().interleaved.back() == 0.0F);
        const auto alpha = std::exp(-2.0 * std::numbers::pi * 1'000 / 48'000);
        const auto expected = 0.25 * (1 - std::pow(alpha, 21)) * 0.5;
        REQUIRE(std::abs(whole.Value().interleaved[20 * 2] - expected) < 2e-6);
    }
}  // namespace
