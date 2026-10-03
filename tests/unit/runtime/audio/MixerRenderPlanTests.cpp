#include "MixerTestFixture.h"

using namespace Horo::Tests::MixerFixture;

TEST_CASE("Mixer pulls retained pre and post fader taps without erasing earlier source output", "[audio][mixer]") {
    MixerAssetSchema asset = Asset(true);
    asset.buses[1].defaults.gainDb = -6.0206F;
    asset.buses[2].role = MixerBusRole::Return;
    asset.routes.push_back({IdOf<AudioRouteId>(80), IdOf<AudioBusId>(2), IdOf<AudioBusId>(3), MixerRouteKind::Send,
                            MixerSendTap::PostInsertPreFader, -6.0206F});
    Probe probe;
    Factory factory(probe);
    auto plan = Plan(asset, 1, &factory);
    Signal signal(plan, 1);
    REQUIRE(signal.publication.runtime->Reconcile().generation == 0);
    const std::size_t allocations = Horo::Tests::AllocationProbe::Count();
    const std::size_t frees = Horo::Tests::AllocationProbe::FreeCount();
    probe.callback = true;
    const MixerRenderResult result = signal.AdoptAndRender();
    probe.callback = false;
    REQUIRE(Horo::Tests::AllocationProbe::Count() == allocations);
    REQUIRE(Horo::Tests::AllocationProbe::FreeCount() == frees);
    REQUIRE(result.status == MixerRenderStatus::Rendered);
    for (const float value : signal.output.left)
        REQUIRE(std::abs(value - 2.0F) < 2e-6F);
    const std::array<MixerVoiceInput, 1> illegalReturnVoice{MixerVoiceInput{{Owner, 1, 1}, 1, 1, signal.input.View()}};
    REQUIRE(signal.publication.runtime->Render(Scope, nullptr, illegalReturnVoice, signal.output.View()).status ==
            MixerRenderStatus::InvalidBuffer);
    REQUIRE(signal.publication.runtime->Reconcile().generation == 1);
    REQUIRE(probe.destroyed.load() == 0);
    ShutDown(*signal.publication.runtime, signal.output);
    REQUIRE(probe.destroyed.load() == 1);
    REQUIRE(probe.destroyedOnCallback.load() == 0);
}

TEST_CASE("Mixer process faults and old voice generations silence output while retaining prepared ownership", "[audio][mixer]") {
    Probe probe;
    Factory factory(probe);
    auto plan = Plan(Asset(true), 1, &factory);
    Signal signal(plan, 1);
    probe.fault.store(true, std::memory_order_relaxed);
    REQUIRE(signal.AdoptAndRender().status == MixerRenderStatus::DSPFault);
    REQUIRE(signal.output.left.front() == 0);
    REQUIRE(probe.destroyed.load() == 0);
    const auto simultaneous = signal.AdoptAndRender();
    REQUIRE(simultaneous.status == MixerRenderStatus::DSPFault);
    REQUIRE(simultaneous.commandRejected);
    probe.fault.store(false, std::memory_order_relaxed);
    REQUIRE(signal.publication.runtime->Render(Scope, nullptr, signal.voices, signal.output.View()).status == MixerRenderStatus::Rendered);
    signal.voices[0].graphGeneration = 99;
    REQUIRE(signal.publication.runtime->Render(Scope, nullptr, signal.voices, signal.output.View()).status ==
            MixerRenderStatus::InvalidBuffer);
    REQUIRE(signal.output.left.front() == 0);
    ShutDown(*signal.publication.runtime, signal.output);
}

TEST_CASE("Mixer bus mute pause disabled routes and bypass have explicit unchanged topology semantics", "[audio][mixer]") {
    MixerAssetSchema asset = Asset(true);
    SECTION("Mute") {
        asset.buses[1].defaults.muted = true;
    }
    SECTION("Pause suppresses supplied direct voices") {
        asset.buses[1].defaults.paused = true;
    }
    SECTION("Bypass preserves input at unity") {
        asset.buses[1].effects.front().bypassed = true;
    }
    SECTION("Disabled send is omitted") {
        asset.routes.push_back(
            {IdOf<AudioRouteId>(90), IdOf<AudioBusId>(2), IdOf<AudioBusId>(3), MixerRouteKind::Send, MixerSendTap::PostFader, 0, false});
        asset.buses[1].effects.front().bypassed = true;
    }
    Probe probe;
    Factory factory(probe);
    auto plan = Plan(asset, 1, &factory);
    REQUIRE(plan->Routes().size() == 5);
    Signal signal(plan, 0.5F);
    REQUIRE(signal.AdoptAndRender().status == MixerRenderStatus::Rendered);
    const float expected = asset.buses[1].defaults.muted || asset.buses[1].defaults.paused ? 0.0F : 0.5F;
    REQUIRE(signal.output.left.front() == expected);
    ShutDown(*signal.publication.runtime, signal.output);
}

