#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiTextShapingInternal.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <hb-ot.h>
#include <hb.h>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    using namespace TextShapingDetail;

    namespace {
        struct FaceSelection final {
            std::size_t index{};
            bool missing{};
        };

        struct ShapedGlyphBatch final {
            std::uint32_t firstGlyph{NoUiTextIndex};
            std::size_t glyphCount{};
            UiLogicalPoint advance;
        };

        /** @brief Resolves one source cluster's script evidence without splitting its scalars. */
        hb_script_t DetectClusterScript(hb_unicode_funcs_t *unicode, const UiTextScript requested,
                                        const std::vector<DecodedScalar> &scalars, const SourceCluster &cluster) noexcept {
            if (!requested.IsAuto())
                return ToHbScript(requested);
            for (std::size_t index = cluster.scalarStart; index < cluster.scalarEnd; ++index) {
                const auto candidate = hb_unicode_script(unicode, scalars[index].value);
                if (candidate != HB_SCRIPT_COMMON && candidate != HB_SCRIPT_INHERITED)
                    return candidate;
            }
            return HB_SCRIPT_COMMON;
        }

        /** @brief Resolves one source cluster's explicit or script-derived direction. */
        UiTextDirection ResolveClusterDirection(const UiTextShapingRequest &request, const hb_script_t script) noexcept {
            return request.direction == UiTextDirection::Auto ? FromHbDirection(hb_script_get_horizontal_direction(script))
                                                              : request.direction;
        }

        /** @brief Selects the first face covering a complete source cluster. */
        Result<FaceSelection> SelectClusterFace(const std::span<const UiFontFace> faces, const std::vector<hb_font_t *> &fonts,
                                                const std::vector<DecodedScalar> &scalars, const SourceCluster &cluster,
                                                const UiMissingGlyphPolicy policy) {
            for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex)
                if (SupportsCluster(fonts[faceIndex], scalars, cluster.scalarStart, cluster.scalarEnd))
                    return Result<FaceSelection>::Success(FaceSelection{faceIndex, false});
            if (policy == UiMissingGlyphPolicy::FailStrict)
                return Failure<FaceSelection>(UiErrors::TextMissingCoverage);
            return Result<FaceSelection>::Success(FaceSelection{faces.size() - 1, true});
        }
    }  // namespace

    struct UiTextShaper::Storage final {
        UiTextShaperDescriptor descriptor;
        UiTextShaperState lifecycle{UiTextShaperState::Active};
        std::vector<std::shared_ptr<UiTextShape::Storage>> slots;
        std::size_t nextSlot{};
        std::uint64_t nextRevision{1};
        std::vector<hb_font_t *> fonts;
        hb_buffer_t *buffer{};
        std::vector<DecodedScalar> scalars;
        std::vector<SourceCluster> sourceClusters;
        std::vector<hb_feature_t> hbFeatures;
        std::vector<std::uint32_t> shapedClusterStarts;
        std::vector<std::uint32_t> shapedClusterFirstGlyphs;
        std::vector<std::uint32_t> shapedClusterGlyphCounts;
        std::vector<UiLogicalPoint> shapedClusterAdvances;

        explicit Storage(const UiTextShaperDescriptor &value) : descriptor(value) {}

        ~Storage() {
            ShutdownNative();
        }

        /** @brief Allocates immutable slots and the private HarfBuzz buffer. */
        bool Initialize() {
            buffer = hb_buffer_create();
            if (buffer == nullptr)
                return false;
            hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
            slots.reserve(descriptor.limits.concurrentShapes);
            for (std::uint32_t index = 0; index < descriptor.limits.concurrentShapes; ++index)
                slots.push_back(std::make_shared<UiTextShape::Storage>(descriptor.limits));
            scalars.reserve(descriptor.limits.maxInputBytes);
            sourceClusters.reserve(descriptor.limits.maxClusters);
            hbFeatures.reserve(descriptor.limits.maxFeatures);
            shapedClusterStarts.reserve(descriptor.limits.maxClusters);
            shapedClusterFirstGlyphs.reserve(descriptor.limits.maxClusters);
            shapedClusterGlyphCounts.reserve(descriptor.limits.maxClusters);
            shapedClusterAdvances.reserve(descriptor.limits.maxClusters);
            return true;
        }

        /** @brief Releases native shaping state while retaining leased immutable result slots. */
        void ShutdownNative() noexcept {
            if (buffer != nullptr) {
                hb_buffer_destroy(buffer);
                buffer = nullptr;
            }
            for (auto *font : fonts)
                if (font != nullptr)
                    hb_font_destroy(font);
            fonts.clear();
        }

        /** @brief Acquires one unleased immutable result slot without allocating. */
        std::shared_ptr<UiTextShape::Storage> TryAcquire() noexcept {
            if (slots.empty())
                return {};
            for (std::size_t offset = 0; offset < slots.size(); ++offset) {
                const auto index = (nextSlot + offset) % slots.size();
                auto &slot = slots[index];
                std::uint64_t expected = 0;
                if (slot->leases.compare_exchange_strong(expected, 1)) {
                    nextSlot = (index + 1) % slots.size();
                    return slot;
                }
            }
            return {};
        }

        /** @brief Checks one exact source range is within all reserved result bounds. */
        bool Fits(const std::size_t glyphs, const std::size_t clusters, const std::size_t runs) const noexcept {
            return glyphs <= descriptor.limits.maxGlyphs && clusters <= descriptor.limits.maxClusters && runs <= descriptor.limits.maxRuns;
        }

        /** @brief Resolves script and direction, chooses complete-cluster fallback, and records source evidence. */
        Result<void> ResolveClusters(const UiTextShapingRequest &request) {
            auto *unicode = hb_unicode_funcs_get_default();
            const auto faces = descriptor.fonts.Faces();
            for (auto &cluster : sourceClusters) {
                const auto hbScript = DetectClusterScript(unicode, request.script, scalars, cluster);
                const auto script = FromHbScript(hbScript);
                if (script.HasError())
                    return Result<void>::Failure(script.ErrorValue());
                cluster.script = script.Value();
                cluster.direction = ResolveClusterDirection(request, hbScript);
                const auto selection = SelectClusterFace(faces, fonts, scalars, cluster, descriptor.fonts.MissingPolicy());
                if (selection.HasError())
                    return Result<void>::Failure(selection.ErrorValue());
                cluster.faceIndex = selection.Value().index;
                cluster.missing = selection.Value().missing;
            }
            return Result<void>::Success();
        }

        /** @brief Converts the request's feature values once before shaping any run. */
        Result<void> PrepareFeatures(const std::span<const UiTextFeature> features) {
            hbFeatures.clear();
            for (const auto &feature : features) {
                hb_feature_t converted{};
                converted.tag = ToHbTag(feature.Tag());
                converted.value = feature.value;
                converted.start = feature.start;
                converted.end = feature.end;
                hbFeatures.push_back(converted);
            }
            return Result<void>::Success();
        }

        /** @brief Appends one omitted source group with terminal-face replacement advances. */
        Result<void> AppendOmittedGroup(UiTextShape::Storage &slot, const std::size_t first, const std::size_t last,
                                        const UiTextShapingRequest &request) {
            const auto &group = sourceClusters[first];
            auto *font = fonts[group.faceIndex];
            hb_font_set_scale(font, request.fontSize.value, request.fontSize.value);
            const auto missingAdvance = MissingAdvance(font, group.direction);
            const auto firstCluster = static_cast<std::uint32_t>(slot.clusters.size());
            UiLogicalPoint runAdvance{};
            for (std::size_t index = first; index < last; ++index) {
                if (!Fits(slot.glyphs.size(), slot.clusters.size() + 1, slot.runs.size() + 1))
                    return Failure(UiErrors::CapacityExceeded);
                const auto &cluster = sourceClusters[index];
                slot.clusters.push_back(UiTextCluster{cluster.byteStart, cluster.byteEnd, NoUiTextIndex, 0, missingAdvance, true});
                if (!AddPoint(runAdvance, missingAdvance))
                    return Failure(UiErrors::TextShapeInvalid);
            }
            if (!AccumulateMetrics(font, group.direction, request.fontSize, slot.metrics))
                return Failure(UiErrors::TextShapeInvalid);
            if (!AddPoint(slot.metrics.advance, runAdvance))
                return Failure(UiErrors::TextShapeInvalid);
            slot.runs.push_back(UiTextGlyphRun{descriptor.fonts.Faces()[group.faceIndex].Id(), group.script, group.direction, NoUiTextIndex,
                                               0, firstCluster, static_cast<std::uint32_t>(slot.clusters.size() - firstCluster),
                                               group.byteStart, sourceClusters[last - 1].byteEnd, runAdvance});
            return Result<void>::Success();
        }

        /** @brief Prepares and shapes one contiguous source group in the reusable HarfBuzz buffer. */
        Result<void> PrepareBuffer(const std::size_t first, const std::size_t last, const UiTextShapingRequest &request) {
            const auto &group = sourceClusters[first];
            auto *font = fonts[group.faceIndex];
            hb_font_set_scale(font, request.fontSize.value, request.fontSize.value);
            hb_buffer_clear_contents(buffer);
            hb_buffer_set_direction(buffer, ToHbDirection(group.direction));
            hb_buffer_set_script(buffer, ToHbScript(group.script));
            hb_buffer_set_language(buffer, request.language.IsAuto()
                                               ? HB_LANGUAGE_INVALID
                                               : hb_language_from_string(request.language.View().data(),
                                                                         static_cast<int>(request.language.View().size())));
            for (std::size_t clusterIndex = first; clusterIndex < last; ++clusterIndex)
                for (std::size_t scalarIndex = sourceClusters[clusterIndex].scalarStart;
                     scalarIndex < sourceClusters[clusterIndex].scalarEnd; ++scalarIndex)
                    hb_buffer_add(buffer, scalars[scalarIndex].value, scalars[scalarIndex].byteStart);
            if (!hb_buffer_allocation_successful(buffer) ||
                !hb_shape_full(font, buffer, hbFeatures.data(), static_cast<unsigned int>(hbFeatures.size()), nullptr) ||
                !hb_buffer_allocation_successful(buffer))
                return Failure(UiErrors::TextShapeInvalid);
            return Result<void>::Success();
        }

        /** @brief Copies positioned glyphs from the shaped HarfBuzz buffer into one result slot. */
        Result<ShapedGlyphBatch> AppendGlyphs(UiTextShape::Storage &slot, const std::size_t first, const UiTextShapingRequest &request) {
            const auto glyphCount = static_cast<std::size_t>(hb_buffer_get_length(buffer));
            if (!Fits(slot.glyphs.size() + glyphCount, slot.clusters.size() + 1, slot.runs.size() + 1) ||
                glyphCount > descriptor.limits.maxGlyphs)
                return Failure<ShapedGlyphBatch>(UiErrors::CapacityExceeded);
            const auto *infos = hb_buffer_get_glyph_infos(buffer, nullptr);
            const auto *positions = hb_buffer_get_glyph_positions(buffer, nullptr);
            if (glyphCount != 0 && (infos == nullptr || positions == nullptr))
                return Failure<ShapedGlyphBatch>(UiErrors::TextShapeInvalid);
            const auto firstGlyph = static_cast<std::uint32_t>(slot.glyphs.size());
            UiLogicalPoint runAdvance{};
            for (std::size_t index = 0; index < glyphCount; ++index) {
                if (infos[index].cluster >= request.text.size())
                    return Failure<ShapedGlyphBatch>(UiErrors::TextShapeInvalid);
                const UiLogicalPoint advance{static_cast<std::int32_t>(positions[index].x_advance),
                                             static_cast<std::int32_t>(positions[index].y_advance)};
                const UiLogicalPoint offset{static_cast<std::int32_t>(positions[index].x_offset),
                                            static_cast<std::int32_t>(positions[index].y_offset)};
                slot.glyphs.push_back(UiTextGlyph{descriptor.fonts.Faces()[sourceClusters[first].faceIndex].Id(), infos[index].codepoint,
                                                  infos[index].cluster, offset, advance});
                if (!AddPoint(runAdvance, advance))
                    return Failure<ShapedGlyphBatch>(UiErrors::TextShapeInvalid);
            }
            shapedClusterStarts.clear();
            for (std::size_t index = 0; index < glyphCount; ++index)
                shapedClusterStarts.push_back(infos[index].cluster);
            std::sort(shapedClusterStarts.begin(), shapedClusterStarts.end());
            shapedClusterStarts.erase(std::unique(shapedClusterStarts.begin(), shapedClusterStarts.end()), shapedClusterStarts.end());
            return Result<ShapedGlyphBatch>::Success(ShapedGlyphBatch{firstGlyph, glyphCount, runAdvance});
        }

        /** @brief Aggregates shaped glyphs back into their complete source grapheme clusters. */
        Result<void> AppendSourceClusters(UiTextShape::Storage &slot, const std::size_t first, const std::size_t last,
                                          const ShapedGlyphBatch &batch, const bool missing) {
            if (slot.clusters.size() + (last - first) > descriptor.limits.maxClusters)
                return Failure(UiErrors::CapacityExceeded);
            std::size_t assignedGlyphs = 0;
            for (std::size_t clusterIndex = first; clusterIndex < last; ++clusterIndex) {
                const auto &source = sourceClusters[clusterIndex];
                UiTextCluster cluster{source.byteStart, source.byteEnd, NoUiTextIndex, 0, {}, missing};
                for (std::size_t glyphIndex = 0; glyphIndex < batch.glyphCount; ++glyphIndex) {
                    const auto &glyph = slot.glyphs[batch.firstGlyph + glyphIndex];
                    if (glyph.cluster < source.byteStart || glyph.cluster >= source.byteEnd)
                        continue;
                    if (cluster.firstGlyph == NoUiTextIndex)
                        cluster.firstGlyph = static_cast<std::uint32_t>(batch.firstGlyph + glyphIndex);
                    ++cluster.glyphCount;
                    ++assignedGlyphs;
                    if (!AddPoint(cluster.advance, glyph.advance))
                        return Failure(UiErrors::TextShapeInvalid);
                }
                slot.clusters.push_back(cluster);
            }
            return assignedGlyphs == batch.glyphCount ? Result<void>::Success() : Failure(UiErrors::TextShapeInvalid);
        }

        /** @brief Accumulates glyph-to-cluster ranges and advances for one shaped group. */
        Result<void> AccumulateClusterGlyphs(const UiTextShape::Storage &slot, const ShapedGlyphBatch &batch) {
            for (std::size_t glyphIndex = 0; glyphIndex < batch.glyphCount; ++glyphIndex) {
                const auto found = std::lower_bound(shapedClusterStarts.begin(), shapedClusterStarts.end(),
                                                    slot.glyphs[batch.firstGlyph + glyphIndex].cluster);
                if (found == shapedClusterStarts.end() || *found != slot.glyphs[batch.firstGlyph + glyphIndex].cluster)
                    return Failure(UiErrors::TextShapeInvalid);
                const auto clusterIndex = static_cast<std::size_t>(found - shapedClusterStarts.begin());
                if (shapedClusterFirstGlyphs[clusterIndex] == NoUiTextIndex)
                    shapedClusterFirstGlyphs[clusterIndex] = static_cast<std::uint32_t>(batch.firstGlyph + glyphIndex);
                ++shapedClusterGlyphCounts[clusterIndex];
                if (!AddPoint(shapedClusterAdvances[clusterIndex], slot.glyphs[batch.firstGlyph + glyphIndex].advance))
                    return Failure(UiErrors::TextShapeInvalid);
            }
            return Result<void>::Success();
        }

        /** @brief Appends sorted HarfBuzz clusters with source-byte ranges and advances. */
        Result<void> AppendMappedClusters(UiTextShape::Storage &slot, const std::uint32_t sourceByteEnd, const ShapedGlyphBatch &batch) {
            if (slot.clusters.size() + shapedClusterStarts.size() > descriptor.limits.maxClusters)
                return Failure(UiErrors::CapacityExceeded);
            shapedClusterFirstGlyphs.assign(shapedClusterStarts.size(), NoUiTextIndex);
            shapedClusterGlyphCounts.assign(shapedClusterStarts.size(), 0);
            shapedClusterAdvances.assign(shapedClusterStarts.size(), UiLogicalPoint{});
            if (const auto accumulated = AccumulateClusterGlyphs(slot, batch); accumulated.HasError())
                return accumulated;
            for (std::size_t index = 0; index < shapedClusterStarts.size(); ++index) {
                const auto byteStart = shapedClusterStarts[index];
                const auto byteEnd = index + 1 < shapedClusterStarts.size() ? shapedClusterStarts[index + 1] : sourceByteEnd;
                if (byteStart >= byteEnd || byteEnd > sourceByteEnd)
                    return Failure(UiErrors::TextShapeInvalid);
                slot.clusters.push_back(UiTextCluster{byteStart, byteEnd, shapedClusterFirstGlyphs[index], shapedClusterGlyphCounts[index],
                                                      shapedClusterAdvances[index], false});
            }
            return Result<void>::Success();
        }

        /** @brief Shapes one contiguous face/script/direction group and publishes stable glyph clusters. */
        Result<void> AppendShapedGroup(UiTextShape::Storage &slot, const std::size_t first, const std::size_t last,
                                       const UiTextShapingRequest &request) {
            const auto &group = sourceClusters[first];
            if (const auto prepared = PrepareBuffer(first, last, request); prepared.HasError())
                return prepared;
            const auto batch = AppendGlyphs(slot, first, request);
            if (batch.HasError())
                return Result<void>::Failure(batch.ErrorValue());
            const auto firstCluster = static_cast<std::uint32_t>(slot.clusters.size());
            const auto clusters =
                group.missing ? AppendSourceClusters(slot, first, last, batch.Value(), true)
                              : (shapedClusterStarts.empty() ? AppendSourceClusters(slot, first, last, batch.Value(), false)
                                                             : AppendMappedClusters(slot, sourceClusters[last - 1].byteEnd, batch.Value()));
            if (clusters.HasError())
                return clusters;
            auto *font = fonts[group.faceIndex];
            if (!AccumulateMetrics(font, group.direction, request.fontSize, slot.metrics))
                return Failure(UiErrors::TextShapeInvalid);
            if (!AddPoint(slot.metrics.advance, batch.Value().advance))
                return Failure(UiErrors::TextShapeInvalid);
            slot.runs.push_back(UiTextGlyphRun{descriptor.fonts.Faces()[group.faceIndex].Id(), group.script, group.direction,
                                               batch.Value().glyphCount == 0 ? NoUiTextIndex : batch.Value().firstGlyph,
                                               static_cast<std::uint32_t>(batch.Value().glyphCount), firstCluster,
                                               static_cast<std::uint32_t>(slot.clusters.size() - firstCluster), group.byteStart,
                                               sourceClusters[last - 1].byteEnd, batch.Value().advance});
            return Result<void>::Success();
        }

        /** @brief Finds the next source group with matching face and shaping evidence. */
        std::size_t FindGroupEnd(const std::size_t first) const noexcept {
            std::size_t last = first + 1;
            while (last < sourceClusters.size() && sourceClusters[last].faceIndex == sourceClusters[first].faceIndex &&
                   sourceClusters[last].script == sourceClusters[first].script &&
                   sourceClusters[last].direction == sourceClusters[first].direction &&
                   sourceClusters[last].missing == sourceClusters[first].missing)
                ++last;
            return last;
        }

        /** @brief Appends all resolved source groups to one result slot. */
        Result<void> AppendGroups(UiTextShape::Storage &slot, const UiTextShapingRequest &request) {
            if (sourceClusters.empty()) {
                const auto direction = request.direction == UiTextDirection::Auto ? UiTextDirection::LeftToRight : request.direction;
                return AccumulateMetrics(fonts.front(), direction, request.fontSize, slot.metrics) ? Result<void>::Success()
                                                                                                   : Failure(UiErrors::TextShapeInvalid);
            }
            std::size_t first = 0;
            while (first < sourceClusters.size()) {
                const auto last = FindGroupEnd(first);
                const auto omitted =
                    sourceClusters[first].missing && descriptor.fonts.MissingPolicy() == UiMissingGlyphPolicy::OmitWithAdvance;
                const auto result =
                    omitted ? AppendOmittedGroup(slot, first, last, request) : AppendShapedGroup(slot, first, last, request);
                if (result.HasError())
                    return result;
                first = last;
            }
            return Result<void>::Success();
        }

        /** @brief Finalizes conservative logical bounds after all groups have contributed metrics. */
        Result<void> FinalizeMetrics(UiTextShape::Storage &slot) {
            const auto width = Magnitude(slot.metrics.advance.x);
            const auto height = static_cast<std::int64_t>(slot.metrics.ascent) + slot.metrics.descent + slot.metrics.lineGap;
            const auto verticalAdvance = Magnitude(slot.metrics.advance.y);
            const auto resolvedHeight = std::max(height, verticalAdvance);
            if (width > std::numeric_limits<std::int32_t>::max() || resolvedHeight > std::numeric_limits<std::int32_t>::max())
                return Failure(UiErrors::TextShapeInvalid);
            slot.metrics.bounds = UiLogicalExtent{static_cast<std::int32_t>(width), static_cast<std::int32_t>(resolvedHeight)};
            return slot.metrics.IsValid() ? Result<void>::Success() : Failure(UiErrors::TextShapeInvalid);
        }

        /** @brief Resolves the aggregate script and direction evidence retained by one shape. */
        void ResolveShapeEvidence(UiTextShape::Storage &slot, const UiTextShapingRequest &request) const noexcept {
            if (sourceClusters.empty()) {
                slot.descriptor.resolvedScript = request.script;
                slot.descriptor.resolvedDirection = request.direction;
                return;
            }
            UiTextScript resolvedScript = sourceClusters.front().script;
            UiTextDirection resolvedDirection = sourceClusters.front().direction;
            for (const auto &cluster : sourceClusters) {
                if (cluster.script != resolvedScript)
                    resolvedScript = UiTextScript::Auto();
                if (cluster.direction != resolvedDirection)
                    resolvedDirection = UiTextDirection::Auto;
            }
            slot.descriptor.resolvedScript = resolvedScript;
            slot.descriptor.resolvedDirection = resolvedDirection;
        }

        /** @brief Validates lifecycle, request, and non-wrapping revision admission. */
        Result<void> ValidateAdmission(const UiTextShapingRequest &request) const {
            if (lifecycle != UiTextShaperState::Active)
                return Failure(UiErrors::TextLifecycleUnavailable);
            if (!request.IsValid(descriptor.limits))
                return Failure(UiErrors::TextInputInvalid);
            if (nextRevision == 0)
                return Failure(UiErrors::GenerationExhausted);
            return Result<void>::Success();
        }

        /** @brief Builds one immutable result slot from the validated request. */
        Result<void> Build(UiTextShape::Storage &slot, const UiTextShapingRequest &request, const UiTextShapeDescriptor &shapeDescriptor) {
            slot.text.assign(request.text.data(), request.text.size());
            slot.features.assign(request.features.begin(), request.features.end());
            slot.descriptor = shapeDescriptor;
            slot.metrics = {};
            slot.runs.clear();
            slot.glyphs.clear();
            slot.clusters.clear();
            if (const auto decoded = DecodeText(request.text, scalars); decoded.HasError())
                return decoded;
            if (const auto segmented = SegmentText(scalars, sourceClusters); segmented.HasError())
                return segmented;
            if (const auto features = PrepareFeatures(slot.features); features.HasError())
                return features;
            if (const auto resolved = ResolveClusters(request); resolved.HasError())
                return resolved;
            if (const auto groups = AppendGroups(slot, request); groups.HasError())
                return groups;
            return FinalizeMetrics(slot);
        }

        /** @brief Reports whether every immutable result slot has retired. */
        bool IsDrained() const noexcept {
            return std::all_of(slots.begin(), slots.end(), [](const auto &slot) {
                return slot->leases.load() == 0;
            });
        }
    };

    /** @copydoc UiTextShaper::Create */
    Result<UiTextShaper> UiTextShaper::Create(const UiTextShaperDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return Failure<UiTextShaper>(UiErrors::TextFallbackInvalid);
        try {
            auto storage = std::make_unique<Storage>(descriptor);
            if (!storage->Initialize())
                return Failure<UiTextShaper>(UiErrors::TextShapeInvalid);

            std::vector<HbFontOwner> ownedFonts;
            ownedFonts.reserve(descriptor.fonts.Faces().size());
            for (const auto &face : descriptor.fonts.Faces()) {
                auto font = HbFontOwner{hb_font_create(face.storage_->face)};
                if (!font)
                    return Failure<UiTextShaper>(UiErrors::TextShapeInvalid);
                hb_ot_font_set_funcs(font.get());
                ownedFonts.push_back(std::move(font));
            }
            std::vector<hb_font_t *> rawFonts;
            rawFonts.reserve(ownedFonts.size());
            for (const auto &font : ownedFonts)
                rawFonts.push_back(font.get());
            storage->fonts = std::move(rawFonts);
            for (auto &font : ownedFonts)
                static_cast<void>(font.release());
            return Result<UiTextShaper>::Success(UiTextShaper{std::move(storage)});
        } catch (const std::bad_alloc &) {
            return Failure<UiTextShaper>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiTextShaper::UiTextShaper(std::unique_ptr<Storage>) */
    UiTextShaper::UiTextShaper(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiTextShaper::~UiTextShaper */
    UiTextShaper::~UiTextShaper() {
        Shutdown();
    }

    /** @copydoc UiTextShaper::UiTextShaper(UiTextShaper&&) */
    UiTextShaper::UiTextShaper(UiTextShaper &&other) noexcept = default;

    /** @copydoc UiTextShaper::operator=(UiTextShaper&&) */
    UiTextShaper &UiTextShaper::operator=(UiTextShaper &&other) noexcept = default;

    /** @copydoc UiTextShaper::Shape */
    Result<UiTextShape> UiTextShaper::Shape(const UiTextShapingRequest &request) {
        if (!storage_)
            return Failure<UiTextShape>(UiErrors::TextLifecycleUnavailable);
        if (const auto admitted = storage_->ValidateAdmission(request); admitted.HasError())
            return Result<UiTextShape>::Failure(admitted.ErrorValue());
        auto slot = storage_->TryAcquire();
        if (!slot)
            return Failure<UiTextShape>(UiErrors::TextShapeStorageExhausted);

        struct PublishLease final {
            std::shared_ptr<UiTextShape::Storage> slot;
            bool committed{};

            ~PublishLease() {
                if (!committed)
                    slot->leases.fetch_sub(1);
            }
        } publishLease{slot};

        try {
            const auto shapeRevision = UiTextShapeRevision::Create(storage_->nextRevision);
            if (shapeRevision.HasError())
                return Result<UiTextShape>::Failure(shapeRevision.ErrorValue());
            UiTextShapeDescriptor descriptor{storage_->descriptor.ownership,
                                             request.content,
                                             storage_->descriptor.font,
                                             shapeRevision.Value(),
                                             request.script,
                                             request.script,
                                             request.language,
                                             request.direction,
                                             request.direction,
                                             request.fontSize};
            const auto built = storage_->Build(*slot, request, descriptor);
            if (built.HasError())
                return Result<UiTextShape>::Failure(built.ErrorValue());
            storage_->ResolveShapeEvidence(*slot, request);
            if (!slot->descriptor.IsValid())
                return Failure<UiTextShape>(UiErrors::TextShapeInvalid);
            publishLease.committed = true;
            if (storage_->nextRevision == std::numeric_limits<std::uint64_t>::max())
                storage_->nextRevision = 0;
            else
                ++storage_->nextRevision;
            return Result<UiTextShape>::Success(UiTextShape{std::move(slot)});
        } catch (const std::bad_alloc &) {
            return Failure<UiTextShape>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiTextShaper::Close */
    void UiTextShaper::Close() noexcept {
        if (storage_)
            storage_->lifecycle = UiTextShaperState::Closed;
    }

    /** @copydoc UiTextShaper::Shutdown */
    void UiTextShaper::Shutdown() noexcept {
        if (!storage_)
            return;
        storage_->lifecycle = UiTextShaperState::Closed;
        storage_->scalars.clear();
        storage_->sourceClusters.clear();
        storage_->hbFeatures.clear();
        storage_->ShutdownNative();
    }

    /** @copydoc UiTextShaper::IsDrained */
    bool UiTextShaper::IsDrained() const noexcept {
        return !storage_ || storage_->IsDrained();
    }

    /** @copydoc UiTextShaper::State */
    UiTextShaperState UiTextShaper::State() const noexcept {
        return storage_ ? storage_->lifecycle : UiTextShaperState::Closed;
    }
}  // namespace Horo::Runtime::Ui
