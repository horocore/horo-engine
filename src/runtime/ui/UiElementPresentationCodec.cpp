#include "UiElementPresentationCodec.h"

#include "JsonUtils.h"
#include "UiDocumentSerializationInternal.h"
#include "UiElementAuthoringCodec.h"

#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <type_traits>

namespace Horo::Runtime::Ui::PresentationCodec {
    namespace {
        using Json = nlohmann::json;
        using Out = nlohmann::ordered_json;

        /** @brief Rejects missing, unknown or wrongly shaped members before typed extraction. */
        void Fields(const Json &value, const std::initializer_list<std::string_view> keys) {
            if (!Horo::Foundation::HasAllowedFields(value, keys))
                throw std::invalid_argument("presentation object shape");
        }

        /** @brief Rejects oversized and incorrectly shaped fixed arrays before indexing. */
        void Array(const Json &value, const std::size_t count) {
            if (!value.is_array() || value.size() != count)
                throw std::invalid_argument("presentation array shape");
        }

        /** @brief Reads integers without narrowing or accepting floating point coercion. */
        template <typename T> T Integer(const Json &value) {
            if (!value.is_number_integer())
                throw std::invalid_argument("presentation integer");
            if (value.is_number_unsigned()) {
                const auto number = value.get<std::uint64_t>();
                if (number > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
                    throw std::out_of_range("presentation integer");
                return static_cast<T>(number);
            }
            const auto number = value.get<std::int64_t>();
            if (number < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
                number > static_cast<std::int64_t>(std::numeric_limits<T>::max()))
                throw std::out_of_range("presentation integer");
            return static_cast<T>(number);
        }

        /** @brief Reads a known numeric representation; semantic range validation stays with the owning type. */
        float Number(const Json &value) {
            if (!value.is_number())
                throw std::invalid_argument("presentation number");
            const double number = value.get<double>();
            if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max())
                throw std::out_of_range("presentation number");
            return static_cast<float>(number);
        }

        /** @brief Reads booleans without truthy numeric conversion. */
        bool Boolean(const Json &value) {
            if (!value.is_boolean())
                throw std::invalid_argument("presentation boolean");
            return value.get<bool>();
        }

        /** @brief Decodes a stable Runtime UI identity through the existing canonical source codec. */
        template <typename T> T Identity(const Json &value) {
            auto decoded = SerializationInternal::DecodeUiId<T>(value);
            if (decoded.HasError())
                throw std::invalid_argument("presentation identity");
            return decoded.Value();
        }

        /** @brief Decodes one canonical stable Assets identity. */
        Assets::AssetId Asset(const Json &value) {
            if (!value.is_string())
                throw std::invalid_argument("presentation asset");
            auto parsed = Assets::AssetId::Parse(value.get<std::string>());
            if (parsed.HasError() || parsed.Value().ToString() != value.get<std::string>())
                throw std::invalid_argument("presentation asset");
            return parsed.Value();
        }

        /** @brief Encodes canonical source identity text or explicit absence. */
        template <typename T> Out Id(const T id) {
            return id.IsValid() ? Out(SerializationInternal::EncodeUiId(id.Bytes())) : Out(nullptr);
        }

        /** @brief Encodes the two-field canonical UiLength representation. */
        Out Length(const UiLength value) {
            return Out::array({static_cast<std::uint8_t>(value.kind), value.value});
        }

        /** @brief Decodes UiLength without interpreting its unit in the codec. */
        UiLength Length(const Json &value) {
            Array(value, 2);
            return {static_cast<UiLengthKind>(Integer<std::uint8_t>(value[0])), Integer<UiScalar>(value[1])};
        }

        /** @brief Encodes all four canonical logical edges. */
        Out Edges(const UiLayoutEdges value) {
            return Out::array({value.left, value.top, value.right, value.bottom});
        }

        /** @brief Decodes logical edges without clamping signed offsets. */
        UiLayoutEdges Edges(const Json &value) {
            Array(value, 4);
            return {Integer<UiScalar>(value[0]), Integer<UiScalar>(value[1]), Integer<UiScalar>(value[2]), Integer<UiScalar>(value[3])};
        }

        /** @brief Encodes optional anchor coordinates and their canonical alignment policy. */
        Out Axis(const UiLayoutAnchorAxis &axis) {
            return Out::array({axis.start ? Out(*axis.start) : Out(nullptr), axis.end ? Out(*axis.end) : Out(nullptr),
                               static_cast<std::uint8_t>(axis.alignment), axis.preserveSize});
        }