TEST_CASE("Mixer stable voice order and hostile buffers fail without mutating plan ownership", "[audio][mixer]") {
    auto plan = Plan(Asset());
    Block input, output;
    input.Fill(1);
    std::array<MixerVoiceInput, 2> voices{Voice(*plan, input, 2, 1), Voice(*plan, input, 2, 2)};
    SECTION("Duplicate slots") {
        voices[1].voice.slot = 1;
    }
    SECTION("Foreign owner") {
        voices[0].voice.owner = IdOf<AudioRuntimeId>(99);
    }
    SECTION("Unbounded slot") {
        voices[0].voice.slot = MaximumAudioHandleSlots + 1;
    }
    SECTION("Stale generation") {
        voices[0].graphGeneration = 99;
    }
    SECTION("Missing bus") {
        voices[0].busIndex = 99;
    }
    SECTION("Insufficient input") {
        voices[0].samples.validFrames = 0;
    }
    SECTION("Malformed storage") {
        voices[0].samples.planes = {};
    }
    Publication publication(plan);
    REQUIRE(publication.AdoptAndRender(voices, output.View()).status == MixerRenderStatus::InvalidBuffer);
    REQUIRE(output.left.front() == 0);
    REQUIRE(publication.runtime->Reconcile().generation == 1);
    ShutDown(*publication.runtime, output);
}

