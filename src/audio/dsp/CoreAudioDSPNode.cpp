#include "Horo/Audio/CoreAudioDSPNode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <ranges>

namespace Horo::Audio {
    namespace {
        struct FilterState final {
            std::array<double, MaximumAudioChannels> low{};
            std::uint32_t remainingTailFrames{};
        };

        /** @brief Identifies the operations with persistent one-pole history. */
        bool Filter(const CoreAudioDSPKind kind) noexcept {
            return kind == CoreAudioDSPKind::LowPass || kind == CoreAudioDSPKind::HighPass;
        }

        /** @brief Normalizes signed zero and binary32 subnormals without clipping headroom. */
        float Canonical(const float value) noexcept {
            return value == 0.0F || std::fpclassify(value) == FP_SUBNORMAL ? 0.0F : value;
        }

        /** @brief Evaluates the explicit per-call linear segment in double precision. */
        double Parameter(const AudioDSPParameterValue &parameter, const std::uint32_t frame) noexcept {
            if (parameter.rampFrames == 0 || frame >= parameter.rampFrames - 1)
                return parameter.target;
            return static_cast<double>(parameter.value) +
                   (static_cast<double>(parameter.target) - parameter.value) * (frame + 1) / parameter.rampFrames;
        }

        /** @brief Compares validated flat-address storage ranges without dereferencing them. */
        bool RangesOverlap(const std::uintptr_t begin, const std::size_t bytes, const std::uintptr_t otherBegin,
                           const std::size_t otherBytes) noexcept {
            return begin < otherBegin + otherBytes && otherBegin < begin + bytes;
        }

        /** @brief Rejects a sample plane that aliases the host's active node state. */
        bool StateOverlap(const AudioPlanarBlockView &block, const std::span<std::byte> state) noexcept {
            return std::ranges::any_of(block.planes, [&block, state](const auto *plane) {
                return RangesOverlap(reinterpret_cast<std::uintptr_t>(plane), block.capacityFrames * sizeof(AudioSample),
                                     reinterpret_cast<std::uintptr_t>(state.data()), sizeof(FilterState));
            });
        }

        /** @brief Rejects cross-plane or partial aliases before samples or history are modified. */
        bool Overlap(const AudioDSPProcessContext &context) noexcept {
            const auto &input = context.inputs.front().block;
            const auto &output = context.outputs.front().block;
            if (StateOverlap(input, context.stateStorage) || StateOverlap(output, context.stateStorage))
                return true;
            for (std::size_t i = 0; i < input.planes.size(); ++i) {
                for (std::size_t o = 0; o < output.planes.size(); ++o) {
                    if (i == o && input.planes[i] == output.planes[o])
                        continue;
                    if (RangesOverlap(reinterpret_cast<std::uintptr_t>(input.planes[i]), input.capacityFrames * sizeof(AudioSample),
                                      reinterpret_cast<std::uintptr_t>(output.planes[o]), output.capacityFrames * sizeof(AudioSample)))
                        return true;
                }
            }
            return false;
        }

        /** @brief Publishes canonical silence for the exact processed fault range. */
        void ClearOutput(const AudioDSPProcessContext &context) noexcept {
            for (auto *plane : context.outputs.front().block.planes)
                std::fill_n(plane, context.frames, 0.0F);
            context.outputs.front().block.validFrames = context.frames;
        }

        /** @brief Checks finite input before any in-place sample mutation. */
        bool FiniteInput(const AudioDSPProcessContext &context) noexcept {
            for (const auto *plane : context.inputs.front().block.planes) {
                for (std::uint32_t frame = 0; frame < context.frames; ++frame) {
                    if (!std::isfinite(plane[frame]))
                        return false;
                }
            }
            return true;
        }

