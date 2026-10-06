#include "Horo/Audio/CoreAudioDSPNode.h"
#include "Horo/Audio/MixerSnapshot.h"
#include "MixerTestFixture.h"

#include <catch2/catch_approx.hpp>

namespace {
    namespace MF = Horo::Tests::MixerFixture;
    using namespace Horo::Audio;

    /** @brief Owned real core gain strategy whose descriptor preserves the authored mixer default. */
    class SnapshotGain final : public IAudioDSPNode {
    public:
        SnapshotGain(const AudioProcessingFormat &format, const MixerEffectDescriptor &effect)
            : core_(CoreAudioDSPKind::Gain, format, 16), descriptor_(core_.Descriptor()) {
            descriptor_.parameters[0].defaultValue = std::pow(10.0F, std::get<MixerGainEffectParameters>(effect.parameters).gainDb / 20.0F);
        }

        const AudioDSPNodeDescriptor &Descriptor() const noexcept override {
            return descriptor_;
        }

        Horo::Result<void> Prepare(const AudioDSPPrepareContext &context) override {
            return core_.Prepare(context);
        }

        AudioDSPProcessResult Process(const AudioDSPProcessContext &context) noexcept override {
            return core_.Process(context);
        }

        void Reset() noexcept override {
            core_.Reset();
        }

    private:
        CoreAudioDSPNode core_;
        AudioDSPNodeDescriptor descriptor_;
    };