        /** @brief Decodes one anchor axis retaining explicit absence. */
        UiLayoutAnchorAxis Axis(const Json &value) {
            Array(value, 4);
            return {value[0].is_null() ? std::nullopt : std::optional{Integer<UiScalar>(value[0])},
                    value[1].is_null() ? std::nullopt : std::optional{Integer<UiScalar>(value[1])},
                    static_cast<UiLayoutAlignment>(Integer<std::uint8_t>(value[2])), Boolean(value[3])};
        }

        /** @brief Encodes every grid slot, preserving the complete authored typed value. */
        Out Tracks(const std::array<UiGridTrack, MaximumUiGridTracks> &tracks) {
            Out result = Out::array();
            for (const auto &track : tracks)
                result.push_back(
                    Out::array({static_cast<std::uint8_t>(track.kind), track.value, Length(track.minimum), Length(track.maximum)}));
            return result;
        }

        /** @brief Decodes exactly the bounded canonical grid slot count. */
        std::array<UiGridTrack, MaximumUiGridTracks> Tracks(const Json &value) {
            Array(value, MaximumUiGridTracks);
            std::array<UiGridTrack, MaximumUiGridTracks> result;
            for (std::size_t index = 0; index < result.size(); ++index) {
                Array(value[index], 4);
                result[index] = {static_cast<UiGridTrackKind>(Integer<std::uint8_t>(value[index][0])), Integer<UiScalar>(value[index][1]),
                                 Length(value[index][2]), Length(value[index][3])};
            }
            return result;
        }

        /** @brief Encodes the existing declarative layout contract without introducing alternate semantics. */
        Out Layout(const UiLayoutStyle &style) {
            const auto &container = style.container;
            return Out{{"lengths", Out::array({Length(style.width), Length(style.height), Length(style.minimumWidth),
                                               Length(style.minimumHeight), Length(style.maximumWidth), Length(style.maximumHeight)})},
                       {"aspect", Out::array({style.aspectRatio.width, style.aspectRatio.height})},
                       {"margin", Edges(style.margin)},
                       {"padding", Edges(style.padding)},
                       {"border", Edges(style.border)},
                       {"offsets", Edges(style.offsets)},
                       {"anchors", Out::array({Axis(style.anchors.horizontal), Axis(style.anchors.vertical)})},
                       {"pivot", Out::array({style.pivot.x, style.pivot.y})},
                       {"alignment", Out::array({static_cast<std::uint8_t>(style.horizontalAlignment),
                                                 static_cast<std::uint8_t>(style.verticalAlignment)})},
                       {"positioning", static_cast<std::uint8_t>(style.positioning)},
                       {"container", Out{{"kind", static_cast<std::uint8_t>(container.kind)},
                                         {"orientation", static_cast<std::uint8_t>(container.orientation)},
                                         {"wrap", static_cast<std::uint8_t>(container.wrap)},
                                         {"gap", container.gap},
                                         {"main", static_cast<std::uint8_t>(container.mainAlignment)},
                                         {"cross", static_cast<std::uint8_t>(container.crossAlignment)},
                                         {"columns", Tracks(container.columns)},
                                         {"rows", Tracks(container.rows)},
                                         {"columnCount", container.columnCount},
                                         {"rowCount", container.rowCount}}},
                       {"flex", Out::array({style.flex.grow, style.flex.shrink})},
                       {"grid", Out::array({style.grid.column, style.grid.row, style.grid.columnSpan, style.grid.rowSpan})}};
        }

        /** @brief Decodes the canonical direct-child layout policy independently of element geometry. */
        UiLayoutContainerStyle Container(const Json &container) {
            UiLayoutContainerStyle result;
            Fields(container, {"kind", "orientation", "wrap", "gap", "main", "cross", "columns", "rows", "columnCount", "rowCount"});
            result.kind = static_cast<UiLayoutContainerKind>(Integer<std::uint8_t>(container.at("kind")));
            result.orientation = static_cast<UiLayoutOrientation>(Integer<std::uint8_t>(container.at("orientation")));
            result.wrap = static_cast<UiLayoutWrapMode>(Integer<std::uint8_t>(container.at("wrap")));
            result.gap = Integer<UiScalar>(container.at("gap"));
            result.mainAlignment = static_cast<UiLayoutDistribution>(Integer<std::uint8_t>(container.at("main")));
            result.crossAlignment = static_cast<UiLayoutAlignment>(Integer<std::uint8_t>(container.at("cross")));
            result.columns = Tracks(container.at("columns"));
            result.rows = Tracks(container.at("rows"));
            result.columnCount = Integer<std::uint16_t>(container.at("columnCount"));
            result.rowCount = Integer<std::uint16_t>(container.at("rowCount"));
            return result;
        }

