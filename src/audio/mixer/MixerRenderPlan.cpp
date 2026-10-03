#include "MixerPlanState.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Audio::MixerDetail {
    namespace {
        /** @brief Construct a borrow over immutable prepared pointer arrays without allocating layout copies. */
        AudioPlanarBlockView Block(const BusState &bus, const std::array<AudioSample *, MaximumAudioChannels> &planes,
                                   const std::uint32_t frames, const std::uint32_t capacity) noexcept {
            return {ViewAudioChannelLayout(bus.format.layout),
                    bus.format.sampleRate,
                    {planes.data(), bus.format.layout.orderedChannels.size()},
                    frames,
                    capacity};
        }

        /** @brief Reject nonfinite/out-of-range results before binary32 conversion; canonicalize tiny samples. */
        bool Store(const double value, AudioSample &output) noexcept {
            if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<AudioSample>::max())
                return false;
            const auto sample = static_cast<float>(value);
            output = sample == 0.0F || std::fpclassify(sample) == FP_SUBNORMAL ? 0.0F : sample;
            return true;
        }

        /** @brief Validate and canonicalize insert output before a later node or immutable source tap can read it. */
        bool Normalize(const AudioPlanarBlockView &block) noexcept {
            for (AudioSample *plane : block.planes)
                for (std::uint32_t frame = 0; frame < block.validFrames; ++frame)
                    if (!Store(plane[frame], plane[frame]))
                        return false;
            return true;
        }

        /** @brief Accumulate a completed source tap in canonical incoming order, preserving finite headroom. */
        bool Accumulate(const AudioPlanarBlockView &source, const AudioPlanarBlockView &destination, const float gain) noexcept {
            for (std::size_t c = 0; c < destination.planes.size(); ++c)
                for (std::uint32_t f = 0; f < destination.validFrames; ++f)
                    if (!Store(static_cast<double>(destination.planes[c][f]) + static_cast<double>(source.planes[c][f]) * gain,
                               destination.planes[c][f]))
                        return false;
            return true;
        }

        /** @brief Validate stable identity and compiled bus index before dereferencing any per-bus binding. */
        bool VoiceIdentity(const MixerRenderPlan::State &s, const MixerVoiceInput &voice, const std::uint32_t previous) noexcept {
            return voice.voice.IsValid() && voice.voice.owner == s.identity.owner && voice.voice.slot <= MaximumAudioHandleSlots &&
                   voice.voice.slot > previous && voice.graphGeneration == s.identity.generation && voice.busIndex < s.buses.size();
        }

        /** @brief Validate admitted direct routing and caller-owned dynamic buffer shape. */
        bool VoiceBuffer(const MixerRenderPlan::State &s, const MixerVoiceInput &voice, const std::uint32_t frames) noexcept {
            return s.buses[voice.busIndex].role != MixerBusRole::Return && voice.samples.validFrames >= frames &&
                   voice.samples.capacityFrames <= s.profile.maximumFrames &&
                   ValidateAudioPlanarBlock(voice.samples, s.processing[voice.busIndex].format);
        }

        /** @brief Require exact-generation, live-shaped, stable-slot-ordered direct voice inputs. */
        bool ValidVoices(const MixerRenderPlan::State &s, const std::span<const MixerVoiceInput> voices,
                         const std::uint32_t frames) noexcept {
            if (voices.size() > s.profile.maximumVoices)
                return false;
            std::uint32_t previous{};
            for (const MixerVoiceInput &voice : voices) {
                if (!VoiceIdentity(s, voice, previous) || !VoiceBuffer(s, voice, frames))
                    return false;
                previous = voice.voice.slot;
            }
            return true;
        }

        /** @brief Accept only known successful process dispositions with exact work and declared tail bounds. */
        bool ValidResult(const AudioDSPProcessResult &result, const AudioDSPNodeDescriptor &descriptor,
                         const std::uint32_t frames) noexcept {
            using enum AudioDSPProcessStatus;
            const bool disposition = result.status == Processed || result.status == Bypassed || result.status == Tail;
            return disposition && result.fault == AudioDSPFault::None && result.processedFrames == frames &&
                   result.remainingTailFrames <= descriptor.tailFrames;
        }

        /** @brief Run the insert chain with disjoint output storage; pre-fader taps become immutable only afterward. */
        bool Inserts(const MixerRenderPlan::State &s, BusState &bus, const std::uint32_t frames) noexcept {
            AudioPlanarBlockView input = Block(bus, bus.pre, frames, s.profile.maximumFrames);
            AudioPlanarBlockView output = Block(bus, bus.work, frames, s.profile.maximumFrames);
            for (Insert &insert : bus.inserts) {
                std::array<AudioDSPPortBuffer, 1> inputs{{{true, input}}};
                std::array<AudioDSPPortBuffer, 1> outputs{{{true, output}}};
                const AudioDSPProcessContext context{inputs,
                                                     outputs,
                                                     insert.parameters,
                                                     {s.storage.get() + insert.stateOffset, insert.descriptor.memory.stateBytes},
                                                     {s.storage.get() + s.scratchOffset, insert.descriptor.memory.scratchBytes},
                                                     frames,
                                                     insert.effect.bypassed ? AudioDSPBypassMode::CopyMainInput
                                                                            : AudioDSPBypassMode::Process};
                if (const AudioDSPProcessResult result = insert.node->Process(context);
                    !ValidResult(result, insert.descriptor, frames) || !Normalize(output))
                    return false;
                std::swap(input, output);
            }
            if (input.planes.data() != bus.pre.data()) {
                for (std::size_t c = 0; c < input.planes.size(); ++c)
                    std::copy_n(input.planes[c], frames, bus.pre[c]);
            }
            return true;
        }

        /** @brief Pull only already-complete taps; destination clearing cannot touch any source buffer. */
        bool Routes(MixerRenderPlan::State &s, const MixerCompiledBus &bus, const AudioPlanarBlockView &accumulator) noexcept {
            for (std::uint32_t i = bus.firstIncoming; i < bus.firstIncoming + bus.incomingCount; ++i) {
                const MixerCompiledRoute &route = s.routes[i];
                const BusState &source = s.processing[route.sourceBus];
                const auto &planes = route.tap == MixerSendTap::PostFader ? source.post : source.pre;
                if (!Accumulate(Block(source, planes, accumulator.validFrames, s.profile.maximumFrames), accumulator, route.gain))
                    return false;
            }
            return true;
        }

        /** @brief Publish separate post-fader storage without modifying pre-fader samples still read by sends. */
        bool Fader(const MixerCompiledBus &bus, const BusState &processing, const std::uint32_t frames) noexcept {
            for (std::size_t c = 0; c < bus.layout.orderedChannels.size(); ++c)
                for (std::uint32_t f = 0; f < frames; ++f)
                    if (!Store(static_cast<double>(processing.pre[c][f]) * bus.gain, processing.post[c][f]))
                        return false;
            return true;
        }

        /** @brief Clear all retained signal/history after a fault, leaving the complete generation owned. */
        void ResetAfterFault(MixerRenderPlan::State &s) noexcept {
            for (BusState &bus : s.processing) {
                for (Insert &insert : bus.inserts)
                    insert.node->Reset();
                Silence(Block(bus, bus.pre, s.profile.maximumFrames, s.profile.maximumFrames));
                Silence(Block(bus, bus.post, s.profile.maximumFrames, s.profile.maximumFrames));
                Silence(Block(bus, bus.work, s.profile.maximumFrames, s.profile.maximumFrames));
            }
        }

        /** @brief Execute one bounded bus; direct voices precede sorted incoming routes and persisted inserts. */
        bool RenderBus(MixerRenderPlan::State &s, const std::size_t index, const std::span<const MixerVoiceInput> voices,
                       const std::uint32_t frames) noexcept {
            const MixerCompiledBus &bus = s.buses[index];
            BusState &processing = s.processing[index];
            const AudioPlanarBlockView accumulator = Block(processing, processing.pre, frames, s.profile.maximumFrames);
            Silence(accumulator);
            Silence(Block(processing, processing.post, frames, s.profile.maximumFrames));
            Silence(Block(processing, processing.work, frames, s.profile.maximumFrames));
            if (!bus.paused) {
                for (const MixerVoiceInput &voice : voices)
                    if (voice.busIndex == index && !Accumulate(voice.samples, accumulator, 1.0F))
                        return false;
            }
            return Routes(s, bus, accumulator) && Inserts(s, processing, frames) && Fader(bus, processing, frames);
        }
    }  // namespace

    /** @copydoc Silence */
    void Silence(const AudioPlanarBlockView &output) noexcept {
        for (AudioSample *plane : output.planes)
            std::fill_n(plane, output.capacityFrames, 0.0F);
    }

    /** @copydoc RenderPlan */
    MixerRenderStatus RenderPlan(MixerRenderPlan::State &s, const std::span<const MixerVoiceInput> voices,
                                 const AudioPlanarBlockView &output) noexcept {
        if (!ValidVoices(s, voices, output.validFrames)) {
            Silence(output);
            return MixerRenderStatus::InvalidBuffer;
        }
        for (std::size_t i = 0; i < s.buses.size(); ++i) {
            if (!RenderBus(s, i, voices, output.validFrames)) {
                ResetAfterFault(s);
                Silence(output);
                return MixerRenderStatus::DSPFault;
            }
        }
        const BusState &master = s.processing.back();
        for (std::size_t c = 0; c < output.planes.size(); ++c) {
            std::copy_n(master.post[c], output.validFrames, output.planes[c]);
            std::fill(output.planes[c] + output.validFrames, output.planes[c] + output.capacityFrames, 0.0F);
        }
        return MixerRenderStatus::Rendered;
    }
}  // namespace Horo::Audio::MixerDetail