    class SnapshotFactory final : public IMixerDSPFactory {
    public:
        Horo::Result<MixerDSPStrategy> Create(const MixerEffectDescriptor &effect, const AudioProcessingFormat &format) override {
            return Horo::Result<MixerDSPStrategy>::Success({std::make_unique<SnapshotGain>(format, effect), 64});
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

    AudioCommandTarget Target(const std::uint64_t frame) {
        return {.kind = AudioCommandTargetKind::ExactSampleFrame, .sampleFrame = frame, .clockGeneration = 4, .discontinuityRevision = 5};
    }

    struct Fixture {
        SnapshotFactory factory;
        std::unique_ptr<MixerRenderPlan> plan;
        std::array<AudioAutomationParameter, 3> bindings;
        MixerSnapshotAsset snapshot;
        AudioParameterAutomation automation{Clock(), MF::Scope.scene};
        MixerSnapshotTransitions transitions{automation};
        MF::Block input, output;
        std::array<MixerVoiceInput, 1> voices;
        std::unique_ptr<MixerGraphRuntime> runtime = MF::Runtime();
        AudioCommandStaging staging = MF::Staging();
        AudioCommandRecord swap;
        float initialEffect{};

        explicit Fixture(const bool muted = false) {
            auto asset = MF::Asset(true);
            asset.buses[1].defaults.muted = muted;
            asset.buses[2].role = MixerBusRole::Return;
            asset.routes.push_back({MF::IdOf<AudioRouteId>(80), MF::IdOf<AudioBusId>(2), MF::IdOf<AudioBusId>(3), MixerRouteKind::Send,
                                    MixerSendTap::PostFader, 0});
            plan = MF::Plan(asset, 1, &factory);
            std::array<std::uint8_t, 16> mixer{};
            mixer[0] = 8;
            snapshot.mixer = Horo::Assets::AssetId::FromBytes(mixer);
            std::copy_n("dialogue", 8, snapshot.name.begin());
            snapshot.parameterCount = 3;
            initialEffect = std::pow(10.0F, 6.0206F / 20.0F);
            for (std::size_t index = 0; index < 3; ++index) {
                auto &parameter = snapshot.parameters[index];
                parameter.kind = static_cast<AudioParameterTargetKind>(index + 1);
                parameter.bus = MF::IdOf<AudioBusId>(2);
                parameter.parameter = MF::IdOf<AudioParameterId>(1);
                parameter.value = index == 1 ? 0.25F : 0.5F;
                if (index == 1)
                    parameter.send = MF::IdOf<AudioRouteId>(80);
                if (index == 2)
                    parameter.effect = MF::IdOf<AudioEffectId>(20);
                bindings[index] = {.address = {.kind = parameter.kind,
                                               .owner = MF::Owner,
                                               .bus = parameter.bus,
                                               .send = parameter.send,
                                               .effect = parameter.effect,
                                               .bindingGeneration = 1,
                                               .parameter = parameter.parameter},
                                   .minimum = 0,
                                   .maximum = 16,
                                   .initialValue = index == 2 ? initialEffect : 1.0F,
                                   .maximumSampleDelta = 0.2F};
                REQUIRE(automation.Bind(bindings[index]) == AudioAutomationStatus::Ok);
            }
            REQUIRE(automation.Seal() == AudioAutomationStatus::Ok);
            REQUIRE(plan->BindAutomation(bindings).HasValue());
            voices[0] = MF::Voice(*plan, input);
        }

        ~Fixture() {
            runtime->Close();
            static_cast<void>(runtime->CompleteShutdown(MF::Scope, true));  // This synchronous fixture has no native/in-flight callback.
        }

        PreparedMixerSnapshot Prepare(const std::uint64_t id = 1, const std::uint64_t first = 1, const std::uint64_t frame = 4) {
            PreparedMixerSnapshot prepared;
            REQUIRE(PrepareMixerSnapshot(snapshot, snapshot.mixer, bindings,
                                         {MF::Scope, Target(frame), id, first, 0, 160, AudioAutomationCurve::Smoothstep},
                                         prepared) == MixerSnapshotStatus::Ok);
            return prepared;
        }

        void Publish() {
            swap = MF::Publish(*runtime, staging, plan);
        }

        MixerRenderResult Render(const std::uint64_t first, const std::uint32_t frames, const PreparedMixerSnapshot *prepared = nullptr,
                                 const bool adopt = false) {
            for (std::uint32_t index = 0; index < frames; ++index)
                input.left[index] = input.right[index] = 1.0F + static_cast<float>(first + index) * 0.001F;
            voices[0].samples = input.View(frames);
            return runtime->RenderAutomated(MF::Scope, adopt ? &swap : nullptr, voices, output.View(frames),
                                            {automation, Clock(first), &transitions, prepared});
        }
    };
}  // namespace

TEST_CASE("Mixer snapshots project bus send and real DSP gain into compiled graph at an interior sample atomically",
          "[audio][mixer][snapshot][realtime]") {
    Fixture fixture;
    const auto immutableGain = fixture.plan->Buses()[0].gain;
    const auto immutableRoutes = std::vector<MixerCompiledRoute>{fixture.plan->Routes().begin(), fixture.plan->Routes().end()};
    MixerRenderPlan *const retained = fixture.plan.get();
    auto prepared = fixture.Prepare();
    fixture.Publish();
    const AudioMemoryHandle storage{MF::Owner, MF::IdOf<AudioMemoryPoolId>(34), 1, 1};
    AudioCommand publication;
    REQUIRE(MakeScheduledAudioBatchCommand(prepared.batch, storage, publication) == ScheduledAudioCommandBatchStatus::Ok);
    REQUIRE(fixture.staging.Submit(publication).status == AudioCommandStagingStatus::Ok);
    const auto record = MF::ConsumePublished(fixture.staging);
    REQUIRE(std::get<AudioScheduledBatchCommand>(record.command.payload).storage == storage);
    const auto allocations = Horo::Tests::AllocationProbe::Count();
    const auto frees = Horo::Tests::AllocationProbe::FreeCount();
    const auto result = fixture.Render(0, 16, &prepared, true);
    const auto finalAllocations = Horo::Tests::AllocationProbe::Count();
    const auto finalFrees = Horo::Tests::AllocationProbe::FreeCount();
    REQUIRE(result.status == MixerRenderStatus::Rendered);
    CHECK(result.snapshotStatus == MixerSnapshotStatus::Ok);
    CHECK(finalAllocations == allocations);
    CHECK(finalFrees == frees);
    for (std::size_t frame = 0; frame < 16; ++frame) {
        const auto fraction = frame < 4 ? 0.0F : static_cast<float>(frame - 4) / 160.0F;
        const auto curve = fraction * fraction * (3.0F - 2.0F * fraction);
        const float bus = std::lerp(1.0F, 0.5F, curve);
        const float send = std::lerp(1.0F, 0.25F, curve);
        const float effect = std::lerp(fixture.initialEffect, 0.5F, curve);
        CHECK(fixture.output.left[frame] == Catch::Approx((1.0F + frame * 0.001F) * bus * effect * (1 + send)).margin(3e-6));
    }
    CHECK(retained->Buses()[0].gain == immutableGain);
    CHECK(std::ranges::equal(retained->Routes(), immutableRoutes));
    CHECK(retained->BindAutomation(fixture.bindings).HasError());
    CHECK(fixture.runtime->Reconcile().generation == 1);
}

TEST_CASE("Mixer snapshot compiled graph output is invariant across block partition and exact endpoints",
          "[audio][mixer][snapshot][partition]") {
    Fixture whole, singles;
    auto first = whole.Prepare();
    auto second = singles.Prepare();
    whole.Publish();
    singles.Publish();
    for (std::uint64_t start = 0; start < 176; start += 16) {
        REQUIRE(whole.Render(start, 16, start == 0 ? &first : nullptr, start == 0).status == MixerRenderStatus::Rendered);
        for (std::uint32_t offset = 0; offset < 16; ++offset) {
            const auto frame = start + offset;
            REQUIRE(singles.Render(frame, 1, frame == 4 ? &second : nullptr, frame == 0).status == MixerRenderStatus::Rendered);
            CHECK(whole.output.left[offset] == Catch::Approx(singles.output.left[0]).margin(2e-6));
        }
    }
    float value{};
    REQUIRE(whole.automation.Value(whole.bindings[0].address, value) == AudioAutomationStatus::Ok);
    CHECK(value == 0.5F);
}

TEST_CASE("Mixer snapshot failed start admission preserves prior compiled graph trajectory", "[audio][mixer][snapshot][atomic]") {
    Fixture fixture;
    auto prepared = fixture.Prepare();
    fixture.Publish();
    REQUIRE(fixture.Render(0, 16, &prepared, true).status == MixerRenderStatus::Rendered);
    auto rejected = fixture.Prepare(2, 4, 20);
    std::get<AudioAutomateParameterCommand>(rejected.batch.commands[2].payload).request.targetValue = 17;
    const auto result = fixture.Render(16, 16, &rejected);
    REQUIRE(result.status == MixerRenderStatus::Rendered);
    CHECK(result.snapshotStatus == MixerSnapshotStatus::AutomationRejected);
    CHECK(fixture.transitions.AutomationStatus() == AudioAutomationStatus::OutOfRange);
    CHECK(fixture.automation.HasRequest(1));
    CHECK_FALSE(fixture.automation.HasRequest(4));
    for (std::size_t index = 0; index < 16; ++index) {
        const auto frame = 16 + index;
        const auto fraction = static_cast<float>(frame - 4) / 160;
        const auto curve = fraction * fraction * (3 - 2 * fraction);
        const auto expected = (1 + frame * 0.001F) * std::lerp(1.0F, 0.5F, curve) * std::lerp(fixture.initialEffect, 0.5F, curve) *
                              (1 + std::lerp(1.0F, 0.25F, curve));
        CHECK(fixture.output.left[index] == Catch::Approx(expected).margin(3e-6));
    }
}

TEST_CASE("Mixer automation bind and render reject stale identities and mismatched budgets without unsafe projection",
          "[audio][mixer][snapshot][bindings]") {
    Fixture fixture;
    SECTION("muted bus stays muted") {
        Fixture muted{true};
        auto prepared = muted.Prepare();
        muted.Publish();
        REQUIRE(muted.Render(0, 16, &prepared, true).status == MixerRenderStatus::Rendered);
        CHECK(muted.output.left[15] == 0);
    }
    SECTION("clock mismatch") {
        fixture.Publish();
        fixture.input.Fill(1);
        auto stale = Clock();
        ++stale.discontinuityRevision;
        const auto result =
            fixture.runtime->RenderAutomated(MF::Scope, &fixture.swap, fixture.voices, fixture.output.View(), {fixture.automation, stale});
        CHECK(result.status == MixerRenderStatus::InvalidEpoch);
        CHECK(fixture.output.left[0] == 0);
        CHECK(fixture.automation.CurrentClock().sampleFrame == 0);
    }
    SECTION("unbound replacement generation cannot inherit old automation") {
        auto replacement = MF::Plan(MF::Asset(), 2);
        auto stale = fixture.bindings;
        CHECK(replacement->BindAutomation(stale).HasError());
    }
    SECTION("work budget") {
        auto profile = MF::Profile();
        profile.maximumSampleOperations = 16'000;
        auto candidate = MF::Plan(MF::Asset(), 1, nullptr, profile);
        const auto binding = std::span{fixture.bindings}.first(1);
        CHECK(candidate->BindAutomation(binding).HasError());
    }
    SECTION("memory budget") {
        auto profile = MF::Profile();
        auto candidate = MF::Plan(MF::Asset(), 1, nullptr, profile);
        profile.maximumStorageBytes = candidate->StorageBytes();
        candidate = MF::Plan(MF::Asset(), 1, nullptr, profile);
        CHECK(candidate->BindAutomation(std::span{fixture.bindings}.first(1)).HasError());
    }
}

TEST_CASE("Sealed automation selectors reject another engine and close without changing output", "[audio][snapshot][selectors]") {
    Fixture first, second;
    AudioAutomationValueSelector selector;
    float value = -1;
    CHECK(first.automation.Value(selector, value) == AudioAutomationStatus::MissingParameter);
    REQUIRE(first.automation.ResolveValue(first.bindings[0].address, selector));
    CHECK(first.automation.Value(selector, value) == AudioAutomationStatus::Ok);
    CHECK(value == 1);
    CHECK(second.automation.Value(selector, value) == AudioAutomationStatus::MissingParameter);
    CHECK(value == 1);
    first.automation.Close();
    CHECK(first.automation.Value(selector, value) == AudioAutomationStatus::Closed);
    CHECK_FALSE(first.automation.ResolveValue(first.bindings[0].address, selector));
    CHECK(value == 1);
}