        /** @brief Decodes typed fields with exact shape and integer-width validation. */
        UiLayoutStyle Layout(const Json &value) {
            Fields(value, {"lengths", "aspect", "margin", "padding", "border", "offsets", "anchors", "pivot", "alignment", "positioning",
                           "container", "flex", "grid"});
            UiLayoutStyle style;
            const auto &lengths = value.at("lengths");
            Array(lengths, 6);
            style.width = Length(lengths[0]);
            style.height = Length(lengths[1]);
            style.minimumWidth = Length(lengths[2]);
            style.minimumHeight = Length(lengths[3]);
            style.maximumWidth = Length(lengths[4]);
            style.maximumHeight = Length(lengths[5]);
            const auto &aspect = value.at("aspect");
            Array(aspect, 2);
            style.aspectRatio = {Integer<std::uint32_t>(aspect[0]), Integer<std::uint32_t>(aspect[1])};
            style.margin = Edges(value.at("margin"));
            style.padding = Edges(value.at("padding"));
            style.border = Edges(value.at("border"));
            style.offsets = Edges(value.at("offsets"));
            const auto &anchors = value.at("anchors");
            Array(anchors, 2);
            style.anchors = {Axis(anchors[0]), Axis(anchors[1])};
            const auto &pivot = value.at("pivot");
            Array(pivot, 2);
            style.pivot = {Integer<UiScalar>(pivot[0]), Integer<UiScalar>(pivot[1])};
            const auto &alignment = value.at("alignment");
            Array(alignment, 2);
            style.horizontalAlignment = static_cast<UiLayoutAlignment>(Integer<std::uint8_t>(alignment[0]));
            style.verticalAlignment = static_cast<UiLayoutAlignment>(Integer<std::uint8_t>(alignment[1]));
            style.positioning = static_cast<UiLayoutPositioning>(Integer<std::uint8_t>(value.at("positioning")));
            style.container = Container(value.at("container"));
            const auto &flex = value.at("flex");
            Array(flex, 2);
            style.flex = {Integer<std::uint32_t>(flex[0]), Integer<std::uint32_t>(flex[1])};
            const auto &grid = value.at("grid");
            Array(grid, 4);
            style.grid = {Integer<std::uint16_t>(grid[0]), Integer<std::uint16_t>(grid[1]), Integer<std::uint16_t>(grid[2]),
                          Integer<std::uint16_t>(grid[3])};
            return style;
        }

        /** @brief Encodes a linear semantic color without renderer conversion. */
        Out Color(const UiStyleColor &color) {
            return Out::array({color.red, color.green, color.blue, color.alpha, static_cast<std::uint8_t>(color.role)});
        }

        /** @brief Decodes a linear semantic color; owning validation rejects unknown roles. */
        UiStyleColor Color(const Json &value) {
            Array(value, 5);
            return {Number(value[0]), Number(value[1]), Number(value[2]), Number(value[3]),
                    static_cast<UiStyleColorRole>(Integer<std::uint8_t>(value[4]))};
        }

        /** @brief Encodes the closed existing style value variant, without encoding editor-specific properties. */
        Out Literal(const UiStyleValue &value) {
            Out payload;
            std::visit([&](const auto &typed) {
                using T = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<T, UiStyleColor>)
                    payload = Color(typed);
                else if constexpr (std::is_same_v<T, UiStyleDimension>)
                    payload = typed.value;
                else if constexpr (std::is_same_v<T, UiStyleTypography>)
                    payload = Out::array({typed.family.ToString(), typed.weight, typed.stretch, typed.style, typed.size, typed.lineHeight,
                                          typed.letterSpacing});
                else if constexpr (std::is_same_v<T, UiStyleImage>)
                    payload =
                        Out::array({typed.asset.ToString(), static_cast<std::uint8_t>(typed.fit), typed.nineSlice, Color(typed.tint)});
                else if constexpr (std::is_same_v<T, UiStyleShape>)
                    payload = Out::array(
                        {typed.radius, typed.borderWidth, typed.outlineWidth, typed.shadowOffsetX, typed.shadowOffsetY, typed.shadowBlur});
                else if constexpr (std::is_same_v<T, UiStyleScalar> || std::is_same_v<T, UiStyleEnumValue>)
                    payload = typed.value;
                else if constexpr (std::is_same_v<T, UiStyleMotionReference>)
                    payload = Out::array({typed.durationMicroseconds, typed.easing});
            }, value);
            return Out{{"category", static_cast<std::uint8_t>(UiStyleValueCategoryOf(value))}, {"value", std::move(payload)}};
        }