        /** @brief Evaluates one admitted sample while preserving independent channel history. */
        double Evaluate(const CoreAudioDSPKind kind, const double sample, const double scale, const double pole, double &history) noexcept {
            if (!Filter(kind))
                return sample * scale;
            history = (1.0 - pole) * sample + pole * history;
            if (std::abs(history) < std::numeric_limits<float>::min())
                history = 0.0;
            return kind == CoreAudioDSPKind::LowPass ? history : sample - history;
        }

        /** @brief Computes pan/balance gains once per frame; other operations use one common scale. */
        std::array<double, 2> Scales(const CoreAudioDSPKind kind, const double parameter, const std::size_t inputChannels) noexcept {
            if (kind != CoreAudioDSPKind::Pan)
                return {parameter, parameter};
            const double angle = (parameter + 1.0) * std::numbers::pi / 4.0;
            const double normalization = inputChannels == 2 ? std::numbers::sqrt2 : 1.0;
            return {std::cos(angle) * normalization, std::sin(angle) * normalization};
        }

        /** @brief Validates representability before narrowing to canonical binary32. */
        bool StoreSample(float &output, const double result) noexcept {
            if (!std::isfinite(result) || std::abs(result) > std::numeric_limits<float>::max()) {
                output = 0.0F;
                return false;
            }
            output = Canonical(static_cast<float>(result));
            return true;
        }

        /** @brief Processes one frame; mono input is captured before any admitted in-place write. */
        bool ProcessFrame(const CoreAudioDSPKind kind, const AudioDSPProcessContext &context, FilterState &state,
                          const std::uint32_t frame) noexcept {
            const auto &input = context.inputs.front().block;
            const auto &output = context.outputs.front().block;
            const double monoSample = input.planes.front()[frame];
            const double parameter = Parameter(context.parameters.front(), frame);
            const double pole = Filter(kind) ? std::exp(-2.0 * std::numbers::pi * parameter / input.sampleRate) : 0.0;
            const auto scales = Scales(kind, parameter, input.planes.size());
            bool finite = true;
            for (std::size_t channel = 0; channel < output.planes.size(); ++channel) {
                const double sample = input.planes.size() == 1 ? monoSample : input.planes[channel][frame];
                double result = sample;
                if (context.bypass == AudioDSPBypassMode::Silence)
                    result = 0.0;
                else if (context.bypass == AudioDSPBypassMode::Process)
                    result = Evaluate(kind, sample, scales[std::min(channel, std::size_t{1})], pole, state.low[channel]);
                const bool sampleFinite = StoreSample(output.planes[channel][frame], result);
                finite = finite && sampleFinite;
            }
            return finite;
        }

        /** @brief Advances the declared finite tail budget only for active filter processing. */
        void AdvanceTail(const AudioDSPProcessContext &context, FilterState &state, const std::uint32_t frame,
                         const std::uint32_t tailFrames) noexcept {
            for (const auto *plane : context.inputs.front().block.planes) {
                if (plane[frame] != 0.0F) {
                    state.remainingTailFrames = tailFrames;
                    return;
                }
            }
            if (state.remainingTailFrames == 0)
                std::ranges::fill(state.low, 0.0);
            else
                --state.remainingTailFrames;
        }

        /** @brief Executes every admitted frame even if one needs a subsequent whole-block fault clear. */
        bool ProcessSamples(const CoreAudioDSPKind kind, const AudioDSPProcessContext &context, FilterState &state,
                            const std::uint32_t tailFrames) noexcept {
            bool finite = true;
            for (std::uint32_t frame = 0; frame < context.frames; ++frame) {
                if (context.bypass == AudioDSPBypassMode::Process && Filter(kind))
                    AdvanceTail(context, state, frame, tailFrames);
                const bool frameFinite = ProcessFrame(kind, context, state, frame);
                finite = finite && frameFinite;
            }
            return finite;
        }

    }  // namespace

