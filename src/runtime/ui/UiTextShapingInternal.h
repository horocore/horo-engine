#pragma once

#include "Horo/Runtime/Ui/UiTextShaping.h"

#include <atomic>
#include <hb.h>
#include <memory>
#include <string>
#include <vector>

namespace Horo::Runtime::Ui::TextShapingDetail {
    template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
        return Result<T>::Failure(MakeError(descriptor));
    }

    [[nodiscard]] bool ValidDirection(UiTextDirection direction) noexcept;
    [[nodiscard]] hb_tag_t ToHbTag(std::string_view value) noexcept;
    [[nodiscard]] hb_direction_t ToHbDirection(UiTextDirection direction) noexcept;
    [[nodiscard]] Result<UiTextScript> FromHbScript(hb_script_t script);
    [[nodiscard]] hb_script_t ToHbScript(UiTextScript script) noexcept;
    [[nodiscard]] UiTextDirection FromHbDirection(hb_direction_t direction) noexcept;
    [[nodiscard]] bool IsVariationSelector(std::uint32_t scalar) noexcept;
    [[nodiscard]] bool IsJoinerOrFormatControl(std::uint32_t scalar) noexcept;
    [[nodiscard]] bool AddPoint(UiLogicalPoint &target, UiLogicalPoint value) noexcept;
    [[nodiscard]] std::int64_t Magnitude(std::int32_t value) noexcept;
    [[nodiscard]] bool PositiveMetric(hb_position_t value, std::int32_t &result) noexcept;

    struct DecodedScalar final {
        std::uint32_t value{};
        std::uint32_t byteStart{};
        std::uint32_t byteEnd{};
    };

    struct SourceCluster final {
        std::size_t scalarStart{};
        std::size_t scalarEnd{};
        std::uint32_t byteStart{};
        std::uint32_t byteEnd{};
        std::size_t faceIndex{};
        UiTextScript script;
        UiTextDirection direction{UiTextDirection::LeftToRight};
        bool missing{};
    };

    struct HbFontDeleter final {
        void operator()(hb_font_t *font) const noexcept;
    };

    using HbFontOwner = std::unique_ptr<hb_font_t, HbFontDeleter>;

    [[nodiscard]] bool SupportsCluster(hb_font_t *font, const std::vector<DecodedScalar> &scalars, std::size_t first,
                                       std::size_t last) noexcept;
    [[nodiscard]] UiLogicalPoint MissingAdvance(hb_font_t *font, UiTextDirection direction) noexcept;
    [[nodiscard]] bool AccumulateMetrics(hb_font_t *font, UiTextDirection direction, UiTextFontSize size, UiTextMetrics &metrics) noexcept;
    [[nodiscard]] Result<void> DecodeText(std::string_view text, std::vector<DecodedScalar> &scalars);
    [[nodiscard]] Result<void> SegmentText(const std::vector<DecodedScalar> &scalars, std::vector<SourceCluster> &clusters);
}  // namespace Horo::Runtime::Ui::TextShapingDetail

namespace Horo::Runtime::Ui {
    struct UiFontFace::Storage final {
        UiFontFaceDescriptor descriptor;
        std::vector<std::uint8_t> bytes;
        hb_blob_t *blob{};
        hb_face_t *face{};

        ~Storage();
    };

    struct UiTextShape::Storage final {
        mutable std::atomic<std::uint64_t> leases{0};
        UiTextShapeDescriptor descriptor;
        std::string text;
        std::vector<UiTextFeature> features;
        UiTextMetrics metrics;
        std::vector<UiTextGlyphRun> runs;
        std::vector<UiTextGlyph> glyphs;
        std::vector<UiTextCluster> clusters;

        explicit Storage(const UiTextShaperLimits &limits) {
            text.reserve(limits.maxInputBytes);
            features.reserve(limits.maxFeatures);
            runs.reserve(limits.maxRuns);
            glyphs.reserve(limits.maxGlyphs);
            clusters.reserve(limits.maxClusters);
        }
    };
}  // namespace Horo::Runtime::Ui