        /** @brief Decodes the closed style variant with explicit width/shape checks. */
        UiStyleValue Literal(const Json &encoded) {
            Fields(encoded, {"category", "value"});
            const auto &value = encoded.at("value");
            switch (static_cast<UiStyleValueCategory>(Integer<std::uint8_t>(encoded.at("category")))) {
                case UiStyleValueCategory::Color:
                    return Color(value);
                case UiStyleValueCategory::Dimension:
                    return UiStyleDimension{Integer<UiScalar>(value)};
                case UiStyleValueCategory::Typography:
                    Array(value, 7);
                    return UiStyleTypography{Asset(value[0]),
                                             Integer<std::uint16_t>(value[1]),
                                             Integer<std::uint8_t>(value[2]),
                                             Integer<std::uint8_t>(value[3]),
                                             Integer<UiScalar>(value[4]),
                                             Integer<UiScalar>(value[5]),
                                             Integer<UiScalar>(value[6])};
                case UiStyleValueCategory::Imagery: {
                    Array(value, 4);
                    Array(value[2], 4);
                    UiStyleImage image{Asset(value[0]), static_cast<UiStyleImageFit>(Integer<std::uint8_t>(value[1]))};
                    for (std::size_t index = 0; index < 4; ++index)
                        image.nineSlice[index] = Integer<UiScalar>(value[2][index]);
                    image.tint = Color(value[3]);
                    return image;
                }
                case UiStyleValueCategory::Shape:
                    Array(value, 6);
                    return UiStyleShape{Integer<UiScalar>(value[0]), Integer<UiScalar>(value[1]), Integer<UiScalar>(value[2]),
                                        Integer<UiScalar>(value[3]), Integer<UiScalar>(value[4]), Integer<UiScalar>(value[5])};
                case UiStyleValueCategory::Scalar:
                    return UiStyleScalar{Number(value)};
                case UiStyleValueCategory::Enum:
                    return UiStyleEnumValue{Integer<std::uint32_t>(value)};
                case UiStyleValueCategory::Motion:
                    Array(value, 2);
                    return UiStyleMotionReference{Integer<std::uint32_t>(value[0]), Integer<std::uint16_t>(value[1])};
            }
            throw std::invalid_argument("presentation style category");
        }

        /** @brief Encodes an explicitly qualified style class reference. */
        Out Class(const UiStyleClassReference value) {
            return value.IsValid() ? Out::array({Id(value.asset), Id(value.id)}) : Out(nullptr);
        }

