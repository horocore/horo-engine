#include "editor/UiCanvasDocumentSerialization.h"

#include <array>
#include <limits>
#include <nlohmann/json.hpp>
#include <utility>

namespace Horo::Editor::UiCanvasDocumentSerialization {
    namespace {
        using Json = nlohmann::json;
        constexpr std::string_view DocumentFormat = "horo.ui_canvas";

        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            return Result<T>::Failure(MakeError(descriptor, std::move(message)));
        }

        [[nodiscard]] std::string EncodeUiId(const Runtime::Ui::SerializedUiId &bytes) {
            constexpr std::array<char, 16> Hex{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
            std::string result;
            result.reserve(bytes.size() * 2U);
            for (const std::uint8_t byte : bytes) {
                result.push_back(Hex[byte >> 4U]);
                result.push_back(Hex[byte & 0x0fU]);
            }
            return result;
        }

        [[nodiscard]] std::optional<Runtime::Ui::SerializedUiId> DecodeUiId(const std::string_view text) {
            if (text.size() != 32U)
                return std::nullopt;
            Runtime::Ui::SerializedUiId bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index) {
                const auto nibble = [](const char value) -> std::optional<std::uint8_t> {
                    if (value >= '0' && value <= '9')
                        return static_cast<std::uint8_t>(value - '0');
                    if (value >= 'a' && value <= 'f')
                        return static_cast<std::uint8_t>(value - 'a' + 10);
                    if (value >= 'A' && value <= 'F')
                        return static_cast<std::uint8_t>(value - 'A' + 10);
                    return std::nullopt;
                };
                const auto high = nibble(text[index * 2U]);
                const auto low = nibble(text[index * 2U + 1U]);
                if (!high.has_value() || !low.has_value())
                    return std::nullopt;
                bytes[index] = static_cast<std::uint8_t>((*high << 4U) | *low);
            }
            return bytes;
        }

        template <typename Id> [[nodiscard]] Result<Id> ParseUiId(const Json &value, const std::string_view field) {
            if (!value.is_string())
                return Failure<Id>(UiCanvasDocumentErrors::Malformed, "UI Canvas identity field is not text: " + std::string(field));
            const auto bytes = DecodeUiId(value.get<std::string>());
            if (!bytes.has_value())
                return Failure<Id>(UiCanvasDocumentErrors::Malformed, "UI Canvas identity field is not canonical: " + std::string(field));
            const Result<Id> parsed = Id::Create(*bytes);
            if (parsed.HasError())
                return Failure<Id>(UiCanvasDocumentErrors::Malformed, "UI Canvas identity field is zero: " + std::string(field));
            return parsed;
        }

