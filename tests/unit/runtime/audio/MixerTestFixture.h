#pragma once

#include "AllocationProbe.h"
#include "Horo/Audio/MixerGraphCompiler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <latch>
#include <limits>
#include <thread>

using namespace Horo;
using namespace Horo::Audio;

namespace Horo::Tests::MixerFixture {
    template <class Id> Id IdOf(const std::uint64_t value) {
        return Id::Create(value).Value();
    }

    inline const AudioRuntimeId Owner = IdOf<AudioRuntimeId>(17);
    inline const AudioCommandScope Scope{Owner, 3, {Owner, 1, 1}};

    inline MixerCompileProfile Profile() {
        return {.sampleRate = 48'000,
                .outputLayout = MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo),
                .maximumFrames = 16,
                .maximumVoices = 8};
    }

    inline MixerPlanIdentity Identity(const std::uint64_t generation = 1) {
        return {Owner, 3, generation, generation, 2, 4};
    }

    /** @brief Fixed aligned fixture with semantic ownership independent of plan/asset containers. */
    struct Block final {
        AudioChannelLayout layout = Profile().outputLayout;
        alignas(64) std::array<float, 16> left{};
        alignas(64) std::array<float, 16> right{};
        std::array<AudioSample *, 2> planes{left.data(), right.data()};

        AudioPlanarBlockView View(const std::uint32_t frames = 16) const {
            return {ViewAudioChannelLayout(layout), 48'000, planes, frames, 16};
        }

        void Fill(const float value) {
            left.fill(value);
            right.fill(value);
        }
    };

    struct Probe final {
        std::atomic<std::uint32_t> destroyed{};
        std::atomic<std::uint32_t> destroyedOnCallback{};
        std::atomic<bool> callback{};
        bool failPrepare{};
        std::atomic<bool> fault{};
        bool wrongFrames{};
        bool nonFinite{};
        AudioDSPProcessStatus status{AudioDSPProcessStatus::Processed};
        std::uint32_t tail{};
        std::latch *entered{};
        std::latch *leave{};
    };

    /** @brief Instrumented fixture strategy; optional latches hold a reader for deterministic lifetime testing only. */
    class Node final : public IAudioDSPNode {
    public:
        Node(const AudioProcessingFormat &format, const MixerEffectDescriptor &effect, Probe &probe) : probe_(probe) {
            descriptor_.inputs.push_back({AudioDSPPortKind::Main, format, 16, false});
            descriptor_.outputs = descriptor_.inputs;
            descriptor_.parameters.push_back(
                {IdOf<AudioParameterId>(1), 0, 16, std::pow(10.0F, std::get<MixerGainEffectParameters>(effect.parameters).gainDb / 20.0F)});
            descriptor_.memory = {64, 64, 64};
            descriptor_.maximumFrames = 16;
            descriptor_.tailFrames = 16;
        }

        ~Node() override {
            probe_.destroyed.fetch_add(1);
            if (probe_.callback.load())
                probe_.destroyedOnCallback.fetch_add(1);
        }

        const AudioDSPNodeDescriptor &Descriptor() const noexcept override {
            return descriptor_;
        }

        Result<void> Prepare(const AudioDSPPrepareContext &context) override {
            if (probe_.failPrepare)
                return Result<void>::Failure(MakeError(AudioErrors::DspPreparationStorageInsufficient));
            if (const Result<void> valid = ValidateAudioDSPPreparation(descriptor_, context); valid.HasError())
                return valid;
            state_ = context.stateStorage;
            return Result<void>::Success();
        }

        AudioDSPProcessResult Process(const AudioDSPProcessContext &context) noexcept override {
            if (probe_.entered != nullptr) {
                probe_.entered->count_down();
                probe_.leave->wait();
            }
            if (probe_.fault.load(std::memory_order_relaxed))
                return {AudioDSPProcessStatus::Fault, AudioDSPFault::InternalError};
            if (!ValidateAudioDSPProcess(descriptor_, context) || context.stateStorage.data() != state_.data())
                return {AudioDSPProcessStatus::Rejected, AudioDSPFault::InvalidInput};
            const float selectedGain = context.bypass == AudioDSPBypassMode::Process ? context.parameters.front().value : 1.0F;
            const float gain = probe_.nonFinite ? std::numeric_limits<float>::quiet_NaN() : selectedGain;
            for (std::size_t c = 0; c < context.outputs.front().block.planes.size(); ++c)
                for (std::uint32_t f = 0; f < context.frames; ++f)
                    context.outputs.front().block.planes[c][f] = context.inputs.front().block.planes[c][f] * gain;
            const auto disposition = context.bypass == AudioDSPBypassMode::Process ? probe_.status : AudioDSPProcessStatus::Bypassed;
            return {disposition, AudioDSPFault::None, context.frames + static_cast<std::uint32_t>(probe_.wrongFrames), probe_.tail};
        }

        void Reset() noexcept override {
            std::ranges::fill(state_, std::byte{});
        }

        AudioDSPNodeDescriptor descriptor_;

    private:
        Probe &probe_;
        std::span<std::byte> state_;
    };

    class Factory final : public IMixerDSPFactory {
    public:
        explicit Factory(Probe &probe) : probe_(probe) {}

        Result<MixerDSPStrategy> Create(const MixerEffectDescriptor &effect, const AudioProcessingFormat &format) override {
            if (failCreate)
                return Result<MixerDSPStrategy>::Failure(MakeError(AudioErrors::ProviderUnavailable));
            if (noNode)
                return Result<MixerDSPStrategy>::Success({{}, operations});
            auto node = std::make_unique<Node>(format, effect, probe_);
            if (sidechain)
                node->descriptor_.inputs.push_back(node->descriptor_.inputs.front());
            node->descriptor_.latencyFrames = latency;
            node->descriptor_.memory.scratchBytes = scratch;
            node->descriptor_.allocationFree = allocationFree;
            node->descriptor_.supportsReset = reset;
            node->descriptor_.supportsBypass = bypass;
            node->descriptor_.maximumFrames = maximumFrames;
            if (mismatchedFormat)
                node->descriptor_.outputs.front().format.layout = MakeAudioSpeakerLayout(AudioSpeakerPreset::Mono);
            return Result<MixerDSPStrategy>::Success({std::move(node), operations});
        }

        bool sidechain{};
        std::uint32_t latency{};
        std::size_t scratch{64};
        bool allocationFree{true};
        std::uint64_t operations{4};
        bool failCreate{};
        bool noNode{};
        bool mismatchedFormat{};
        bool reset{true};
        bool bypass{true};
        std::uint32_t maximumFrames{16};

    private:
        Probe &probe_;
    };

    inline MixerAssetSchema Asset(const bool effects = false) {
        MixerAssetSchema asset = MakeDefaultMixerAsset();
        if (effects)
            asset.buses[1].effects.push_back({IdOf<AudioEffectId>(20), MixerEffectKind::Gain, false, MixerGainEffectParameters{6.0206F}});
        return asset;
    }

    inline std::unique_ptr<MixerRenderPlan> Plan(const MixerAssetSchema &asset, const std::uint64_t generation = 1,
                                                 IMixerDSPFactory *factory = nullptr, const MixerCompileProfile &profile = Profile()) {
        auto result = CompileMixerGraph(asset, Identity(generation), profile, factory);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    inline AudioCommandStaging Staging() {
        auto result = AudioCommandStaging::Create(
            {{Owner, IdOf<AudioMemoryPoolId>(31), 3, 4, 1, 32'768}, IdOf<AudioMemoryPoolId>(32), 4, 1, 2, 32'768});
        REQUIRE(result.HasValue());
        AudioCommandStaging staging = std::move(result).Value();
        REQUIRE(staging.RegisterScene(Scope.scene) == AudioCommandStagingStatus::Ok);
        return staging;
    }

    inline std::unique_ptr<MixerGraphRuntime> Runtime(const MixerCompileProfile &profile = Profile(),
                                                      const std::size_t retained = MaximumAudioMemoryBytes) {
        auto result = MixerGraphRuntime::Create({Scope, IdOf<AudioMemoryPoolId>(33), profile, retained});
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    /** @brief Consume one accepted record through the same control Pump and callback transport boundary. */
    inline AudioCommandRecord ConsumePublished(AudioCommandStaging &staging) {
        REQUIRE(staging.Pump(1).published == 1);
        AudioCommandRecord record;
        REQUIRE(staging.TryConsume(record));
        return record;
    }

    inline AudioCommandRecord Publish(MixerGraphRuntime &runtime, AudioCommandStaging &staging, std::unique_ptr<MixerRenderPlan> &plan) {
        const MixerPlanIdentity identity = plan->Identity();
        REQUIRE(runtime.Publish(plan, identity, staging).status == AudioCommandStagingStatus::Ok);
        REQUIRE(plan == nullptr);
        return ConsumePublished(staging);
    }

    inline MixerVoiceInput Voice(const MixerRenderPlan &plan, const Block &block, const std::uint64_t bus = 2,
                                 const std::uint32_t slot = 1) {
        const auto index = plan.ResolveBus(IdOf<AudioBusId>(bus));
        REQUIRE(index.has_value());
        return {{Owner, slot, 1}, plan.Identity().generation, *index, block.View()};
    }

    inline void ShutDown(MixerGraphRuntime &runtime, Block &output) {
        runtime.Close();
        REQUIRE(runtime.Render(Scope, nullptr, {}, output.View()).status == MixerRenderStatus::Quiesced);
        REQUIRE(runtime.CompleteShutdown(Scope, true) == MixerRenderStatus::Quiesced);
    }

    /** @brief Retain normal staging alongside a submitted complete plan for focused render fixtures. */
    struct Publication final {
        std::unique_ptr<MixerGraphRuntime> runtime = Runtime();
        AudioCommandStaging staging = Staging();
        AudioCommandRecord record;

        explicit Publication(std::unique_ptr<MixerRenderPlan> &plan) : record(Publish(*runtime, staging, plan)) {}

        MixerRenderResult AdoptAndRender(std::span<const MixerVoiceInput> voices, const AudioPlanarBlockView &output) {
            return runtime->Render(Scope, &record, voices, output);
        }
    };

    /** @brief Retain one direct input/output pair across adoption, recovery and padding checks. */
    struct Signal final {
        Block input, output;
        std::array<MixerVoiceInput, 1> voices;
        Publication publication;

        Signal(std::unique_ptr<MixerRenderPlan> &plan, const float value) : voices{Voice(*plan, input)}, publication(plan) {
            input.Fill(value);
        }

        MixerRenderResult AdoptAndRender(const std::uint32_t frames = 16) {
            return publication.AdoptAndRender(voices, output.View(frames));
        }
    };
}  // namespace Horo::Tests::MixerFixture