TEST_CASE("Mixer nonfinite or overflowing signal faults silence and reset rather than clamp internal headroom", "[audio][mixer]") {
    auto plan = Plan(Asset());
    Block input, output;
    input.Fill(std::numeric_limits<float>::max());
    SECTION("NaN") {
        input.left[0] = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("Infinity") {
        input.left[0] = std::numeric_limits<float>::infinity();
    }
    SECTION("Accumulation overflow") {}
    std::array<MixerVoiceInput, 2> voices{Voice(*plan, input, 2, 1), Voice(*plan, input, 2, 2)};
    Publication publication(plan);
    REQUIRE(publication.AdoptAndRender(voices, output.View()).status == MixerRenderStatus::DSPFault);
    REQUIRE(output.left.front() == 0);
    input.Fill(2);
    REQUIRE(publication.runtime->Render(Scope, nullptr, voices, output.View()).status == MixerRenderStatus::Rendered);
    REQUIRE(output.left.front() == 4);
    ShutDown(*publication.runtime, output);
}

TEST_CASE("Mixer repeated inserts retain canonical pre fader data across odd and even chain lengths", "[audio][mixer]") {
    MixerAssetSchema asset = Asset(true);
    asset.buses[1].effects.push_back({IdOf<AudioEffectId>(21), MixerEffectKind::Gain, false, MixerGainEffectParameters{-6.0206F}});
    Probe probe;
    Factory factory(probe);
    auto plan = Plan(asset, 1, &factory);
    Signal signal(plan, 1);
    REQUIRE(signal.AdoptAndRender().status == MixerRenderStatus::Rendered);
    REQUIRE(std::abs(signal.output.left.front() - 1) < 2e-6F);
    ShutDown(*signal.publication.runtime, signal.output);
    REQUIRE(probe.destroyed.load() == 2);
}

TEST_CASE("Mixer short blocks preserve positive zero padding and zero length cannot adopt a candidate", "[audio][mixer]") {
    auto plan = Plan(Asset());
    Signal signal(plan, 1);
    REQUIRE(signal.AdoptAndRender(0).status == MixerRenderStatus::InvalidBuffer);
    REQUIRE(signal.publication.runtime->Reconcile().generation == 0);
    signal.output.Fill(-7);
    REQUIRE(signal.AdoptAndRender(4).status == MixerRenderStatus::Rendered);
    for (std::size_t i = 0; i < 16; ++i)
        REQUIRE(signal.output.left[i] == (i < 4 ? 1.0F : 0.0F));
    REQUIRE_FALSE(std::signbit(signal.output.left.back()));
    ShutDown(*signal.publication.runtime, signal.output);
}

TEST_CASE("Mixer validates exact DSP disposition frame tail and finite output before publishing taps", "[audio][mixer]") {
    Probe probe;
    SECTION("Unknown disposition") {
        probe.status = static_cast<AudioDSPProcessStatus>(99);
    }
    SECTION("Incomplete process") {
        probe.wrongFrames = true;
    }
    SECTION("Undeclared tail") {
        probe.tail = 17;
    }
    SECTION("Nonfinite output") {
        probe.nonFinite = true;
    }
    Factory factory(probe);
    auto plan = Plan(Asset(true), 1, &factory);
    Publication publication(plan);
    Block output;
    REQUIRE(publication.runtime->Render(Scope, &publication.record, {}, output.View()).status == MixerRenderStatus::DSPFault);
    REQUIRE(output.left.front() == 0);
    REQUIRE(probe.destroyed.load() == 0);
    ShutDown(*publication.runtime, output);
}

TEST_CASE("Mixer route and bus fader overflow fault before publishing unsafe Master output", "[audio][mixer]") {
    MixerAssetSchema asset = Asset();
    SECTION("Route multiplication overflow") {
        asset.routes.front().gainDb = 20;
    }
    SECTION("Bus fader overflow") {
        asset.buses[1].defaults.gainDb = 20;
    }
    auto plan = Plan(asset);
    Signal signal(plan, std::numeric_limits<float>::max() / 4.0F);
    REQUIRE(signal.AdoptAndRender().status == MixerRenderStatus::DSPFault);
    REQUIRE(signal.output.left.front() == 0);
    ShutDown(*signal.publication.runtime, signal.output);
}

TEST_CASE("Mixer rejects excess voices and malformed runtime preparation without partial activation", "[audio][mixer]") {
    MixerRuntimeDescriptor descriptor{Scope, IdOf<AudioMemoryPoolId>(33), Profile()};
    SECTION("Runtime owner") {
        descriptor.scope.owner = {};
    }
    SECTION("Runtime epoch") {
        descriptor.scope.epoch = 0;
    }
    SECTION("Foreign scene") {
        descriptor.scope.scene.owner = IdOf<AudioRuntimeId>(99);
    }
    SECTION("Pool identity") {
        descriptor.storageIdentity = {};
    }
    SECTION("Retained budget") {
        descriptor.maximumRetainedBytes = 0;
    }
    SECTION("Invalid profile") {
        descriptor.profile.maximumFrames = 0;
    }
    REQUIRE(MixerGraphRuntime::Create(descriptor).HasError());
    auto plan = Plan(Asset());
    Block input, output;
    std::array<MixerVoiceInput, 9> voices;
    for (std::uint32_t i = 0; i < voices.size(); ++i)
        voices[i] = Voice(*plan, input, 2, i + 1);
    Publication publication(plan);
    REQUIRE(publication.AdoptAndRender(voices, output.View()).status == MixerRenderStatus::InvalidBuffer);
    REQUIRE(output.left.front() == 0);
    ShutDown(*publication.runtime, output);
}

namespace {
    /** @brief Exercise canonical layout interpretation with an aligned, fixed-extent 64-plane fixture. */
    void CheckLayout(const AudioChannelLayout &layout) {
        MixerCompileProfile profile = Profile();
        profile.outputLayout = layout;
        MixerAssetSchema asset = Asset();
        for (auto &bus : asset.buses)
            bus.layout = layout;
        auto plan = Plan(asset, 1, nullptr, profile);
        alignas(64) std::array<std::array<float, 16>, MaximumAudioChannels> samples{};
        std::array<float *, MaximumAudioChannels> planes{};
        for (std::size_t i = 0; i < layout.orderedChannels.size(); ++i) {
            samples[i].fill(static_cast<float>(i + 1) * 0.01F);
            planes[i] = samples[i].data();
        }
        const AudioPlanarBlockView block{ViewAudioChannelLayout(layout), 48'000, {planes.data(), layout.orderedChannels.size()}, 16, 16};
        const std::array<MixerVoiceInput, 1> voices{MixerVoiceInput{{Owner, 1, 1}, 1, *plan->ResolveBus(IdOf<AudioBusId>(2)), block}};
        auto runtime = Runtime(profile);
        auto staging = Staging();
        const AudioCommandRecord swap = Publish(*runtime, staging, plan);
        REQUIRE(runtime->Render(Scope, &swap, voices, block).status == MixerRenderStatus::Rendered);
        for (std::size_t i = 0; i < layout.orderedChannels.size(); ++i)
            REQUIRE(samples[i].front() == static_cast<float>(i + 1) * 0.01F);
        runtime->Close();
        REQUIRE(runtime->Render(Scope, nullptr, {}, block).status == MixerRenderStatus::Quiesced);
        REQUIRE(runtime->CompleteShutdown(Scope, true) == MixerRenderStatus::Quiesced);
    }
}  // namespace

TEST_CASE("Mixer preserves every admitted speaker discrete and canonical Ambisonic layout without reinterpretation", "[audio][mixer]") {
    using enum AudioSpeakerPreset;
    for (const auto preset : std::array{Mono, Stereo, TwoPointOne, Quad, FivePointOne, SevenPointOne, SevenPointOneFour})
        CheckLayout(MakeAudioSpeakerLayout(preset));
    for (const std::uint8_t channels : std::array<std::uint8_t, 2>{1, 64}) {
        AudioChannelLayout layout{AudioLayoutKind::Discrete, {}, {}};
        for (std::uint8_t index = 0; index < channels; ++index)
            layout.orderedChannels.emplace_back(AudioDiscreteChannel{index});
        CheckLayout(layout);
    }
    for (std::uint8_t order = 0; order < 4; ++order) {
        AudioChannelLayout layout{AudioLayoutKind::Ambisonic, {}, AmbisonicDescriptor{order}};
        for (std::uint8_t acn = 0; acn < (order + 1) * (order + 1); ++acn)
            layout.orderedChannels.emplace_back(AudioAmbisonicChannel{acn});
        CheckLayout(layout);
    }
}