        [[nodiscard]] Result<std::uint32_t> ParseUInt32(const Json &value, const std::string_view field) {
            if (!value.is_number_unsigned() || value.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
                return Failure<std::uint32_t>(UiCanvasDocumentErrors::Malformed,
                                              "UI Canvas numeric field is invalid: " + std::string(field));
            return Result<std::uint32_t>::Success(value.get<std::uint32_t>());
        }

        [[nodiscard]] Result<Runtime::Ui::UiRenderMode> ParseRenderMode(const Json &value) {
            if (!value.is_string())
                return Failure<Runtime::Ui::UiRenderMode>(UiCanvasDocumentErrors::Malformed, "UI Canvas render mode is invalid.");
            const std::string mode = value.get<std::string>();
            using enum Runtime::Ui::UiRenderMode;
            if (mode == "screen_space_overlay")
                return Result<Runtime::Ui::UiRenderMode>::Success(ScreenSpaceOverlay);
            if (mode == "screen_space_camera")
                return Result<Runtime::Ui::UiRenderMode>::Success(ScreenSpaceCamera);
            if (mode == "world_space")
                return Result<Runtime::Ui::UiRenderMode>::Success(WorldSpace);
            return Failure<Runtime::Ui::UiRenderMode>(UiCanvasDocumentErrors::Malformed, "UI Canvas render mode is unsupported.");
        }

        [[nodiscard]] Result<Runtime::Ui::UiScaleMode> ParseScaleMode(const Json &value) {
            if (!value.is_string())
                return Failure<Runtime::Ui::UiScaleMode>(UiCanvasDocumentErrors::Malformed, "UI Canvas scale mode is invalid.");
            const std::string mode = value.get<std::string>();
            using enum Runtime::Ui::UiScaleMode;
            if (mode == "scale_with_screen_size")
                return Result<Runtime::Ui::UiScaleMode>::Success(ScaleWithScreenSize);
            if (mode == "constant_pixel_size")
                return Result<Runtime::Ui::UiScaleMode>::Success(ConstantPixelSize);
            if (mode == "constant_physical_size")
                return Result<Runtime::Ui::UiScaleMode>::Success(ConstantPhysicalSize);
            return Failure<Runtime::Ui::UiScaleMode>(UiCanvasDocumentErrors::Malformed, "UI Canvas scale mode is unsupported.");
        }

        [[nodiscard]] Result<Runtime::Ui::UiSafeAreaMode> ParseSafeAreaMode(const Json &value) {
            if (!value.is_string())
                return Failure<Runtime::Ui::UiSafeAreaMode>(UiCanvasDocumentErrors::Malformed, "UI Canvas safe-area mode is invalid.");
            const std::string mode = value.get<std::string>();
            using enum Runtime::Ui::UiSafeAreaMode;
            if (mode == "ignore")
                return Result<Runtime::Ui::UiSafeAreaMode>::Success(Ignore);
            if (mode == "inset")
                return Result<Runtime::Ui::UiSafeAreaMode>::Success(Inset);
            return Failure<Runtime::Ui::UiSafeAreaMode>(UiCanvasDocumentErrors::Malformed, "UI Canvas safe-area mode is unsupported.");
        }

        [[nodiscard]] Result<Runtime::Ui::UiPixelSnapMode> ParsePixelSnapMode(const Json &value) {
            if (!value.is_string())
                return Failure<Runtime::Ui::UiPixelSnapMode>(UiCanvasDocumentErrors::Malformed, "UI Canvas pixel-snap mode is invalid.");
            const std::string mode = value.get<std::string>();
            using enum Runtime::Ui::UiPixelSnapMode;
            if (mode == "disabled")
                return Result<Runtime::Ui::UiPixelSnapMode>::Success(Disabled);
            if (mode == "edges")
                return Result<Runtime::Ui::UiPixelSnapMode>::Success(Edges);
            return Failure<Runtime::Ui::UiPixelSnapMode>(UiCanvasDocumentErrors::Malformed, "UI Canvas pixel-snap mode is unsupported.");
        }

        [[nodiscard]] Result<Runtime::Ui::UiCanvasScaleFactor> ParseScaleFactor(const Json &value, const std::string_view field) {
            if (!value.is_object() || !value.contains("numerator") || !value.contains("denominator"))
                return Failure<Runtime::Ui::UiCanvasScaleFactor>(UiCanvasDocumentErrors::Malformed,
                                                                 "UI Canvas scale factor is incomplete: " + std::string(field));
            const Result<std::uint32_t> numerator = ParseUInt32(value["numerator"], std::string(field) + ".numerator");
            const Result<std::uint32_t> denominator = ParseUInt32(value["denominator"], std::string(field) + ".denominator");
            if (numerator.HasError() || denominator.HasError())
                return Failure<Runtime::Ui::UiCanvasScaleFactor>(UiCanvasDocumentErrors::Malformed,
                                                                 "UI Canvas scale factor is invalid: " + std::string(field));
            const Runtime::Ui::UiCanvasScaleFactor factor{numerator.Value(), denominator.Value()};
            if (!factor.IsValid())
                return Failure<Runtime::Ui::UiCanvasScaleFactor>(UiCanvasDocumentErrors::Malformed,
                                                                 "UI Canvas scale factor is outside the supported bound: " +
                                                                     std::string(field));
            return Result<Runtime::Ui::UiCanvasScaleFactor>::Success(factor);
        }

        [[nodiscard]] Result<Runtime::Ui::UiCanvasPresentationPolicy> ParsePresentation(const Json &value) {
            if (!value.is_object() || !value.contains("safeArea") || !value.contains("uiScale") || !value.contains("fontScale") ||
                !value.contains("pixelSnap"))
                return Failure<Runtime::Ui::UiCanvasPresentationPolicy>(UiCanvasDocumentErrors::Malformed,
                                                                        "UI Canvas presentation policy is incomplete.");
            const Result<Runtime::Ui::UiSafeAreaMode> safeArea = ParseSafeAreaMode(value["safeArea"]);
            const Result<Runtime::Ui::UiCanvasScaleFactor> uiScale = ParseScaleFactor(value["uiScale"], "presentation.uiScale");
            const Result<Runtime::Ui::UiCanvasScaleFactor> fontScale = ParseScaleFactor(value["fontScale"], "presentation.fontScale");
            const Result<Runtime::Ui::UiPixelSnapMode> pixelSnap = ParsePixelSnapMode(value["pixelSnap"]);
            if (safeArea.HasError() || uiScale.HasError() || fontScale.HasError() || pixelSnap.HasError())
                return Failure<Runtime::Ui::UiCanvasPresentationPolicy>(UiCanvasDocumentErrors::Malformed,
                                                                        "UI Canvas presentation policy is invalid.");
            const Runtime::Ui::UiCanvasPresentationPolicy presentation{safeArea.Value(), uiScale.Value(), fontScale.Value(),
                                                                       pixelSnap.Value()};
            if (!presentation.IsValid())
                return Failure<Runtime::Ui::UiCanvasPresentationPolicy>(UiCanvasDocumentErrors::Malformed,
                                                                        "UI Canvas presentation policy is invalid.");
            return Result<Runtime::Ui::UiCanvasPresentationPolicy>::Success(presentation);
        }

        [[nodiscard]] std::string RenderModeText(const Runtime::Ui::UiRenderMode mode) {
            using enum Runtime::Ui::UiRenderMode;
            switch (mode) {
                case ScreenSpaceOverlay:
                    return "screen_space_overlay";
                case ScreenSpaceCamera:
                    return "screen_space_camera";
                case WorldSpace:
                    return "world_space";
            }
            return {};
        }

        [[nodiscard]] std::string ScaleModeText(const Runtime::Ui::UiScaleMode mode) {
            using enum Runtime::Ui::UiScaleMode;
            switch (mode) {
                case ScaleWithScreenSize:
                    return "scale_with_screen_size";
                case ConstantPixelSize:
                    return "constant_pixel_size";
                case ConstantPhysicalSize:
                    return "constant_physical_size";
            }
            return {};
        }

        [[nodiscard]] std::string SafeAreaModeText(const Runtime::Ui::UiSafeAreaMode mode) {
            using enum Runtime::Ui::UiSafeAreaMode;
            switch (mode) {
                case Ignore:
                    return "ignore";
                case Inset:
                    return "inset";
            }
            return {};
        }

        [[nodiscard]] std::string PixelSnapModeText(const Runtime::Ui::UiPixelSnapMode mode) {
            using enum Runtime::Ui::UiPixelSnapMode;
            switch (mode) {
                case Disabled:
                    return "disabled";
                case Edges:
                    return "edges";
            }
            return {};
        }

        [[nodiscard]] Json EncodeScaleFactor(const Runtime::Ui::UiCanvasScaleFactor &factor) {
            return Json{{"numerator", factor.numerator}, {"denominator", factor.denominator}};
        }

        [[nodiscard]] Json EncodePresentation(const Runtime::Ui::UiCanvasPresentationPolicy &presentation) {
            return Json{{"safeArea", SafeAreaModeText(presentation.safeArea)},
                        {"uiScale", EncodeScaleFactor(presentation.uiScale)},
                        {"fontScale", EncodeScaleFactor(presentation.fontScale)},
                        {"pixelSnap", PixelSnapModeText(presentation.pixelSnap)}};
        }

        /** @brief Validates the required top-level UI Canvas fields and format version. */
        [[nodiscard]] Result<void> ValidateDocumentEnvelope(const Json &root) {
            if (!root.is_object() || root.value("format", "") != DocumentFormat)
                return Failure(UiCanvasDocumentErrors::Malformed, "UI Canvas document format is invalid.");
            if (!root.contains("formatVersion") || !root["formatVersion"].is_number_unsigned())
                return Failure(UiCanvasDocumentErrors::Malformed, "UI Canvas format version is missing.");
            if (root["formatVersion"].get<std::uint64_t>() != UiCanvasDocumentFormatVersion)
                return Failure(UiCanvasDocumentErrors::UnsupportedVersion, "UI Canvas format version is not supported by this editor.");
            if (!root.contains("documentId") || !root.contains("revision") || !root.contains("canvases") ||
                !root.contains("dependencies") || !root["canvases"].is_array() || !root["dependencies"].is_array())
                return Failure(UiCanvasDocumentErrors::Malformed, "UI Canvas document fields are incomplete.");
            return Result<void>::Success();
        }

        struct ParsedDocumentIdentity final {
            Runtime::Ui::UiDocumentId id;
            Runtime::Ui::UiDocumentRevision revision;
        };

        /** @brief Parses the stable authored identity and non-zero revision. */
        [[nodiscard]] Result<ParsedDocumentIdentity> ParseDocumentIdentity(const Json &root) {
            const Result<Runtime::Ui::UiDocumentId> documentId = ParseUiId<Runtime::Ui::UiDocumentId>(root["documentId"], "documentId");
            if (!root["revision"].is_number_unsigned() || documentId.HasError())
                return Failure<ParsedDocumentIdentity>(UiCanvasDocumentErrors::Malformed, "UI Canvas document identity is invalid.");
            const Result<Runtime::Ui::UiDocumentRevision> revision =
                Runtime::Ui::UiDocumentRevision::Create(root["revision"].get<std::uint64_t>());
            if (revision.HasError())
                return Failure<ParsedDocumentIdentity>(UiCanvasDocumentErrors::Malformed, "UI Canvas document revision is invalid.");
            return Result<ParsedDocumentIdentity>::Success({.id = documentId.Value(), .revision = revision.Value()});
        }

        /** @brief Parses bounded canvas descriptors into the authored document builder. */
        [[nodiscard]] Result<void> ParseCanvasList(const Json &values, Runtime::Ui::UiDocumentBuilder &builder) {
            if (values.empty() || values.size() > Runtime::Ui::MaximumUiDocumentCanvases)
                return Failure(UiCanvasDocumentErrors::InvalidDocument, "UI Canvas count is outside the supported bound.");
            for (const Json &canvas : values) {
                if (!canvas.is_object() || !canvas.contains("id") || !canvas.contains("rootElement") || !canvas.contains("renderMode") ||
                    !canvas.contains("referenceResolution") || !canvas.contains("scaleMode") || !canvas["referenceResolution"].is_object())
                    return Failure(UiCanvasDocumentErrors::Malformed, "UI Canvas descriptor is incomplete.");
                const Result<Runtime::Ui::UiCanvasId> id = ParseUiId<Runtime::Ui::UiCanvasId>(canvas["id"], "canvas.id");
                const Result<Runtime::Ui::UiElementId> rootElement =
                    ParseUiId<Runtime::Ui::UiElementId>(canvas["rootElement"], "canvas.rootElement");
                const Result<Runtime::Ui::UiRenderMode> renderMode = ParseRenderMode(canvas["renderMode"]);
                const Result<Runtime::Ui::UiScaleMode> scaleMode = ParseScaleMode(canvas["scaleMode"]);
                Result<Runtime::Ui::UiCanvasPresentationPolicy> presentation = Result<Runtime::Ui::UiCanvasPresentationPolicy>::Success({});
                if (canvas.contains("presentation"))
                    presentation = ParsePresentation(canvas["presentation"]);
                const Json &resolution = canvas["referenceResolution"];
                if (id.HasError() || rootElement.HasError() || renderMode.HasError() || scaleMode.HasError() || presentation.HasError() ||
                    !resolution.contains("width") || !resolution.contains("height"))
                    return Failure(UiCanvasDocumentErrors::Malformed, "UI Canvas descriptor identity or mode is invalid.");
                const Result<std::uint32_t> width = ParseUInt32(resolution["width"], "canvas.referenceResolution.width");
                const Result<std::uint32_t> height = ParseUInt32(resolution["height"], "canvas.referenceResolution.height");
                if (width.HasError() || height.HasError())
                    return Failure(UiCanvasDocumentErrors::Malformed, "UI Canvas reference resolution is invalid.");
                if (const Result<void> added = builder.AddCanvas(Runtime::Ui::UiCanvasDescriptor{
                        .id = id.Value(),
                        .rootElement = rootElement.Value(),
                        .renderMode = renderMode.Value(),
                        .referenceResolution = {.width = width.Value(), .height = height.Value()},
                        .scaleMode = scaleMode.Value(),
                        .presentation = presentation.Value(),
                    });
                    added.HasError())
                    return Failure(UiCanvasDocumentErrors::InvalidDocument, added.ErrorValue().message);
            }
            return Result<void>::Success();
        }

        /** @brief Parses bounded asset dependencies into the authored document builder. */
        [[nodiscard]] Result<void> ParseDependencyList(const Json &values, Runtime::Ui::UiDocumentBuilder &builder) {
            if (values.size() > Runtime::Ui::MaximumUiDocumentDependencies)
                return Failure(UiCanvasDocumentErrors::InvalidDocument, "UI Canvas dependency count exceeds the supported bound.");
            for (const Json &dependency : values) {
                if (!dependency.is_object() || !dependency.contains("asset") || !dependency.contains("expectedType") ||
                    !dependency.contains("required") || !dependency["asset"].is_string() || !dependency["expectedType"].is_string() ||
                    !dependency["required"].is_boolean())
                    return Failure(UiCanvasDocumentErrors::Malformed, "UI Canvas dependency is incomplete.");
                const Result<Assets::AssetId> asset = Assets::AssetId::Parse(dependency["asset"].get<std::string>());
                const Result<Assets::AssetTypeId> expectedType = Assets::AssetTypeId::Parse(dependency["expectedType"].get<std::string>());
                if (asset.HasError() || expectedType.HasError())
                    return Failure(UiCanvasDocumentErrors::Malformed, "UI Canvas dependency identity is invalid.");
                if (const Result<void> required = builder.RequireAsset(Runtime::Ui::UiAssetDependency{
                        .asset = asset.Value(),
                        .expectedType = expectedType.Value(),
                        .required = dependency["required"].get<bool>(),
                    });
                    required.HasError())
                    return Failure(UiCanvasDocumentErrors::InvalidDocument, required.ErrorValue().message);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<Runtime::Ui::UiDocument> ParseDocument(const Json &root) {
            if (const Result<void> envelope = ValidateDocumentEnvelope(root); envelope.HasError())
                return Result<Runtime::Ui::UiDocument>::Failure(envelope.ErrorValue());
            const Result<ParsedDocumentIdentity> identity = ParseDocumentIdentity(root);
            if (identity.HasError())
                return Result<Runtime::Ui::UiDocument>::Failure(identity.ErrorValue());

            Runtime::Ui::UiDocumentBuilder builder{identity.Value().id, identity.Value().revision};
            if (const Result<void> canvases = ParseCanvasList(root["canvases"], builder); canvases.HasError())
                return Result<Runtime::Ui::UiDocument>::Failure(canvases.ErrorValue());
            if (const Result<void> dependencies = ParseDependencyList(root["dependencies"], builder); dependencies.HasError())
                return Result<Runtime::Ui::UiDocument>::Failure(dependencies.ErrorValue());

            const Result<Runtime::Ui::UiDocument> built = std::move(builder).Build();
            if (built.HasError())
                return Failure<Runtime::Ui::UiDocument>(UiCanvasDocumentErrors::InvalidDocument, built.ErrorValue().message);
            return built;
        }

        [[nodiscard]] Json SerializeDocument(const Runtime::Ui::UiDocument &document) {
            Json canvases = Json::array();
            for (const Runtime::Ui::UiCanvasDescriptor &canvas : document.Canvases()) {
                canvases.push_back({
                    {"id", EncodeUiId(canvas.id.Bytes())},
                    {"rootElement", EncodeUiId(canvas.rootElement.Bytes())},
                    {"renderMode", RenderModeText(canvas.renderMode)},
                    {"referenceResolution", {{"width", canvas.referenceResolution.width}, {"height", canvas.referenceResolution.height}}},
                    {"scaleMode", ScaleModeText(canvas.scaleMode)},
                    {"presentation", EncodePresentation(canvas.presentation)},
                });
            }
            Json dependencies = Json::array();
            for (const Runtime::Ui::UiAssetDependency &dependency : document.Dependencies()) {
                dependencies.push_back({
                    {"asset", dependency.asset.ToString()},
                    {"expectedType", dependency.expectedType.Value()},
                    {"required", dependency.required},
                });
            }
            return Json{
                {"format", DocumentFormat},
                {"formatVersion", UiCanvasDocumentFormatVersion},
                {"documentId", EncodeUiId(document.Id().Bytes())},
                {"revision", document.Revision().Value()},
                {"canvases", std::move(canvases)},
                {"dependencies", std::move(dependencies)},
            };
        }
    }  // namespace

    Result<Runtime::Ui::UiDocument> Parse(const std::string_view contents) {
        try {
            return ParseDocument(Json::parse(contents));
        } catch (const Json::exception &exception) {
            return Failure<Runtime::Ui::UiDocument>(UiCanvasDocumentErrors::Malformed, exception.what());
        }
    }

    std::string Serialize(const Runtime::Ui::UiDocument &document) {
        return SerializeDocument(document).dump(2) + '\n';
    }
}  // namespace Horo::Editor::UiCanvasDocumentSerialization
