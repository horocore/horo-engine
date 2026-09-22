#pragma once

#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiTextLayout.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace UiTextLayoutInternal {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        struct LinePlan final {
            std::uint32_t firstCluster{};
            std::uint32_t clusterCount{};
            std::int64_t advance{};
            bool hardBreak{};
        };

        struct LineWindow final {
            std::uint32_t start{};
            std::uint32_t end{};
            std::int64_t sourceWidth{};
            std::int64_t ellipsisUsedWidth{};
            std::uint32_t ellipsisGlyphCount{};
            bool ellipsis{};
        };

        struct LinePlacement final {
            std::int64_t lineAdvance{};
            std::int64_t freeWidth{};
            std::int32_t originX{};
            std::int32_t originY{};
            std::int32_t baseline{};
        };

        struct OutputMetrics final {
            std::int64_t fullWidth{};
            std::int64_t fullHeight{};
            std::int64_t ellipsisWidth{};
            bool verticallyTruncated{};
            bool needsEllipsis{};
        };

        struct LineBuildContext final {
            std::int32_t lineHeight{};
            std::uint32_t visibleLines{};
            std::uint32_t lineIndex{};
            bool verticallyTruncated{};
            std::int64_t ellipsisWidth{};
            std::int64_t verticalOffset{};
            std::int32_t scaledAscent{};
        };

        struct SourceAppendState final {
            std::uint32_t start{};
            std::uint32_t lineIndex{};
            UiLogicalPoint lineOrigin;
            std::int64_t lineOriginX{};
            std::int64_t cursor{};
            std::uint32_t optionalGaps{};
            std::int64_t justifyExtra{};
            std::int64_t justifyRemainder{};
            std::uint32_t gapIndex{};
            std::int64_t justificationAdded{};
        };

        [[nodiscard]] Result<std::int32_t> ScaleValue(std::int32_t value, UiTextScale scale);
        [[nodiscard]] Result<std::int32_t> AddValue(std::int64_t left, std::int64_t right);
        [[nodiscard]] bool IsOlder(const UiLayoutSourceRevisions &candidate, const UiLayoutSourceRevisions &current) noexcept;
    }  // namespace UiTextLayoutInternal

    struct UiTextLayoutResult::Storage final {
        mutable std::atomic<std::uint64_t> leases{};
        UiTextLayoutResultDescriptor descriptor;
        UiLayoutMeasurement measurement;
        UiTextLayoutOverflow overflow;
        std::vector<UiTextLayoutLine> lines;
        std::vector<UiTextLayoutGlyph> glyphs;
        std::vector<UiTextLayoutRun> runs;

        explicit Storage(const UiTextLayoutLimits &limits);
        void Reset() noexcept;
    };

    struct UiTextLayoutEngine::Storage final {
        UiTextLayoutEngineDescriptor descriptor;
        UiTextLayoutEngineState lifecycle{UiTextLayoutEngineState::Active};
        std::vector<std::shared_ptr<UiTextLayoutResult::Storage>> slots;
        std::size_t nextSlot{};
        UiTextLayoutRevision lastRevision;
        UiTextLayoutSource lastSource;
        bool hasSource{};

        std::vector<std::int32_t> scaledClusterAdvances;
        std::vector<std::int64_t> clusterPrefix;
        std::vector<std::uint32_t> softBreaks;
        std::vector<UiTextLayoutInternal::LinePlan> linePlans;
        std::vector<UiTextFaceId> glyphFaces;
        std::vector<UiTextFaceId> ellipsisFaces;

        explicit Storage(const UiTextLayoutEngineDescriptor &source);
        Storage(const Storage &) = delete;
        Storage &operator=(const Storage &) = delete;
        Storage(Storage &&) = delete;
        Storage &operator=(Storage &&) = delete;

        [[nodiscard]] std::shared_ptr<UiTextLayoutResult::Storage> TryAcquire() noexcept;
        void ReleaseSlot(const std::shared_ptr<UiTextLayoutResult::Storage> &slot) noexcept;
        [[nodiscard]] Result<void> BuildFaceTable(const UiTextShapedTextView &view, std::vector<UiTextFaceId> &output);
        [[nodiscard]] Result<std::int64_t> ShapedWidth(const UiTextShapedTextView &view, UiTextScale scale) const;
        [[nodiscard]] Result<void> BuildLinePlans(const UiTextLayoutRequest &request);
        [[nodiscard]] Result<void> AppendLinePlan(std::uint32_t first, std::uint32_t end, bool hardBreak);
        [[nodiscard]] std::uint32_t SelectWrapSplit(std::uint32_t index, std::uint32_t lineStart, UiTextWrapMode wrap) const noexcept;
        [[nodiscard]] Result<std::uint32_t> VisibleLineCount(const UiTextLayoutRequest &request, std::int32_t lineHeight) const;
        [[nodiscard]] Result<void> AppendGlyph(UiTextLayoutResult::Storage &slot, UiTextFaceId face, std::uint32_t glyph,
                                               std::uint32_t cluster, UiLogicalPoint origin, UiLogicalPoint advance, std::uint32_t line,
                                               bool ellipsis);
        [[nodiscard]] Result<void> BuildOutput(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot,
                                               std::int32_t lineHeight, std::uint32_t visibleLines, bool sourceTruncated);
        [[nodiscard]] Result<void> PrepareEllipsis(const UiTextLayoutRequest &request, bool needsEllipsis);
        [[nodiscard]] Result<UiTextLayoutInternal::OutputMetrics> PrepareOutputMetrics(const UiTextLayoutRequest &request,
                                                                                       std::int32_t lineHeight, std::uint32_t visibleLines);
        [[nodiscard]] Result<UiTextLayoutInternal::LineWindow> PrepareLineWindow(const UiTextLayoutRequest &request,
                                                                                 std::uint32_t lineIndex, std::uint32_t visibleLines,
                                                                                 bool verticallyTruncated,
                                                                                 std::int64_t ellipsisWidth) const;
        [[nodiscard]] Result<UiTextLayoutInternal::LinePlacement> PrepareLinePlacement(const UiTextLayoutRequest &request,
                                                                                       const UiTextLayoutInternal::LineWindow &window,
                                                                                       std::uint32_t lineIndex, std::uint32_t visibleLines,
                                                                                       std::int32_t lineHeight, std::int64_t verticalOffset,
                                                                                       std::int32_t scaledAscent) const;
        [[nodiscard]] Result<void> BuildLine(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot,
                                             const UiTextLayoutInternal::LineBuildContext &context);
        [[nodiscard]] Result<void> AppendLineRecord(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot,
                                                    const UiTextLayoutInternal::LineBuildContext &context,
                                                    const UiTextLayoutInternal::LineWindow &window,
                                                    const UiTextLayoutInternal::LinePlacement &placement, std::int32_t renderedWidth);
        [[nodiscard]] Result<void> AppendSourceRange(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot,
                                                     std::uint32_t end, UiTextLayoutInternal::SourceAppendState &state);
        [[nodiscard]] Result<void> AppendSourceCluster(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot,
                                                       std::uint32_t clusterIndex, UiTextLayoutInternal::SourceAppendState &state);
        [[nodiscard]] Result<std::uint32_t> CountEllipsisGlyphs(const UiTextLayoutRequest &request, std::int64_t available,
                                                                std::int64_t &usedWidth) const;
        [[nodiscard]] Result<void> AppendEllipsisGlyphs(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot,
                                                        std::uint32_t glyphCount, std::uint32_t lineIndex, UiLogicalPoint lineOrigin,
                                                        std::int64_t &cursor);
        [[nodiscard]] Result<std::shared_ptr<UiTextLayoutResult::Storage>> Layout(const UiTextLayoutRequest &request);
        [[nodiscard]] Result<void> BuildCandidate(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot);
        [[nodiscard]] Result<void> CommitCandidate(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot);
        [[nodiscard]] Result<UiTextLayoutRevision> NextPublication() const;
        [[nodiscard]] bool ValidateOutput(const UiTextLayoutResult::Storage &slot) const noexcept;
        [[nodiscard]] bool IsDrained() const noexcept;
    };
}  // namespace Horo::Runtime::Ui
