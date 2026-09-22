#include "UiTextLayoutInternal.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace Horo::Runtime::Ui {
    using UiTextLayoutInternal::Failure;

    Result<void> UiTextLayoutEngine::Storage::AppendLinePlan(const std::uint32_t first, const std::uint32_t end, const bool hardBreak) {
        if (linePlans.size() >= descriptor.limits.lines)
            return Failure(UiErrors::TextLayoutCapacityExceeded);
        linePlans.push_back({first, end - first, clusterPrefix[end] - clusterPrefix[first], hardBreak});
        return Result<void>::Success();
    }

    std::uint32_t UiTextLayoutEngine::Storage::SelectWrapSplit(const std::uint32_t index, const std::uint32_t lineStart,
                                                               const UiTextWrapMode wrap) const noexcept {
        if (wrap != UiTextWrapMode::Word)
            return index;
        const auto upper = std::upper_bound(softBreaks.begin(), softBreaks.end(), index);
        if (upper == softBreaks.begin())
            return index;
        const auto candidate = *std::prev(upper);
        return candidate > lineStart ? candidate : index;
    }

    Result<void> UiTextLayoutEngine::Storage::BuildLinePlans(const UiTextLayoutRequest &request) {
        const auto &clusters = request.shaped.clusters;
        scaledClusterAdvances.resize(clusters.size());
        clusterPrefix.resize(clusters.size() + 1U);
        clusterPrefix[0] = 0;
        softBreaks.clear();
        for (std::uint32_t index = 0; index < clusters.size(); ++index) {
            const auto advance = UiTextLayoutInternal::ScaleValue(clusters[index].advance.x, request.options.scale);
            if (advance.HasError())
                return Result<void>::Failure(advance.ErrorValue());
            scaledClusterAdvances[index] = advance.Value();
            clusterPrefix[index + 1U] = clusterPrefix[index] + advance.Value();
            if (clusters[index].breakOpportunity == UiTextBreakOpportunity::Optional)
                softBreaks.push_back(index + 1U);
        }

        linePlans.clear();
        const auto availableWidth = static_cast<std::int64_t>(request.assignedContent.extent.width);
        std::uint32_t lineStart = 0;
        for (std::uint32_t index = 0; index < clusters.size(); ++index) {
            const auto mandatory = clusters[index].breakOpportunity == UiTextBreakOpportunity::Mandatory;
            while (!mandatory && request.options.wrap != UiTextWrapMode::NoWrap && index > lineStart &&
                   clusterPrefix[index + 1U] - clusterPrefix[lineStart] > availableWidth) {
                const auto split = SelectWrapSplit(index, lineStart, request.options.wrap);
                if (const auto appended = AppendLinePlan(lineStart, split, false); appended.HasError())
                    return appended;
                lineStart = split;
            }
            if (mandatory) {
                if (const auto appended = AppendLinePlan(lineStart, index + 1U, true); appended.HasError())
                    return appended;
                lineStart = index + 1U;
            }
        }
        if (lineStart == clusters.size() && !linePlans.empty() && linePlans.back().hardBreak)
            return AppendLinePlan(lineStart, lineStart, false);
        if (lineStart < clusters.size() || linePlans.empty())
            return AppendLinePlan(lineStart, static_cast<std::uint32_t>(clusters.size()), false);
        return Result<void>::Success();
    }

    Result<std::uint32_t> UiTextLayoutEngine::Storage::VisibleLineCount(const UiTextLayoutRequest &request,
                                                                        const std::int32_t lineHeight) const {
        std::uint32_t count = static_cast<std::uint32_t>(linePlans.size());
        if (request.options.maxLines > 0)
            count = std::min(count, request.options.maxLines);
        if (request.options.overflow == UiTextOverflowMode::Ellipsis && lineHeight > 0 &&
            request.assignedContent.extent.height < std::numeric_limits<std::int32_t>::max()) {
            const auto byHeight = std::max<std::int32_t>(1, request.assignedContent.extent.height / lineHeight);
            count = std::min(count, static_cast<std::uint32_t>(byHeight));
        }
        return Result<std::uint32_t>::Success(count);
    }
}  // namespace Horo::Runtime::Ui
