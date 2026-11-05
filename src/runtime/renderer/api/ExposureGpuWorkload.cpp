#include "Horo/Runtime/Render/ExposureGpuWorkload.h"

#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#include <algorithm>
#include <limits>

namespace Horo::Render {
    namespace {
        /** @brief Reconstructs one portable split64 ABI identity. */
        std::uint64_t Wide(std::uint32_t low, std::uint32_t high) noexcept {
            return low | (std::uint64_t{high} << 32U);
        }

        /** @brief Validates fixed GPU control lanes by the same authored settings rules. */
        Result<void> ValidateParameters(const RenderGraphExposureGpu &w) {
            const auto &p = w.dispatch.exposure;
            if (p.controls[0] != 1 || p.controls[1] != ExposureHistogramBins || p.controls[2] > 1 || p.controls[3] > 1)
                return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
            ExposureSettings s;
            s.mode = ExposureMode::AutomaticHistogram;
            s.timeSource = static_cast<ExposureTimeSource>(p.controls[2]);
            s.reset = static_cast<ExposureResetPolicy>(p.controls[3]);
            s.minimumLogLuminance = p.limits[0];
            s.maximumLogLuminance = p.limits[1];
            s.lowPercentile = p.limits[2];
            s.highPercentile = p.limits[3];
            s.minimumEv = p.adaptation[0];
            s.maximumEv = p.adaptation[1];
            s.brightenEvPerSecond = p.adaptation[2];
            s.darkenEvPerSecond = p.adaptation[3];
            s.neutralTarget = p.timing[1];
            s.compensationEv = p.timing[2];
            s.initialEv = p.timing[3];
            s.manualEv = s.initialEv;
            auto prepared = PrepareExposureDispatch(s, w.sourceExtent, p.region, p.timing[0]);
            if (prepared.HasError())
                return Result<void>::Failure(prepared.ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Checks graph-local identity and nonaliasing before native translation. */
        bool DistinctResources(const RenderGraphExposureGpu &w) noexcept {
            const std::array ids{w.sceneColor, w.histogram, w.previousExposure, w.outputExposure};
            for (std::size_t i = 0; i < ids.size(); ++i) {
                if (!ids[i].IsValid() || ids[i].owner != ids[0].owner)
                    return false;
                for (std::size_t j = 0; j < i; ++j)
                    if (ids[i] == ids[j])
                        return false;
            }
            return true;
        }

        /** @brief Compares the two packed output identity fields to admitted metadata. */
        bool PackedIdentityValid(const RenderGraphExposureGpu &w) noexcept {
            return Wide(w.dispatch.identity[0], w.dispatch.identity[1]) == w.produced.generation &&
                   Wide(w.dispatch.identity[2], w.dispatch.identity[3]) == w.produced.sourceSubmission;
        }

        /** @brief Checks identical view/settings/device scope across the two submissions. */
        bool SameScope(const ExposureState &a, const ExposureState &b) noexcept {
            return a.view == b.view && a.settingsGeneration == b.settingsGeneration && a.deviceGeneration == b.deviceGeneration;
        }

        /** @brief Checks output buffer metadata against the exact packed shader identity. */
        bool ProducedValid(const RenderGraphExposureGpu &w) noexcept {
            const auto &s = w.produced;
            const bool identity = s.view.IsValid() && s.generation > 0 && s.settingsGeneration > 0 && s.deviceGeneration > 0;
            const bool ordinal = s.sourceSubmission > 0 && s.sourceSubmission < std::numeric_limits<std::uint64_t>::max() &&
                                 s.consumeSubmission == s.sourceSubmission + 1;
            return identity && ordinal && s.mode == ExposureMode::AutomaticHistogram && s.buffer.IsValid() && PackedIdentityValid(w);
        }

        /** @brief Checks predecessor metadata; actual same-queue acceptance is a native journal predicate. */
        bool PreviousValid(const RenderGraphExposureGpu &w) noexcept {
            const auto &p = w.previous;
            if (w.resetPrevious)
                return p.mode == ExposureMode::Manual && p.reason == ExposureSampleReason::Initial;
            const auto &s = w.produced;
            const bool scope = SameScope(p, s);
            return scope && p.mode == ExposureMode::AutomaticHistogram && p.buffer.IsValid() && p.generation > 0 &&
                   s.generation > p.generation && p.sourceSubmission > 0 && p.consumeSubmission == s.sourceSubmission &&
                   p.sourceSubmission == s.sourceSubmission - 1;
        }
    }  // namespace

    namespace {
        /** @brief Matches one exact logical resource use and kind. */
        bool MatchesUse(const RenderGraphExecutionRequest &request, std::span<const RenderGraphResourceUsage> uses,
                        RenderGraphResourceId id, RenderGraphResourceKind kind, RenderGraphUsageKind usage, RenderGraphAccess access) {
            if (id.owner != request.graph.Owner() || id.value == 0 || id.value > request.graph.Resources().size())
                return false;
            if (request.graph.Resources()[id.value - 1].kind != kind)
                return false;
            const auto found = std::ranges::find(uses, id, &RenderGraphResourceUsage::resource);
            return found != uses.end() && found->kind == usage && found->access == access;
        }

        /** @brief Prevents relabeling an unrelated graph buffer as the public view exposure state. */
        bool MatchesBuffer(const RenderGraphExecutionRequest &request, RenderGraphResourceId id, RenderBufferHandle buffer) {
            const auto &resource = request.graph.Resources()[id.value - 1];
            const auto *handle = std::get_if<RenderBufferHandle>(&resource.binding);
            return handle && *handle == buffer;
        }

        /** @brief Accounts for exactly the four retained exposure graph resource uses. */
        bool GraphResourcesValid(const RenderGraphExecutionRequest &request, std::span<const RenderGraphResourceUsage> uses,
                                 const RenderGraphExposureGpu &w) {
            using enum RenderGraphAccess;
            const std::array ids{w.sceneColor, w.histogram, w.previousExposure, w.outputExposure};
            const std::array kinds{RenderGraphResourceKind::Texture, RenderGraphResourceKind::Buffer, RenderGraphResourceKind::Buffer,
                                   RenderGraphResourceKind::Buffer};
            const std::array accesses{Read, ReadWrite, w.resetPrevious ? ReadWrite : Read, Write};
            for (std::size_t i = 0; i < ids.size(); ++i)
                if (!MatchesUse(request, uses, ids[i], kinds[i], i == 0 ? RenderGraphUsageKind::Sampled : RenderGraphUsageKind::Storage,
                                accesses[i]))
                    return false;
            return true;
        }
    }  // namespace

    /** @copydoc ValidateExposureGpuGraphWorkload */
    Result<void> ValidateExposureGpuGraphWorkload(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                                  const RenderGraphExposureGpu &w) {
        if (auto valid = ValidateExposureGpuWorkload(w); valid.HasError())
            return valid;
        const auto uses = request.graph.Usages().subspan(pass.usages.offset, pass.usages.count);
        if (pass.kind != RenderPassKind::Compute || uses.size() != 4)
            return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        if (!GraphResourcesValid(request, uses, w))
            return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        if (!MatchesBuffer(request, w.outputExposure, w.produced.buffer))
            return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        if (!w.resetPrevious && !MatchesBuffer(request, w.previousExposure, w.previous.buffer))
            return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        return Result<void>::Success();
    }

    /** @copydoc ValidateExposureGpuWorkload */
    Result<void> ValidateExposureGpuWorkload(const RenderGraphExposureGpu &workload) {
        if (!workload.kernel || !DistinctResources(workload) || !ProducedValid(workload) || !PreviousValid(workload))
            return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        return ValidateParameters(workload);
    }
}  // namespace Horo::Render
