#include "Horo/Runtime/Ui/UiTextShaping.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiTextShapingInternal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <exception>
#include <hb-ot.h>
#include <hb.h>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utf8proc.h>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace TextShapingDetail {
        /** @brief Checks the closed direction enum range before mapping to HarfBuzz. */
        bool ValidDirection(const UiTextDirection direction) noexcept {
            return direction < UiTextDirection::Count;
        }

        /** @brief Checks a normalized ASCII script character. */
        bool ValidScriptCharacter(const char value) noexcept {
            return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
        }

        /** @brief Checks an ASCII letter used as the primary BCP-47 language subtag. */
        bool ValidLanguagePrimaryCharacter(const char value) noexcept {
            return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
        }

        /** @brief Checks a bounded ASCII language subtag character. */
        bool ValidLanguageCharacter(const char value) noexcept {
            return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '-';
        }

        /** @brief Checks a printable OpenType feature tag character. */
        bool ValidFeatureCharacter(const char value) noexcept {
            return std::isalnum(static_cast<unsigned char>(value)) != 0;
        }

        /** @brief Checks one bounded shaper limit against its global maximum. */
        bool ValidLimit(const std::uint32_t value, const std::uint32_t maximum) noexcept {
            return value > 0 && value <= maximum;
        }

        /** @brief Checks one feature request against the source text byte range. */
        bool ValidFeatureRange(const UiTextFeature &feature, const std::size_t textSize) noexcept {
            return feature.IsValid() && feature.start <= textSize && (feature.end == NoUiTextFeatureEnd || feature.end <= textSize);
        }

        /** @brief Checks request-level size and limit invariants before inspecting shaping evidence. */
        bool ValidTextRequestBounds(const UiTextShapingRequest &request, const UiTextShaperLimits &limits) noexcept {
            return limits.IsValid() && request.text.size() <= limits.maxInputBytes &&
                   request.text.size() <= std::numeric_limits<std::uint32_t>::max();
        }

        /** @brief Checks the typed shaping evidence and feature-count limit. */
        bool ValidTextRequestEvidence(const UiTextShapingRequest &request, const UiTextShaperLimits &limits) noexcept {
            return request.content.IsValid() && request.script.IsValid() && request.language.IsValid() && request.fontSize.IsValid() &&
                   ValidDirection(request.direction) && request.features.size() <= limits.maxFeatures;
        }

        /** @brief Checks an optional contiguous result range. */
        bool ValidIndexRange(const std::uint32_t first, const std::uint32_t count) noexcept {
            return (first == NoUiTextIndex && count == 0) || (first != NoUiTextIndex && count > 0);
        }

        /** @brief Checks a resolved, non-auto direction. */
        bool ValidResolvedDirection(const UiTextDirection direction) noexcept {
            return direction > UiTextDirection::Auto && direction < UiTextDirection::Count;
        }

        /** @brief Converts a four-byte Horo tag into HarfBuzz's packed tag. */
        hb_tag_t ToHbTag(const std::string_view value) noexcept {
            return hb_tag_from_string(value.data(), static_cast<int>(value.size()));
        }

        /** @brief Converts Horo direction evidence to a HarfBuzz buffer direction. */
        hb_direction_t ToHbDirection(const UiTextDirection direction) noexcept {
            switch (direction) {
                case UiTextDirection::LeftToRight:
                    return HB_DIRECTION_LTR;
                case UiTextDirection::RightToLeft:
                    return HB_DIRECTION_RTL;
                case UiTextDirection::TopToBottom:
                    return HB_DIRECTION_TTB;
                case UiTextDirection::BottomToTop:
                    return HB_DIRECTION_BTT;
                case UiTextDirection::Auto:
                case UiTextDirection::Count:
                    break;
            }
            return HB_DIRECTION_INVALID;
        }

        /** @brief Converts a HarfBuzz script to normalized four-byte Horo evidence. */
        Result<UiTextScript> FromHbScript(const hb_script_t script) {
            std::array<char, 5> value{};
            hb_tag_to_string(hb_script_to_iso15924_tag(script), value.data());
            const auto converted = UiTextScript::Create(std::string_view(value.data(), 4));
            return converted.HasValue() ? converted : Failure<UiTextScript>(UiErrors::TextShapeInvalid);
        }

        /** @brief Converts a normalized Horo script to HarfBuzz script evidence. */
        hb_script_t ToHbScript(const UiTextScript script) noexcept {
            return hb_script_from_iso15924_tag(ToHbTag(script.Value()));
        }

        /** @brief Converts a resolved HarfBuzz direction to Horo direction evidence. */
        UiTextDirection FromHbDirection(const hb_direction_t direction) noexcept {
            switch (direction) {
                case HB_DIRECTION_RTL:
                    return UiTextDirection::RightToLeft;
                case HB_DIRECTION_TTB:
                    return UiTextDirection::TopToBottom;
                case HB_DIRECTION_BTT:
                    return UiTextDirection::BottomToTop;
                case HB_DIRECTION_LTR:
                case HB_DIRECTION_INVALID:
                default:
                    return UiTextDirection::LeftToRight;
            }
        }

        /** @brief Checks whether a scalar is a variation selector handled with its preceding base. */
        bool IsVariationSelector(const std::uint32_t scalar) noexcept {
            return (scalar >= 0xFE00U && scalar <= 0xFE0FU) || (scalar >= 0xE0100U && scalar <= 0xE01EFU);
        }

        /** @brief Checks format controls that do not require an independent nominal glyph. */
        bool IsJoinerOrFormatControl(const std::uint32_t scalar) noexcept {
            return scalar == 0x200CU || scalar == 0x200DU || (scalar >= 0xE0020U && scalar <= 0xE007FU);
        }

        /** @brief Adds two logical coordinates without allowing signed overflow. */
        bool AddPoint(UiLogicalPoint &target, const UiLogicalPoint value) noexcept {
            const auto x = static_cast<std::int64_t>(target.x) + static_cast<std::int64_t>(value.x);
            const auto y = static_cast<std::int64_t>(target.y) + static_cast<std::int64_t>(value.y);
            if (x < std::numeric_limits<std::int32_t>::min() || x > std::numeric_limits<std::int32_t>::max() ||
                y < std::numeric_limits<std::int32_t>::min() || y > std::numeric_limits<std::int32_t>::max())
                return false;
            target.x = static_cast<std::int32_t>(x);
            target.y = static_cast<std::int32_t>(y);
            return true;
        }

        /** @brief Returns the non-negative magnitude of a signed logical coordinate. */
        std::int64_t Magnitude(const std::int32_t value) noexcept {
            return value < 0 ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
        }

        /** @brief Converts one bounded signed metric to a non-negative magnitude. */
        bool PositiveMetric(const hb_position_t value, std::int32_t &result) noexcept {
            const auto magnitude = Magnitude(static_cast<std::int32_t>(value));
            if (magnitude > std::numeric_limits<std::int32_t>::max())
                return false;
            result = static_cast<std::int32_t>(magnitude);
            return true;
        }

        /** @brief Releases one fallible HarfBuzz font owner. */
        void HbFontDeleter::operator()(hb_font_t *font) const noexcept {
            if (font != nullptr)
                hb_font_destroy(font);
        }

        /** @brief Checks a complete fallback cluster against one face without splitting its scalars. */
        bool SupportsCluster(hb_font_t *font, const std::vector<DecodedScalar> &scalars, const std::size_t first,
                             const std::size_t last) noexcept {
            hb_codepoint_t previousBase = 0;
            bool hasPreviousBase = false;
            for (std::size_t index = first; index < last; ++index) {
                const auto scalar = scalars[index].value;
                if (IsVariationSelector(scalar)) {
                    hb_codepoint_t glyph{};
                    if (!hasPreviousBase || !hb_font_get_variation_glyph(font, previousBase, scalar, &glyph) || glyph == 0)
                        return false;
                    continue;
                }
                if (IsJoinerOrFormatControl(scalar))
                    continue;
                hb_codepoint_t glyph{};
                if (!hb_font_get_nominal_glyph(font, scalar, &glyph) || glyph == 0)
                    return false;
                previousBase = scalar;
                hasPreviousBase = true;
            }
            return true;
        }

        /** @brief Returns the face's missing-glyph advance in the selected logical direction. */
        UiLogicalPoint MissingAdvance(hb_font_t *font, const UiTextDirection direction) noexcept {
            if (direction == UiTextDirection::TopToBottom || direction == UiTextDirection::BottomToTop)
                return UiLogicalPoint{0, static_cast<std::int32_t>(hb_font_get_glyph_v_advance(font, 0))};
            return UiLogicalPoint{static_cast<std::int32_t>(hb_font_get_glyph_h_advance(font, 0)), 0};
        }

        /** @brief Applies one font's bounded metrics to the result's maximum line metrics. */
        bool AccumulateMetrics(hb_font_t *font, const UiTextDirection direction, const UiTextFontSize size,
                               UiTextMetrics &metrics) noexcept {
            hb_font_set_scale(font, size.value, size.value);
            hb_font_extents_t extents{};
            hb_font_get_extents_for_direction(font, ToHbDirection(direction), &extents);
            std::int32_t ascent{};
            std::int32_t descent{};
            std::int32_t lineGap{};
            if (!PositiveMetric(extents.ascender, ascent) || !PositiveMetric(extents.descender, descent) ||
                !PositiveMetric(extents.line_gap, lineGap))
                return false;
            metrics.ascent = std::max(metrics.ascent, ascent);
            metrics.descent = std::max(metrics.descent, descent);
            metrics.lineGap = std::max(metrics.lineGap, lineGap);
            return true;
        }

        /** @brief Decodes a validated UTF-8 request while retaining exact source byte boundaries. */
        Result<void> DecodeText(const std::string_view text, std::vector<DecodedScalar> &scalars) {
            scalars.clear();
            std::size_t offset = 0;
            while (offset < text.size()) {
                utf8proc_int32_t scalar{};
                const auto remaining = static_cast<utf8proc_ssize_t>(text.size() - offset);
                const auto decoded = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(text.data() + offset), remaining, &scalar);
                if (decoded <= 0)
                    return Failure(UiErrors::TextInputInvalid);
                const auto end = offset + static_cast<std::size_t>(decoded);
                if (end > text.size() || end > std::numeric_limits<std::uint32_t>::max())
                    return Failure(UiErrors::TextInputInvalid);
                scalars.push_back(
                    DecodedScalar{static_cast<std::uint32_t>(scalar), static_cast<std::uint32_t>(offset), static_cast<std::uint32_t>(end)});
                offset = end;
            }
            return Result<void>::Success();
        }

        /** @brief Segments decoded scalars into deterministic grapheme fallback units. */
        Result<void> SegmentText(const std::vector<DecodedScalar> &scalars, std::vector<SourceCluster> &clusters) {
            clusters.clear();
            if (scalars.empty())
                return Result<void>::Success();
            std::size_t start = 0;
            utf8proc_int32_t state = 0;
            for (std::size_t index = 1; index < scalars.size(); ++index) {
                const auto breaks = utf8proc_grapheme_break_stateful(static_cast<utf8proc_int32_t>(scalars[index - 1].value),
                                                                     static_cast<utf8proc_int32_t>(scalars[index].value), &state);
                if (breaks) {
                    clusters.push_back(SourceCluster{start, index, scalars[start].byteStart, scalars[index - 1].byteEnd, 0,
                                                     UiTextScript::Auto(), UiTextDirection::LeftToRight, false});
                    start = index;
                }
            }
            clusters.push_back(SourceCluster{start, scalars.size(), scalars[start].byteStart, scalars.back().byteEnd, 0,
                                             UiTextScript::Auto(), UiTextDirection::LeftToRight, false});
            return Result<void>::Success();
        }
    }  // namespace TextShapingDetail

    using namespace TextShapingDetail;

    /** @copydoc UiFontFaceDescriptor::IsValid */
    bool UiFontFaceDescriptor::IsValid() const noexcept {
        return id.IsValid();
    }

    /** @brief Releases the native face before the immutable font blob. */
    UiFontFace::Storage::~Storage() {
        if (face != nullptr)
            hb_face_destroy(face);
        if (blob != nullptr)
            hb_blob_destroy(blob);
    }

    /** @copydoc UiFontFace::Create */
    Result<UiFontFace> UiFontFace::Create(const UiFontFaceDescriptor &descriptor, const std::span<const std::uint8_t> bytes) {
        if (!descriptor.IsValid() || bytes.empty() || bytes.size() > MaximumUiFontFaceBytes)
            return Failure<UiFontFace>(UiErrors::TextFontInvalid);
        try {
            auto storage = std::make_shared<Storage>();
            storage->descriptor = descriptor;
            storage->bytes.assign(bytes.begin(), bytes.end());
            storage->blob = hb_blob_create(reinterpret_cast<const char *>(storage->bytes.data()), storage->bytes.size(),
                                           HB_MEMORY_MODE_READONLY, nullptr, nullptr);
            if (storage->blob == nullptr || hb_face_count(storage->blob) <= descriptor.collectionIndex)
                return Failure<UiFontFace>(UiErrors::TextFontInvalid);
            storage->face = hb_face_create(storage->blob, descriptor.collectionIndex);
            if (storage->face == nullptr || hb_face_get_upem(storage->face) == 0 || hb_face_get_glyph_count(storage->face) == 0)
                return Failure<UiFontFace>(UiErrors::TextFontInvalid);
            return Result<UiFontFace>::Success(UiFontFace{std::move(storage)});
        } catch (const std::bad_alloc &) {
            return Failure<UiFontFace>(UiErrors::TextFontInvalid);
        }
    }

    /** @copydoc UiFontFace::~UiFontFace */
    UiFontFace::~UiFontFace() = default;

    /** @copydoc UiFontFace::UiFontFace(const UiFontFace&) */
    UiFontFace::UiFontFace(const UiFontFace &other) noexcept = default;

    /** @copydoc UiFontFace::operator=(const UiFontFace&) */
    UiFontFace &UiFontFace::operator=(const UiFontFace &other) noexcept = default;

    /** @copydoc UiFontFace::UiFontFace(UiFontFace&&) */
    UiFontFace::UiFontFace(UiFontFace &&other) noexcept = default;

    /** @copydoc UiFontFace::operator=(UiFontFace&&) */
    UiFontFace &UiFontFace::operator=(UiFontFace &&other) noexcept = default;

    /** @copydoc UiFontFace::UiFontFace(std::shared_ptr<const Storage>) */
    UiFontFace::UiFontFace(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiFontFace::Id */
    UiFontFaceId UiFontFace::Id() const noexcept {
        return storage_ ? storage_->descriptor.id : UiFontFaceId{};
    }

    /** @copydoc UiFontFace::CollectionIndex */
    std::uint32_t UiFontFace::CollectionIndex() const noexcept {
        return storage_ ? storage_->descriptor.collectionIndex : 0;
    }

    /** @copydoc UiFontFace::UnitsPerEm */
    std::uint32_t UiFontFace::UnitsPerEm() const noexcept {
        return storage_ ? hb_face_get_upem(storage_->face) : 0;
    }

    /** @copydoc UiFontFace::IsValid */
    bool UiFontFace::IsValid() const noexcept {
        return storage_ != nullptr && storage_->face != nullptr && storage_->descriptor.IsValid() && UnitsPerEm() != 0;
    }

    /** @copydoc UiFontFallbackChain::Create */
    Result<UiFontFallbackChain> UiFontFallbackChain::Create(const std::span<const UiFontFace> faces,
                                                            const UiMissingGlyphPolicy missingGlyphPolicy) {
        if (faces.empty() || faces.size() > MaximumUiFallbackFaces || missingGlyphPolicy >= UiMissingGlyphPolicy::Count)
            return Failure<UiFontFallbackChain>(UiErrors::TextFallbackInvalid);
        for (std::size_t index = 0; index < faces.size(); ++index) {
            if (!faces[index].IsValid())
                return Failure<UiFontFallbackChain>(UiErrors::TextFallbackInvalid);
            for (std::size_t prior = 0; prior < index; ++prior)
                if (faces[prior].Id() == faces[index].Id())
                    return Failure<UiFontFallbackChain>(UiErrors::TextFallbackInvalid);
        }
        try {
            UiFontFallbackChain chain;
            chain.faces_.assign(faces.begin(), faces.end());
            chain.missingGlyphPolicy_ = missingGlyphPolicy;
            return Result<UiFontFallbackChain>::Success(std::move(chain));
        } catch (const std::bad_alloc &) {
            return Failure<UiFontFallbackChain>(UiErrors::TextFallbackInvalid);
        }
    }

    /** @copydoc UiFontFallbackChain::Faces */
    std::span<const UiFontFace> UiFontFallbackChain::Faces() const noexcept {
        return faces_;
    }

    /** @copydoc UiFontFallbackChain::Terminal */
    const UiFontFace *UiFontFallbackChain::Terminal() const noexcept {
        return faces_.empty() ? nullptr : &faces_.back();
    }

    /** @copydoc UiFontFallbackChain::MissingPolicy */
    UiMissingGlyphPolicy UiFontFallbackChain::MissingPolicy() const noexcept {
        return missingGlyphPolicy_;
    }

    /** @copydoc UiFontFallbackChain::IsValid */
    bool UiFontFallbackChain::IsValid() const noexcept {
        if (faces_.empty() || faces_.size() > MaximumUiFallbackFaces || missingGlyphPolicy_ >= UiMissingGlyphPolicy::Count)
            return false;
        for (std::size_t index = 0; index < faces_.size(); ++index) {
            if (!faces_[index].IsValid())
                return false;
            for (std::size_t prior = 0; prior < index; ++prior)
                if (faces_[prior].Id() == faces_[index].Id())
                    return false;
        }
        return true;
    }

    /** @copydoc UiTextScript::Auto */
    UiTextScript UiTextScript::Auto() noexcept {
        return {};
    }

    /** @copydoc UiTextScript::Create */
    Result<UiTextScript> UiTextScript::Create(const std::string_view value) {
        if (value.empty())
            return Result<UiTextScript>::Success(UiTextScript::Auto());
        if (value.size() != 4)
            return Failure<UiTextScript>(UiErrors::TextInputInvalid);
        UiTextScript script;
        for (std::size_t index = 0; index < value.size(); ++index) {
            if (!ValidScriptCharacter(value[index]))
                return Failure<UiTextScript>(UiErrors::TextInputInvalid);
            script.value_[index] = index == 0 ? static_cast<char>(std::toupper(static_cast<unsigned char>(value[index])))
                                              : static_cast<char>(std::tolower(static_cast<unsigned char>(value[index])));
        }
        return Result<UiTextScript>::Success(script);
    }

    /** @copydoc UiTextScript::IsValid */
    bool UiTextScript::IsValid() const noexcept {
        if (IsAuto())
            return true;
        return std::all_of(value_.begin(), value_.end(), ValidScriptCharacter);
    }

    /** @copydoc UiTextScript::IsAuto */
    bool UiTextScript::IsAuto() const noexcept {
        return value_ == std::array<char, 4>{};
    }

    /** @copydoc UiTextScript::Value */
    std::string_view UiTextScript::Value() const noexcept {
        return IsAuto() ? std::string_view{} : std::string_view(value_.data(), value_.size());
    }

    /** @copydoc UiTextLanguage::Auto */
    UiTextLanguage UiTextLanguage::Auto() noexcept {
        return {};
    }

    /** @copydoc UiTextLanguage::Create */
    Result<UiTextLanguage> UiTextLanguage::Create(const std::string_view value) {
        if (value.empty())
            return Result<UiTextLanguage>::Success(UiTextLanguage::Auto());
        if (value.size() >= MaximumUiTextLanguageBytes || value.front() == '-' || value.back() == '-')
            return Failure<UiTextLanguage>(UiErrors::TextInputInvalid);
        UiTextLanguage language;
        bool previousHyphen = false;
        if (!ValidLanguagePrimaryCharacter(value.front()))
            return Failure<UiTextLanguage>(UiErrors::TextInputInvalid);
        for (std::size_t index = 0; index < value.size(); ++index) {
            const char character = value[index];
            if (!ValidLanguageCharacter(character) || (character == '-' && previousHyphen))
                return Failure<UiTextLanguage>(UiErrors::TextInputInvalid);
            language.value_[index] = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            previousHyphen = character == '-';
        }
        language.size_ = static_cast<std::uint8_t>(value.size());
        return Result<UiTextLanguage>::Success(language);
    }

    /** @copydoc UiTextLanguage::IsValid */
    bool UiTextLanguage::IsValid() const noexcept {
        const auto value = View();
        if (value.empty())
            return true;
        if (value.size() >= MaximumUiTextLanguageBytes || value.front() == '-' || value.back() == '-' || value.front() < 'a' ||
            value.front() > 'z')
            return false;
        if (!std::all_of(value.begin(), value.end(), ValidLanguageCharacter))
            return false;
        return std::adjacent_find(value.begin(), value.end(), [](const char first, const char second) {
            return first == '-' && second == '-';
        }) == value.end();
    }

    /** @copydoc UiTextLanguage::IsAuto */
    bool UiTextLanguage::IsAuto() const noexcept {
        return size_ == 0;
    }

    /** @copydoc UiTextLanguage::View */
    std::string_view UiTextLanguage::View() const noexcept {
        return std::string_view(value_.data(), size_);
    }

    /** @copydoc UiTextFeature::Create */
    Result<UiTextFeature> UiTextFeature::Create(const std::string_view featureTag, const std::uint32_t featureValue,
                                                const std::uint32_t featureStart, const std::uint32_t featureEnd) {
        if (featureTag.size() != 4 || (featureEnd != NoUiTextFeatureEnd && featureStart > featureEnd))
            return Failure<UiTextFeature>(UiErrors::TextFeatureInvalid);
        UiTextFeature feature;
        feature.value = featureValue;
        feature.start = featureStart;
        feature.end = featureEnd;
        for (std::size_t index = 0; index < featureTag.size(); ++index) {
            if (!ValidFeatureCharacter(featureTag[index]))
                return Failure<UiTextFeature>(UiErrors::TextFeatureInvalid);
            feature.tag[index] = featureTag[index];
        }
        return Result<UiTextFeature>::Success(feature);
    }

    /** @copydoc UiTextFeature::IsValid */
    bool UiTextFeature::IsValid() const noexcept {
        return std::all_of(tag.begin(), tag.end(), ValidFeatureCharacter) && (end == NoUiTextFeatureEnd || start <= end);
    }

    /** @copydoc UiTextFeature::Tag */
    std::string_view UiTextFeature::Tag() const noexcept {
        return std::string_view(tag.data(), tag.size());
    }

    /** @copydoc UiTextFontSize::Create */
    Result<UiTextFontSize> UiTextFontSize::Create(const std::int32_t size) {
        UiTextFontSize result{size};
        return result.IsValid() ? Result<UiTextFontSize>::Success(result) : Failure<UiTextFontSize>(UiErrors::TextInputInvalid);
    }

    /** @copydoc UiTextFontSize::IsValid */
    bool UiTextFontSize::IsValid() const noexcept {
        return value > 0 && value <= 1024 * 1024;
    }

    /** @copydoc UiTextShaperLimits::IsValid */
    bool UiTextShaperLimits::IsValid() const noexcept {
        const std::array<std::pair<std::uint32_t, std::uint32_t>, 6> values{{
            {maxInputBytes, MaximumUiTextInputBytes},
            {maxFeatures, MaximumUiTextFeatures},
            {maxGlyphs, MaximumUiTextGlyphs},
            {maxClusters, MaximumUiTextClusters},
            {maxRuns, MaximumUiTextRuns},
            {concurrentShapes, MaximumUiTextShapesInFlight},
        }};
        return std::all_of(values.begin(), values.end(), [](const auto &value) {
            return ValidLimit(value.first, value.second);
        });
    }

    /** @copydoc UiTextShapingRequest::IsValid */
    bool UiTextShapingRequest::IsValid(const UiTextShaperLimits &limits) const noexcept {
        if (!ValidTextRequestBounds(*this, limits) || !ValidTextRequestEvidence(*this, limits))
            return false;
        return std::all_of(features.begin(), features.end(), [this](const auto &feature) {
            return ValidFeatureRange(feature, text.size());
        }) && IsValidUtf8ScalarSequence(text);
    }

    /** @copydoc UiTextGlyph::IsValid */
    bool UiTextGlyph::IsValid() const noexcept {
        return face.IsValid();
    }

    /** @copydoc UiTextGlyphRun::IsValid */
    bool UiTextGlyphRun::IsValid() const noexcept {
        return face.IsValid() && script.IsValid() && !script.IsAuto() && ValidResolvedDirection(direction) && byteStart < byteEnd &&
               firstCluster != NoUiTextIndex && ValidIndexRange(firstGlyph, glyphCount);
    }

    /** @copydoc UiTextCluster::IsValid */
    bool UiTextCluster::IsValid() const noexcept {
        return byteStart < byteEnd && ValidIndexRange(firstGlyph, glyphCount);
    }

    /** @copydoc UiTextMetrics::IsValid */
    bool UiTextMetrics::IsValid() const noexcept {
        return ascent >= 0 && descent >= 0 && lineGap >= 0 && bounds.IsValid();
    }

    /** @copydoc UiTextShapeDescriptor::IsValid */
    bool UiTextShapeDescriptor::IsValid() const noexcept {
        return ownership.IsValid() && content.IsValid() && font.IsValid() && shape.IsValid() && requestedScript.IsValid() &&
               resolvedScript.IsValid() && language.IsValid() && ValidDirection(requestedDirection) && ValidDirection(resolvedDirection) &&
               fontSize.IsValid();
    }

    /** @copydoc UiTextShaperDescriptor::IsValid */
    bool UiTextShaperDescriptor::IsValid() const noexcept {
        return ownership.IsValid() && font.IsValid() && fonts.IsValid() && limits.IsValid();
    }

}  // namespace Horo::Runtime::Ui