        /** @brief Decodes one qualified style class, preserving optional absence for typeClass. */
        UiStyleClassReference Class(const Json &value) {
            if (value.is_null())
                return {};
            Array(value, 2);
            return {Identity<RuntimeStyleAssetId>(value[0]), Identity<UiStyleClassId>(value[1])};
        }
    }  // namespace

    /** @copydoc Encode */
    nlohmann::ordered_json Encode(const UiElementPresentation &presentation) {
        Out classes = Out::array(), assignments = Out::array();
        for (const auto &entry : presentation.classes)
            classes.push_back(Class(entry));
        for (const auto &assignment : presentation.inlineStyle) {
            const auto &source = assignment.value;
            assignments.push_back(
                Out{{"property", Id(assignment.property)},
                    {"source", source.referencesToken ? Out{{"token", Out::array({Id(source.token.asset), Id(source.token.id)})}}
                                                      : Out{{"literal", Literal(source.literal)}}}});
        }
        Out output{{"version", presentation.UsesAuthoringExtensions() ? 2 : 1},
                   {"layout", Layout(presentation.layout)},
                   {"intrinsic", Out::array({static_cast<std::uint8_t>(presentation.intrinsic.kind), presentation.intrinsic.required,
                                             presentation.intrinsic.fallback.width, presentation.intrinsic.fallback.height})},
                   {"styleAsset", Id(presentation.styleAsset)},
                   {"typeClass", Class(presentation.typeClass)},
                   {"classes", std::move(classes)},
                   {"inlineStyle", std::move(assignments)}};
        if (presentation.UsesAuthoringExtensions())
            WriteAuthoring(output, presentation);
        return output;
    }

    /** @copydoc Decode */
    Result<UiElementPresentation> Decode(const nlohmann::json &value) {
        try {
            const auto version = Integer<std::uint32_t>(value.at("version"));
            if (version == 1)
                Fields(value, {"version", "layout", "intrinsic", "styleAsset", "typeClass", "classes", "inlineStyle"});
            else if (version == 2)
                Fields(value, {"version", "layout", "intrinsic", "styleAsset", "typeClass", "classes", "inlineStyle", "state",
                               "localizedText", "accessibility"});
            else
                throw std::invalid_argument("presentation version");
            UiElementPresentation presentation;
            presentation.layout = Layout(value.at("layout"));
            const auto &intrinsic = value.at("intrinsic");
            Array(intrinsic, 4);
            presentation.intrinsic = {static_cast<UiLayoutIntrinsicKind>(Integer<std::uint8_t>(intrinsic[0])),
                                      Boolean(intrinsic[1]),
                                      {Integer<UiScalar>(intrinsic[2]), Integer<UiScalar>(intrinsic[3])}};
            if (!value.at("styleAsset").is_null())
                presentation.styleAsset = Identity<RuntimeStyleAssetId>(value.at("styleAsset"));
            presentation.typeClass = Class(value.at("typeClass"));
            const auto &classes = value.at("classes"), &assignments = value.at("inlineStyle");
            if (!classes.is_array() || classes.size() > MaximumUiStyleClasses || !assignments.is_array() ||
                assignments.size() > MaximumUiStyleProperties)
                throw std::invalid_argument("presentation capacity");
            for (const auto &entry : classes)
                presentation.classes.push_back(Class(entry));
            for (const auto &entry : assignments) {
                Fields(entry, {"property", "source"});
                const auto &source = entry.at("source");
                UiStyleValueSource decoded;
                if (source.contains("token")) {
                    Fields(source, {"token"});
                    const auto &token = source.at("token");
                    Array(token, 2);
                    decoded = UiStyleValueSource::Token({Identity<RuntimeStyleAssetId>(token[0]), Identity<UiStyleTokenId>(token[1])});
                } else {
                    Fields(source, {"literal"});
                    decoded = UiStyleValueSource::Literal(Literal(source.at("literal")));
                }
                presentation.inlineStyle.push_back({Identity<UiStylePropertyId>(entry.at("property")), std::move(decoded), false});
            }
            if (version == 2) {
                auto authored = ReadAuthoring(value, presentation);
                if (authored.HasError())
                    return Result<UiElementPresentation>::Failure(authored.ErrorValue());
            }
            if (!presentation.IsValid())
                throw std::invalid_argument("presentation validation");
            return Result<UiElementPresentation>::Success(std::move(presentation));
        } catch (const Json::exception &) {
            return Result<UiElementPresentation>::Failure(MakeError(UiErrors::DocumentSerializationInvalid));
        } catch (const std::invalid_argument &) {
            return Result<UiElementPresentation>::Failure(MakeError(UiErrors::DocumentSerializationInvalid));
        } catch (const std::out_of_range &) {
            return Result<UiElementPresentation>::Failure(MakeError(UiErrors::DocumentSerializationInvalid));
        }
    }

    /** @copydoc Write */
    Result<std::string> Write(const UiElementPresentation &presentation) {
        if (!presentation.IsValid())
            return Result<std::string>::Failure(MakeError(UiErrors::DocumentSerializationInvalid));
        auto encoded = Encode(presentation).dump();
        if (encoded.size() > MaximumUiElementPresentationBytes)
            return Result<std::string>::Failure(MakeError(UiErrors::DocumentPayloadTooLarge));
        return Result<std::string>::Success(std::move(encoded));
    }

    /** @copydoc Read */
    Result<UiElementPresentation> Read(const std::string_view source) {
        if (source.size() > MaximumUiElementPresentationBytes)
            return Result<UiElementPresentation>::Failure(MakeError(UiErrors::DocumentPayloadTooLarge));
        auto parsed = SerializationInternal::ParseJson(source, {});
        if (parsed.HasError())
            return Result<UiElementPresentation>::Failure(parsed.ErrorValue());
        return Decode(parsed.Value());
    }
}  // namespace Horo::Runtime::Ui::PresentationCodec

namespace Horo::Runtime::Ui {
    /** @copydoc SerializeUiElementPresentation */
    Result<std::string> SerializeUiElementPresentation(const UiElementPresentation &presentation) {
        return PresentationCodec::Write(presentation);
    }

    /** @copydoc DeserializeUiElementPresentation */
    Result<UiElementPresentation> DeserializeUiElementPresentation(const std::string_view source) {
        return PresentationCodec::Read(source);
    }
}  // namespace Horo::Runtime::Ui
