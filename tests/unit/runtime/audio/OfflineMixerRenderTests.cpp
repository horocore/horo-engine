#include "Horo/Application/Internal/OfflineAudioExport.h"
#include "Horo/Audio/CoreAudioDSPNode.h"
#include "Horo/Platform/NativePublicationFileSystem.h"
#include "MixerTestFixture.h"
#include "OwnedTestDirectory.h"

#include <bit>
#include <numbers>

using namespace Horo::Audio::Backend;
using namespace Horo::Application::Internal;
namespace MF = Horo::Tests::MixerFixture;

namespace {
    constexpr std::uint64_t ContentFrames = 35;
    constexpr std::uint32_t ClipFrames = 256;

    /** @brief Prepare the real core DSP strategy at the admitted mixer format. */
    class FilterFactory final : public IMixerDSPFactory {
    public:
        Horo::Result<MixerDSPStrategy> Create(const MixerEffectDescriptor &, const AudioProcessingFormat &format) override {
            auto node = std::make_unique<CoreAudioDSPNode>(CoreAudioDSPKind::LowPass, format, MF::Profile().maximumFrames);
            tailFrames = node->Descriptor().tailFrames;
            return Horo::Result<MixerDSPStrategy>::Success({std::move(node), 64});
        }

        std::uint32_t tailFrames{};
    };

    AudioSampleClock Clock() {
        return {.owner = MF::Owner,
                .epoch = MF::Scope.epoch,
                .generation = 4,
                .discontinuityRevision = 5,
                .sampleRate = 48'000,
                .observedAt = {7, 0}};
    }

    /** @brief Prepare resident playback, a normal mixer publication and its sealed automation for each independent export. */
    struct OfflineMixer final {
        FilterFactory factory;
        std::unique_ptr<MixerGraphRuntime> mixer = MF::Runtime();
        AudioCommandStaging staging = MF::Staging();
        AudioParameterAutomation automation{Clock(), MF::Scope.scene};
        std::unique_ptr<AudioVoiceRenderRuntime> voice;
        std::array<AudioVoiceRenderRuntime *, 1> voices{};
        AudioCommandRecord publication;

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

            auto registry = AudioVoiceStateMachine::Create({MF::Owner, 1});
            REQUIRE(registry.HasValue());
            const auto conversion = AudioResamplerPlan::Prepare({.quality = AudioResamplerQuality::Linear,
                                                                 .inputRate = 48'000,
                                                                 .outputRate = 48'000,
                                                                 .channels = 1,
                                                                 .maximumOutputFrames = MF::Profile().maximumFrames},
                                                                {1'000'000, 1'000'000, 4096});
            REQUIRE(conversion.HasValue());
            alignas(64) std::array<float, ClipFrames> samples;
            samples.fill(0.25F);
            const std::array<std::span<const float>, 1> planes{samples};
            auto created =
                AudioVoiceRenderRuntime::CreateResident(std::make_shared<AudioVoiceStateMachine>(std::move(registry).Value()),
                                                        {planes, ClipFrames, true},
                                                        {conversion.Value(), {true, 0, ClipFrames}, 1, 1'000'000, 16 * 1024, 1.0F},
                                                        {MF::Scope, MF::IdOf<AudioMemoryPoolId>(58), MF::IdOf<AudioParameterId>(59),
                                                         MF::Profile().maximumFrames});
            REQUIRE(created.HasValue());
            voice = std::move(created).Value();
            voices[0] = voice.get();

            AudioVoiceRenderRequest route;
            route.bus = MF::IdOf<AudioBusId>(2);
            route.source.identity = {MF::Scope.scene, 1, 1};
            route.source.playback.spatialMode = AudioSpatialMode::TwoD;
            route.spatial.smoothingFrames = 0;
            REQUIRE(voice->Publish(route, *plan, staging).Value().status == AudioCommandStagingStatus::Ok);
            REQUIRE(voice->Apply(MF::ConsumePublished(staging)) == nullptr);
            REQUIRE(staging.Submit({MF::Scope, AudioStartVoiceCommand{voice->Voice()}}).status == AudioCommandStagingStatus::Ok);
            REQUIRE(voice->Apply(MF::ConsumePublished(staging)) == nullptr);
            publication = MF::Publish(*mixer, staging, plan);
        }

        ~OfflineMixer() {
            // The caller exclusively owns the synchronous render path, so no future callback can enter.
            mixer->Close();
            (void)mixer->CompleteShutdown(MF::Scope, true);
            if (voice) {
                voice->Close();
                (void)voice->CompleteShutdown(true);
            }
        }