    /** @copydoc CoreAudioDSPNode::CoreAudioDSPNode */
    CoreAudioDSPNode::CoreAudioDSPNode(const CoreAudioDSPKind kind, AudioProcessingFormat format, const std::uint32_t maximumFrames)
        : m_Kind(kind) {
        using enum CoreAudioDSPKind;
        using enum AudioSpeakerPreset;
        m_Descriptor.inputs.push_back({.format = format, .maximumFrames = maximumFrames});
        if (kind == Pan) {
            const bool supported = format.layout == MakeAudioSpeakerLayout(Mono) || format.layout == MakeAudioSpeakerLayout(Stereo);
            if (!supported)
                format.layout = {};
            else
                format.layout = MakeAudioSpeakerLayout(Stereo);
        }
        m_Descriptor.outputs.push_back({.format = format, .maximumFrames = maximumFrames});
        const auto id = AudioParameterId::Create(1).Value();
        switch (kind) {
            case Gain:
                m_Descriptor.parameters.emplace_back(id, 0.0F, 16.0F, 1.0F);
                break;
            case Pan:
                m_Descriptor.parameters.emplace_back(id, -1.0F, 1.0F, 0.0F);
                break;
            case LowPass:
            case HighPass:
                m_Descriptor.parameters.emplace_back(id, 1.0F, 0.45F * static_cast<float>(format.sampleRate), 1'000.0F);
                m_Descriptor.tailFrames =
                    static_cast<std::uint32_t>(std::min(std::ceil(32.0 * format.sampleRate / (2.0 * std::numbers::pi)),
                                                        static_cast<double>(MaximumAudioDSPTailFrames)));
                break;
            default:
                m_Descriptor.outputs.front().format = {};
                break;
        }
        m_Descriptor.memory.stateBytes = sizeof(FilterState);
        m_Descriptor.maximumFrames = maximumFrames;
        m_Descriptor.inPlace = true;
    }

    /** @copydoc CoreAudioDSPNode::Descriptor */
    const AudioDSPNodeDescriptor &CoreAudioDSPNode::Descriptor() const noexcept {
        return m_Descriptor;
    }

    /** @copydoc CoreAudioDSPNode::Prepare */
    Result<void> CoreAudioDSPNode::Prepare(const AudioDSPPrepareContext &context) {
        if (const auto valid = ValidateAudioDSPPreparation(m_Descriptor, context); valid.HasError())
            return valid;
        m_State = context.stateStorage.first(sizeof(FilterState));
        m_MaximumFrames = context.maximumFrames;
        std::construct_at(static_cast<FilterState *>(static_cast<void *>(m_State.data())));
        return Result<void>::Success();
    }

    /** @copydoc CoreAudioDSPNode::Reset */
    void CoreAudioDSPNode::Reset() noexcept {
        if (!m_State.empty())
            *static_cast<FilterState *>(static_cast<void *>(m_State.data())) = {};
    }

    /** @copydoc CoreAudioDSPNode::Process */
    AudioDSPProcessResult CoreAudioDSPNode::Process(const AudioDSPProcessContext &context) noexcept {
        using enum AudioDSPProcessStatus;
        if (m_State.empty() || context.stateStorage.data() != m_State.data() || context.frames > m_MaximumFrames ||
            !ValidateAudioDSPProcess(m_Descriptor, context) || Overlap(context))
            return {.fault = AudioDSPFault::InvalidInput};
        if (!FiniteInput(context)) {
            ClearOutput(context);
            Reset();
            return {.status = Fault, .fault = AudioDSPFault::InvalidInput};
        }
        auto &state = *static_cast<FilterState *>(static_cast<void *>(m_State.data()));
        const bool finite = ProcessSamples(m_Kind, context, state, m_Descriptor.tailFrames);
        context.outputs.front().block.validFrames = context.frames;
        if (!finite) {
            ClearOutput(context);
            Reset();
            return {.status = Fault, .fault = AudioDSPFault::NonFiniteOutput};
        }
        return {.status = context.bypass == AudioDSPBypassMode::Process ? Processed : Bypassed,
                .processedFrames = context.frames,
                .remainingTailFrames = state.remainingTailFrames};
    }
}  // namespace Horo::Audio
