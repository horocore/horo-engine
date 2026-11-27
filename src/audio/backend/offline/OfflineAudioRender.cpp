#include "Horo/Audio/Internal/OfflineAudioRender.h"

#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/Internal/AudioRenderEnvironment.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <new>

namespace Horo::Audio::Backend {
    namespace {
        /** @brief Aligned scratch storage has no native or asynchronous readers. */
        struct ScratchDelete final {
            void operator()(AudioSample *samples) const noexcept {
                ::operator delete[](samples, std::align_val_t{64});
            }
        };

        /** @brief Validate all sizes and timestamp arithmetic before allocation or rendering. */
        bool ValidRequest(const OfflineAudioRenderRequest &request, const RenderPort render) noexcept {
            constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
            if (!MatchesAudioDeviceEpoch(request.epoch, request.epoch) || !ValidateAudioProcessingFormat(request.format) ||
                render.process == nullptr || request.clockDomain == 0 || request.blockFrames == 0 ||
                request.blockFrames > MaximumAudioCallbackFrames || request.contentFrames > maximum - request.tailFrames)
                return false;
            const auto frames = request.contentFrames + request.tailFrames;
            const auto channels = request.format.layout.orderedChannels.size();
            const auto bytesPerFrame = channels * sizeof(AudioSample);
            const auto byteLimit = std::min<std::uint64_t>(request.maximumOutputBytes, std::numeric_limits<std::size_t>::max());
            return request.maximumOutputBytes <= MaximumOfflineAudioOutputBytes && frames != 0 && frames <= byteLimit / bytesPerFrame &&
                   frames / request.format.sampleRate < maximum / 1'000'000'000;
        }

        /** @brief Exact integer sample time, independent of block partition and wall-clock scheduling. */
        AudioMonotonicTimestamp Timestamp(const OfflineAudioRenderRequest &request, const std::uint64_t frame) noexcept {
            return {request.clockDomain, (frame / request.format.sampleRate) * 1'000'000'000 +
                                             (frame % request.format.sampleRate) * 1'000'000'000 / request.format.sampleRate};
        }

        /** @brief One synchronous process call with precleared canonical scratch and strict phase acknowledgement. */
        bool Process(const OfflineAudioRenderRequest &request, const RenderPort render, const RenderPhase phase, const std::uint64_t frame,
                     const AudioPlanarBlockView &block) noexcept {
            const AudioRenderEnvironment environment;
            for (auto *plane : block.planes)
                std::fill_n(plane, block.capacityFrames, 0.0F);
            const auto result = render.process(render.context, {request.epoch, phase, Timestamp(request, frame), frame, block});
            const auto expected = phase == RenderPhase::Priming     ? RenderDisposition::Ready
                                  : phase == RenderPhase::Quiescing ? RenderDisposition::Quiesced
                                                                    : RenderDisposition::Rendered;
            if (result.fault != AudioCallbackFaultCode::None || result.disposition != expected)
                return false;
            for (const auto *plane : block.planes) {
                if (!std::all_of(plane, plane + block.validFrames, [](const AudioSample sample) {
                    return std::isfinite(sample);
                }))
                    return false;
            }
            return true;
        }
    }  // namespace

    /** @copydoc RenderOfflineAudio */
    Result<OfflineAudioRenderOutput> RenderOfflineAudio(const OfflineAudioRenderRequest &request, const RenderPort render) {
        if (request.cancellation.stop_requested())
            return Result<OfflineAudioRenderOutput>::Failure(MakeError(AudioErrors::OperationCancelled));
        if (!ValidRequest(request, render))
            return Result<OfflineAudioRenderOutput>::Failure(MakeError(AudioErrors::DeviceFormatUnsupported));
        try {
            const auto frames = request.contentFrames + request.tailFrames;
            const auto channels = request.format.layout.orderedChannels.size();
            const std::size_t stride = (request.blockFrames + 15u) & ~std::size_t{15};
            std::unique_ptr<AudioSample, ScratchDelete> scratch{
                static_cast<AudioSample *>(::operator new[](stride * channels * sizeof(AudioSample), std::align_val_t{64}))};
            std::vector<AudioSample *> planes(channels);
            for (std::size_t channel = 0; channel < channels; ++channel)
                planes[channel] = scratch.get() + channel * stride;
            OfflineAudioRenderOutput output{request.format, request.contentFrames, request.tailFrames,
                                            std::vector<AudioSample>(static_cast<std::size_t>(frames) * channels)};
            AudioPlanarBlockView block{ViewAudioChannelLayout(request.format.layout), request.format.sampleRate, planes,
                                       request.blockFrames, request.blockFrames};
            if (!Process(request, render, RenderPhase::Priming, 0, block)) {
                (void)Process(request, render, RenderPhase::Quiescing, 0, block);
                return Result<OfflineAudioRenderOutput>::Failure(MakeError(AudioErrors::CallbackFault));
            }
            bool succeeded = true;
            std::uint64_t frame{};
            for (; frame < frames && succeeded && !request.cancellation.stop_requested();) {
                block.validFrames = static_cast<std::uint32_t>(std::min<std::uint64_t>(request.blockFrames, frames - frame));
                // A block never straddles the source/tail boundary; the prepared port receives the exact frame.
                if (frame < request.contentFrames)
                    block.validFrames =
                        static_cast<std::uint32_t>(std::min<std::uint64_t>(block.validFrames, request.contentFrames - frame));
                succeeded = Process(request, render, RenderPhase::Rendering, frame, block);
                for (std::uint32_t index = 0; index < block.validFrames && succeeded; ++index)
                    for (std::size_t channel = 0; channel < channels; ++channel)
                        output.interleaved[(static_cast<std::size_t>(frame) + index) * channels + channel] = planes[channel][index];
                frame += block.validFrames;
            }
            block.validFrames = request.blockFrames;
            const bool quiesced = Process(request, render, RenderPhase::Quiescing, frame, block);
            if (!succeeded || !quiesced)
                return Result<OfflineAudioRenderOutput>::Failure(MakeError(AudioErrors::CallbackFault));
            if (request.cancellation.stop_requested())
                return Result<OfflineAudioRenderOutput>::Failure(MakeError(AudioErrors::OperationCancelled));
            return Result<OfflineAudioRenderOutput>::Success(std::move(output));
        } catch (const std::bad_alloc &) {
            return Result<OfflineAudioRenderOutput>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }
}  // namespace Horo::Audio::Backend
