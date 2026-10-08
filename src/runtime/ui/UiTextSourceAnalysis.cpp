#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiTextUnicode.h"
#include "UiTextShapingInternal.h"

#include <algorithm>

namespace Horo::Runtime::Ui::TextShapingDetail {
    namespace {
        struct FaceSelection final {
            std::size_t index{};
            bool missing{};
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
        Result<FaceSelection> SelectClusterFace(const std::span<const UiFontFace> faces, const std::span<hb_font_t *const> fonts,
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

    /** @copydoc ResolveSourceClusters */
    Result<void> ResolveSourceClusters(const UiTextShapingRequest &request, const SourceAnalysisContext &context,
                                       const std::span<SourceCluster> clusters) {
        auto *unicode = hb_unicode_funcs_get_default();
        const auto faces = context.faces;
        for (auto &cluster : clusters) {
            const auto firstScalar = context.scalars[cluster.scalarStart].value;
            cluster.hardBreak =
                request.unicode != nullptr && (firstScalar == 0x0A || firstScalar == 0x0D || firstScalar == 0x85 || firstScalar == 0x2028 ||
                                               firstScalar == 0x2029 || firstScalar == 0x0B || firstScalar == 0x0C);
            const auto hbScript = DetectClusterScript(unicode, request.script, context.scalars, cluster);
            const auto script = FromHbScript(hbScript);
            if (script.HasError())
                return Result<void>::Failure(script.ErrorValue());
            cluster.script = script.Value();
            cluster.direction = ResolveClusterDirection(request, hbScript);
            if (request.unicode != nullptr) {
                const auto evidence = request.unicode->Scalars();
                const auto scalar = std::ranges::lower_bound(evidence, cluster.byteStart, {}, &UiTextUnicodeScalar::byteStart);
                if (scalar == evidence.end() || scalar->byteStart != cluster.byteStart)
                    return Failure(UiErrors::TextInputInvalid);
                cluster.bidiLevel = scalar->level;
                cluster.direction = (cluster.bidiLevel % 2U) != 0 ? UiTextDirection::RightToLeft : UiTextDirection::LeftToRight;
            }
            if (cluster.hardBreak) {
                cluster.faceIndex = 0;
                cluster.missing = false;
                continue;
            }
            const auto selection = SelectClusterFace(faces, context.fonts, context.scalars, cluster, context.missingPolicy);
            if (selection.HasError())
                return Result<void>::Failure(selection.ErrorValue());
            cluster.faceIndex = selection.Value().index;
            cluster.missing = selection.Value().missing;
        }
        return Result<void>::Success();
    }

}  // namespace Horo::Runtime::Ui::TextShapingDetail