        OfflineAudioRenderRequest Request(const std::uint32_t blockFrames) const {
            return {.epoch = {{MF::Owner, 1, 1}, 1, 1},
                    .format = {48'000, MF::Profile().outputLayout},
                    .blockFrames = blockFrames,
                    .contentFrames = ContentFrames,
                    .tailFrames = factory.tailFrames,
                    .clockDomain = 7};
        }

        OfflineMixerSource Source() {
            return {MF::Scope, Clock(), *mixer, publication, voices, &automation};
        }
    };

    TEST_CASE("Offline exports resident PCM, sample automation and real DSP tails independently of block partition",
              "[audio][offline][mixer]") {
        OfflineMixer first, second;
        const auto whole = RenderOfflineAudio(first.Request(16), first.Source());
        const auto split = RenderOfflineAudio(second.Request(7), second.Source());
        REQUIRE(whole.HasValue());
        REQUIRE(split.HasValue());
        REQUIRE(whole.Value().interleaved.size() == (ContentFrames + first.factory.tailFrames) * 2);
        CHECK(whole.Value().interleaved == split.Value().interleaved);
        CHECK(whole.Value().interleaved[ContentFrames * 2] > 0.0F);
        CHECK(whole.Value().interleaved.back() == 0.0F);
        CHECK(first.voice->Cursor().frame == ContentFrames);
        CHECK(second.voice->Cursor().frame == ContentFrames);
        const auto alpha = std::exp(-2.0 * std::numbers::pi * 1'000 / 48'000);
        const auto expected = 0.25 * std::sqrt(0.5) * (1 - std::pow(alpha, 21)) * 0.5;
        CHECK(std::abs(whole.Value().interleaved[20 * 2] - expected) < 2e-6);
    }

    TEST_CASE("Prepared offline mixer preserves fresh state when sample clock evidence is foreign", "[audio][offline][mixer]") {
        OfflineMixer offline;
        auto source = offline.Source();
        ++source.initialClock.observedAt.clockDomain;
        const auto invalid = RenderOfflineAudio(offline.Request(7), source);
        REQUIRE(invalid.HasError());
        CHECK(invalid.ErrorValue().code.Value() == AudioErrors::CommandBufferInvalid.code.Value());
        CHECK(offline.voice->Cursor().frame == 0);
        CHECK(RenderOfflineAudio(offline.Request(7), offline.Source()).HasValue());
    }

    TEST_CASE("Offline WAVE publication encodes the exact prepared mixer PCM and tail", "[audio][offline][mixer][export]") {
        OfflineMixer bufferOwner, fileOwner;
        const auto pcm = RenderOfflineAudio(bufferOwner.Request(16), bufferOwner.Source());
        REQUIRE(pcm.HasValue());

        Horo::Tests::OwnedTestDirectory directory{"offline-mixer-wave"};
        const auto anchor = std::filesystem::canonical(directory.Path());
        const auto output = anchor / "exports";
        auto authority = Horo::Platform::CreateNativePublicationFileSystem(anchor, "exports");
        REQUIRE(authority.HasValue());
        auto &files = *authority.Value();
        auto writer = files.TryAcquireExclusive(output / ".writer.lock", "offline mixer wave test");
        REQUIRE(writer.HasValue());
        const auto staging = output / OfflineAudioWaveStagingDirectoryName;
        REQUIRE(files.CreateDirectoryPath(staging, DirectoryCreationMode::Single).HasValue());
        const OfflineAudioWaveExportRequest request{fileOwner.Request(7), staging / "mixer.partial", output / "mixer.wav"};
        OfflineAudioWaveExportReceipt receipt;
        const auto result = ExportOfflineAudioWave(request, fileOwner.Source(), files, receipt);
        REQUIRE(result.HasValue());
        CHECK(receipt.publication.WasCommitted());
        CHECK(result.Value().tailFrames == pcm.Value().tailFrames);
        CHECK(fileOwner.voice->Cursor().frame == ContentFrames);
        constexpr std::size_t WaveDataOffset = 80;
        const auto expectedBytes = WaveDataOffset + pcm.Value().interleaved.size() * sizeof(float);
        const auto read = files.ReadFileBounded(request.destination, expectedBytes);
        if (read.HasError())
            FAIL(read.ErrorValue().message);
        const auto &bytes = read.Value();
        REQUIRE(bytes.size() == expectedBytes);
        for (std::size_t sample = 0; sample < pcm.Value().interleaved.size(); ++sample) {
            std::uint32_t encoded{};
            for (std::size_t byte = 0; byte < sizeof(encoded); ++byte)
                encoded |= static_cast<std::uint32_t>(bytes[WaveDataOffset + sample * sizeof(encoded) + byte]) << (byte * 8);
            if (encoded != std::bit_cast<std::uint32_t>(pcm.Value().interleaved[sample])) {
                CAPTURE(sample);
                FAIL("WAVE sample differs from the offline mixer PCM");
            }
        }
    }
}  // namespace
