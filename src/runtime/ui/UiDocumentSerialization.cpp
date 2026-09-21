#include "Horo/Foundation/Utf8.h"
#include "JsonUtils.h"
#include "UiDocumentSerializationInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <nlohmann/json.hpp>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>

namespace Horo::Runtime::Ui::SerializationInternal {
    using Json = nlohmann::json;
    using OrderedJson = nlohmann::ordered_json;

    template <typename T> [[nodiscard]] Result<T> Failed(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
        return Result<T>::Failure(MakeError(descriptor, std::move(message)));
    }

    [[nodiscard]] Result<void> Failed(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
        return Result<void>::Failure(MakeError(descriptor, std::move(message)));
    }

    [[nodiscard]] bool IsSupportedVersion(const UiDocumentSchemaVersion version) noexcept {
        return version.major != 0 && version.major == CurrentUiDocumentSchemaVersion.major &&
               version.minor <= CurrentUiDocumentSchemaVersion.minor;
    }

    [[nodiscard]] bool IsValidBand(const UiPresentationBand band) noexcept {
        return static_cast<std::uint8_t>(band) < static_cast<std::uint8_t>(UiPresentationBand::Count);
    }

    [[nodiscard]] bool IsValidRenderMode(const UiRenderMode mode) noexcept {
        return static_cast<std::uint8_t>(mode) <= static_cast<std::uint8_t>(UiRenderMode::WorldSpace);
    }

    [[nodiscard]] bool IsValidScaleMode(const UiScaleMode mode) noexcept {
        return static_cast<std::uint8_t>(mode) <= static_cast<std::uint8_t>(UiScaleMode::ConstantPhysicalSize);
    }

    [[nodiscard]] bool IsValidReferenceKind(const UiReferenceKind kind) noexcept {
        return static_cast<std::uint8_t>(kind) < static_cast<std::uint8_t>(UiReferenceKind::Count);
    }

    [[nodiscard]] bool IsCanonicalHex(const std::string_view text) noexcept {
        if (text.size() != 32)
            return false;
        return std::ranges::all_of(text, [](const char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        });
    }

    [[nodiscard]] char HexDigit(const std::uint8_t value) noexcept {
        return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + value - 10);
    }

    [[nodiscard]] std::string EncodeUiId(const SerializedUiId &bytes) {
        std::string encoded;
        encoded.reserve(32);
        for (const auto byte : bytes) {
            encoded.push_back(HexDigit(static_cast<std::uint8_t>(byte >> 4U)));
            encoded.push_back(HexDigit(static_cast<std::uint8_t>(byte & 0x0fU)));
        }
        return encoded;
    }

    template <typename Id> [[nodiscard]] Result<Id> DecodeUiId(const Json &value) {
        if (!value.is_string())
            return Failed<Id>(UiErrors::DocumentSerializationInvalid, "Runtime UI identity is not text.");
        const std::string text = value.get<std::string>();
        if (!IsCanonicalHex(text))
            return Failed<Id>(UiErrors::DocumentSerializationInvalid, "Runtime UI identity is not canonical lowercase hexadecimal.");
        SerializedUiId bytes{};
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            const auto decode = [](const char digit) -> std::uint8_t {
                if (digit >= '0' && digit <= '9')
                    return static_cast<std::uint8_t>(digit - '0');
                return static_cast<std::uint8_t>(digit - 'a' + 10);
            };
            bytes[index] = static_cast<std::uint8_t>((decode(text[index * 2]) << 4U) | decode(text[index * 2 + 1]));
        }
        auto parsed = Id::Create(bytes);
        return parsed.HasError() ? Result<Id>::Failure(parsed.ErrorValue()) : parsed;
    }

    [[nodiscard]] Result<std::string> ReadText(const Json &value, const UiDocumentSerializationLimits &limits) {
        if (!value.is_string())
            return Failed<std::string>(UiErrors::DocumentSerializationInvalid, "Runtime UI text field is not a string.");
        std::string text = value.get<std::string>();
        if (text.empty() || text.size() > limits.maximumTextBytes || !IsValidUtf8ScalarSequence(text))
            return Failed<std::string>(UiErrors::DocumentSerializationInvalid,
                                       "Runtime UI text field is empty, oversized, or invalid UTF-8.");
        return Result<std::string>::Success(std::move(text));
    }

    template <typename Integer> [[nodiscard]] Result<Integer> ReadUnsigned(const Json &value) {
        if (!value.is_number_unsigned())
            return Failed<Integer>(UiErrors::DocumentSerializationInvalid, "Runtime UI integer field is not unsigned.");
        const auto decoded = value.get<std::uint64_t>();
        if (decoded > std::numeric_limits<Integer>::max())
            return Failed<Integer>(UiErrors::DocumentSerializationInvalid, "Runtime UI integer field is out of range.");
        return Result<Integer>::Success(static_cast<Integer>(decoded));
    }

    [[nodiscard]] Result<double> ReadFiniteNumber(const Json &value) {
        if (!value.is_number())
            return Failed<double>(UiErrors::DocumentSerializationInvalid, "Runtime UI property is not numeric.");
        const double decoded = value.get<double>();
        if (!std::isfinite(decoded))
            return Failed<double>(UiErrors::DocumentSerializationInvalid, "Runtime UI property is non-finite.");
        return Result<double>::Success(decoded == 0.0 ? 0.0 : decoded);
    }

    [[nodiscard]] std::string RenderModeName(const UiRenderMode mode) {
        switch (mode) {
            case UiRenderMode::ScreenSpaceOverlay:
                return "screenOverlay";
            case UiRenderMode::ScreenSpaceCamera:
                return "screenCamera";
            case UiRenderMode::WorldSpace:
                return "world";
        }
        return {};
    }

    [[nodiscard]] Result<UiRenderMode> ParseRenderMode(const Json &value) {
        if (!value.is_string())
            return Failed<UiRenderMode>(UiErrors::DocumentSerializationInvalid, "Runtime UI render mode is not text.");
        const auto name = value.get<std::string>();
        if (name == "screenOverlay")
            return Result<UiRenderMode>::Success(UiRenderMode::ScreenSpaceOverlay);
        if (name == "screenCamera")
            return Result<UiRenderMode>::Success(UiRenderMode::ScreenSpaceCamera);
        if (name == "world")
            return Result<UiRenderMode>::Success(UiRenderMode::WorldSpace);
        return Failed<UiRenderMode>(UiErrors::DocumentSerializationInvalid, "Runtime UI render mode is unsupported.");
    }

    [[nodiscard]] std::string ScaleModeName(const UiScaleMode mode) {
        switch (mode) {
            case UiScaleMode::ScaleWithScreenSize:
                return "screenSize";
            case UiScaleMode::ConstantPixelSize:
                return "pixelSize";
            case UiScaleMode::ConstantPhysicalSize:
                return "physicalSize";
        }
        return {};
    }

    [[nodiscard]] Result<UiScaleMode> ParseScaleMode(const Json &value) {
        if (!value.is_string())
            return Failed<UiScaleMode>(UiErrors::DocumentSerializationInvalid, "Runtime UI scale mode is not text.");
        const auto name = value.get<std::string>();
        if (name == "screenSize")
            return Result<UiScaleMode>::Success(UiScaleMode::ScaleWithScreenSize);
        if (name == "pixelSize")
            return Result<UiScaleMode>::Success(UiScaleMode::ConstantPixelSize);
        if (name == "physicalSize")
            return Result<UiScaleMode>::Success(UiScaleMode::ConstantPhysicalSize);
        return Failed<UiScaleMode>(UiErrors::DocumentSerializationInvalid, "Runtime UI scale mode is unsupported.");
    }

    [[nodiscard]] const char *BandName(const UiPresentationBand band) noexcept {
        constexpr std::array names{"world", "hud", "screen", "overlay", "modal", "loading", "debug"};
        return IsValidBand(band) ? names[static_cast<std::size_t>(band)] : "";
    }

    [[nodiscard]] Result<UiPresentationBand> ParseBand(const Json &value) {
        if (!value.is_string())
            return Failed<UiPresentationBand>(UiErrors::DocumentRouteInvalid, "Runtime UI route band is not text.");
        const auto name = value.get<std::string>();
        constexpr std::array names{"world", "hud", "screen", "overlay", "modal", "loading", "debug"};
        for (std::size_t index = 0; index < names.size(); ++index)
            if (name == names[index])
                return Result<UiPresentationBand>::Success(static_cast<UiPresentationBand>(index));
        return Failed<UiPresentationBand>(UiErrors::DocumentRouteInvalid, "Runtime UI route band is unsupported.");
    }

    [[nodiscard]] const char *ReferenceKindName(const UiReferenceKind kind) noexcept {
        constexpr std::array names{"element", "canvas", "document", "asset"};
        return IsValidReferenceKind(kind) ? names[static_cast<std::size_t>(kind)] : "";
    }

    [[nodiscard]] Result<UiReferenceKind> ParseReferenceKind(const Json &value) {
        if (!value.is_string())
            return Failed<UiReferenceKind>(UiErrors::DocumentReferenceInvalid, "Runtime UI reference kind is not text.");
        const auto name = value.get<std::string>();
        constexpr std::array names{"element", "canvas", "document", "asset"};
        for (std::size_t index = 0; index < names.size(); ++index)
            if (name == names[index])
                return Result<UiReferenceKind>::Success(static_cast<UiReferenceKind>(index));
        return Failed<UiReferenceKind>(UiErrors::DocumentReferenceInvalid, "Runtime UI reference kind is unsupported.");
    }

    [[nodiscard]] OrderedJson EncodeReference(const UiReference &reference) {
        switch (reference.kind) {
            case UiReferenceKind::Element:
                return OrderedJson{{"kind", ReferenceKindName(reference.kind)}, {"id", EncodeUiId(reference.element)}};
            case UiReferenceKind::Canvas:
                return OrderedJson{{"kind", ReferenceKindName(reference.kind)}, {"id", EncodeUiId(reference.canvas)}};
            case UiReferenceKind::Document:
                return OrderedJson{{"kind", ReferenceKindName(reference.kind)}, {"id", EncodeUiId(reference.document)}};
            case UiReferenceKind::Asset:
                return OrderedJson{{"kind", ReferenceKindName(reference.kind)},
                                   {"id", reference.asset.ToString()},
                                   {"expectedType", reference.expectedAssetType.Value()}};
            case UiReferenceKind::Count:
                break;
        }
        return {};
    }

    [[nodiscard]] Result<UiReference> DecodeElementReference(const Json &value) {
        auto parsed = DecodeUiId<UiElementId>(value);
        if (parsed.HasError())
            return Result<UiReference>::Failure(parsed.ErrorValue());
        return Result<UiReference>::Success({.kind = UiReferenceKind::Element, .element = parsed.Value()});
    }

    [[nodiscard]] Result<UiReference> DecodeCanvasReference(const Json &value) {
        auto parsed = DecodeUiId<UiCanvasId>(value);
        if (parsed.HasError())
            return Result<UiReference>::Failure(parsed.ErrorValue());
        return Result<UiReference>::Success({.kind = UiReferenceKind::Canvas, .canvas = parsed.Value()});
    }

    [[nodiscard]] Result<UiReference> DecodeDocumentReference(const Json &value) {
        auto parsed = DecodeUiId<UiDocumentId>(value);
        if (parsed.HasError())
            return Result<UiReference>::Failure(parsed.ErrorValue());
        return Result<UiReference>::Success({.kind = UiReferenceKind::Document, .document = parsed.Value()});
    }

    [[nodiscard]] Result<UiReference> DecodeAssetReference(const Json &value, const UiDocumentSerializationLimits &limits) {
        auto id = ReadText(value.at("id"), limits);
        if (id.HasError())
            return Result<UiReference>::Failure(id.ErrorValue());
        auto asset = Assets::AssetId::Parse(id.Value());
        if (asset.HasError() || asset.Value().ToString() != id.Value() || !value.contains("expectedType"))
            return Failed<UiReference>(UiErrors::DocumentReferenceInvalid, "Runtime UI asset reference is not canonical.");
        auto type = ReadText(value.at("expectedType"), limits);
        if (type.HasError())
            return Result<UiReference>::Failure(type.ErrorValue());
        auto parsedType = Assets::AssetTypeId::Parse(type.Value());
        if (parsedType.HasError())
            return Result<UiReference>::Failure(parsedType.ErrorValue());
        return Result<UiReference>::Success(
            {.kind = UiReferenceKind::Asset, .asset = asset.Value(), .expectedAssetType = parsedType.Value()});
    }

    [[nodiscard]] Result<UiReference> DecodeReference(const Json &value, const UiDocumentSerializationLimits &limits) {
        if (!Horo::Foundation::HasAllowedFields(value, {"kind", "id"}, {"expectedType"}))
            return Failed<UiReference>(UiErrors::DocumentReferenceInvalid, "Runtime UI reference fields are not canonical.");
        auto kind = ParseReferenceKind(value.at("kind"));
        if (kind.HasError())
            return Result<UiReference>::Failure(kind.ErrorValue());
        switch (kind.Value()) {
            case UiReferenceKind::Element:
                return DecodeElementReference(value.at("id"));
            case UiReferenceKind::Canvas:
                return DecodeCanvasReference(value.at("id"));
            case UiReferenceKind::Document:
                return DecodeDocumentReference(value.at("id"));
            case UiReferenceKind::Asset:
                return DecodeAssetReference(value, limits);
            case UiReferenceKind::Count:
                return Failed<UiReference>(UiErrors::DocumentReferenceInvalid);
        }
        return Failed<UiReference>(UiErrors::DocumentReferenceInvalid);
    }

    [[nodiscard]] std::string_view LocalizedTextFailurePolicyName(const UiLocalizedTextFailurePolicy policy) noexcept {
        using enum UiLocalizedTextFailurePolicy;
        switch (policy) {
            case UseFallback:
                return "fallback";
            case UseSafePlaceholder:
                return "placeholder";
            case RequireTranslation:
                return "required";
        }
        return {};
    }

    [[nodiscard]] Result<UiLocalizedTextFailurePolicy> ParseLocalizedTextFailurePolicy(const Json &value) {
        if (!value.is_string())
            return Failed<UiLocalizedTextFailurePolicy>(UiErrors::LocalizedMessageInvalid);
        const auto name = value.get<std::string>();
        if (name == "fallback")
            return Result<UiLocalizedTextFailurePolicy>::Success(UiLocalizedTextFailurePolicy::UseFallback);
        if (name == "placeholder")
            return Result<UiLocalizedTextFailurePolicy>::Success(UiLocalizedTextFailurePolicy::UseSafePlaceholder);
        if (name == "required")
            return Result<UiLocalizedTextFailurePolicy>::Success(UiLocalizedTextFailurePolicy::RequireTranslation);
        return Failed<UiLocalizedTextFailurePolicy>(UiErrors::LocalizedMessageInvalid);
    }

    [[nodiscard]] std::string_view LocalizedAssetFallbackPolicyName(const UiLocalizedAssetFallbackPolicy policy) noexcept {
        using enum UiLocalizedAssetFallbackPolicy;
        switch (policy) {
            case Required:
                return "required";
            case UseNeutral:
                return "neutral";
            case Omit:
                return "omit";
        }
        return {};
    }

    [[nodiscard]] Result<UiLocalizedAssetFallbackPolicy> ParseLocalizedAssetFallbackPolicy(const Json &value) {
        if (!value.is_string())
            return Failed<UiLocalizedAssetFallbackPolicy>(UiErrors::LocalizedAssetReferenceInvalid);
        const auto name = value.get<std::string>();
        if (name == "required")
            return Result<UiLocalizedAssetFallbackPolicy>::Success(UiLocalizedAssetFallbackPolicy::Required);
        if (name == "neutral")
            return Result<UiLocalizedAssetFallbackPolicy>::Success(UiLocalizedAssetFallbackPolicy::UseNeutral);
        if (name == "omit")
            return Result<UiLocalizedAssetFallbackPolicy>::Success(UiLocalizedAssetFallbackPolicy::Omit);
        return Failed<UiLocalizedAssetFallbackPolicy>(UiErrors::LocalizedAssetReferenceInvalid);
    }

    [[nodiscard]] Result<std::string> ReadLocalizedString(const Json &value, const std::size_t maximumBytes,
                                                          const ErrorCodeDescriptor &descriptor) {
        if (!value.is_string())
            return Failed<std::string>(descriptor);
        std::string text = value.get<std::string>();
        if (text.size() > maximumBytes || !IsValidUtf8ScalarSequence(text))
            return Failed<std::string>(descriptor);
        return Result<std::string>::Success(std::move(text));
    }

    [[nodiscard]] Result<std::int64_t> ReadLocalizedInteger(const Json &value) {
        if (!value.is_number_integer())
            return Failed<std::int64_t>(UiErrors::LocalizedArgumentInvalid);
        return Result<std::int64_t>::Success(value.get<std::int64_t>());
    }

    [[nodiscard]] OrderedJson EncodeLocalizedArgumentValue(const UiLocalizedArgumentValue &value) {
        return std::visit([](const auto &typed) -> OrderedJson {
            using Value = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Value, std::int64_t>)
                return OrderedJson{{"type", "int"}, {"value", typed}};
            if constexpr (std::is_same_v<Value, double>)
                return OrderedJson{{"type", "number"}, {"value", typed == 0.0 ? 0.0 : typed}};
            if constexpr (std::is_same_v<Value, bool>)
                return OrderedJson{{"type", "bool"}, {"value", typed}};
            if constexpr (std::is_same_v<Value, UiLocalizedDateTime>)
                return OrderedJson{{"type", "dateTime"}, {"value", typed.unixMilliseconds}};
            if constexpr (std::is_same_v<Value, UiLocalizedDuration>)
                return OrderedJson{{"type", "duration"}, {"value", typed.milliseconds}};
            if constexpr (std::is_same_v<Value, UiLocalizedStableEnum>)
                return OrderedJson{{"type", "enum"}, {"value", OrderedJson{{"typeId", typed.typeId}, {"value", typed.value}}}};
            if constexpr (std::is_same_v<Value, std::string>)
                return OrderedJson{{"type", "text"}, {"value", typed}};
            if constexpr (std::is_same_v<Value, UiLocalizedShortcut>)
                return OrderedJson{{"type", "shortcut"}, {"value", OrderedJson{{"modifiers", typed.modifiers}, {"key", typed.key}}}};
        }, value);
    }

    [[nodiscard]] Result<UiLocalizedArgumentValue> DecodeLocalizedArgumentValue(const Json &value) {
        if (!Horo::Foundation::HasAllowedFields(value, {"type", "value"}))
            return Failed<UiLocalizedArgumentValue>(UiErrors::LocalizedArgumentInvalid);
        if (!value.at("type").is_string())
            return Failed<UiLocalizedArgumentValue>(UiErrors::LocalizedArgumentInvalid);
        const auto type = value.at("type").get<std::string>();
        const auto &encoded = value.at("value");
        if (type == "int") {
            auto decoded = ReadLocalizedInteger(encoded);
            return decoded.HasError() ? Result<UiLocalizedArgumentValue>::Failure(decoded.ErrorValue())
                                      : Result<UiLocalizedArgumentValue>::Success(decoded.Value());
        }
        if (type == "number") {
            auto decoded = ReadFiniteNumber(encoded);
            return decoded.HasError() ? Result<UiLocalizedArgumentValue>::Failure(decoded.ErrorValue())
                                      : Result<UiLocalizedArgumentValue>::Success(decoded.Value());
        }
        if (type == "bool") {
            if (!encoded.is_boolean())
                return Failed<UiLocalizedArgumentValue>(UiErrors::LocalizedArgumentInvalid);
            return Result<UiLocalizedArgumentValue>::Success(encoded.get<bool>());
        }
        if (type == "dateTime") {
            auto decoded = ReadLocalizedInteger(encoded);
            return decoded.HasError() ? Result<UiLocalizedArgumentValue>::Failure(decoded.ErrorValue())
                                      : Result<UiLocalizedArgumentValue>::Success(UiLocalizedDateTime{decoded.Value()});
        }
        if (type == "duration") {
            auto decoded = ReadLocalizedInteger(encoded);
            return decoded.HasError() ? Result<UiLocalizedArgumentValue>::Failure(decoded.ErrorValue())
                                      : Result<UiLocalizedArgumentValue>::Success(UiLocalizedDuration{decoded.Value()});
        }
        if (type == "enum") {
            if (!Horo::Foundation::HasAllowedFields(encoded, {"typeId", "value"}))
                return Failed<UiLocalizedArgumentValue>(UiErrors::LocalizedArgumentInvalid);
            auto typeId = ReadUnsigned<std::uint64_t>(encoded.at("typeId"));
            auto enumValue = ReadUnsigned<std::uint64_t>(encoded.at("value"));
            if (typeId.HasError())
                return Result<UiLocalizedArgumentValue>::Failure(typeId.ErrorValue());
            if (enumValue.HasError())
                return Result<UiLocalizedArgumentValue>::Failure(enumValue.ErrorValue());
            return Result<UiLocalizedArgumentValue>::Success(UiLocalizedStableEnum{typeId.Value(), enumValue.Value()});
        }
        if (type == "text") {
            auto decoded = ReadLocalizedString(encoded, MaximumUiLocalizedValueBytes, UiErrors::LocalizedArgumentInvalid);
            return decoded.HasError() ? Result<UiLocalizedArgumentValue>::Failure(decoded.ErrorValue())
                                      : Result<UiLocalizedArgumentValue>::Success(std::move(decoded).Value());
        }
        if (type == "shortcut") {
            if (!Horo::Foundation::HasAllowedFields(encoded, {"modifiers", "key"}))
                return Failed<UiLocalizedArgumentValue>(UiErrors::LocalizedArgumentInvalid);
            auto modifiers = ReadUnsigned<std::uint32_t>(encoded.at("modifiers"));
            auto key = ReadUnsigned<std::uint32_t>(encoded.at("key"));
            if (modifiers.HasError())
                return Result<UiLocalizedArgumentValue>::Failure(modifiers.ErrorValue());
            if (key.HasError())
                return Result<UiLocalizedArgumentValue>::Failure(key.ErrorValue());
            return Result<UiLocalizedArgumentValue>::Success(UiLocalizedShortcut{modifiers.Value(), key.Value()});
        }
        return Failed<UiLocalizedArgumentValue>(UiErrors::LocalizedArgumentInvalid);
    }

    [[nodiscard]] OrderedJson EncodeLocalizedText(const UiLocalizedText &text) {
        OrderedJson arguments = OrderedJson::array();
        for (const auto &argument : text.Arguments())
            arguments.push_back(OrderedJson{{"name", argument.name}, {"value", EncodeLocalizedArgumentValue(argument.value)}});
        return OrderedJson{{"namespace", text.Key().namespaceId},
                           {"localKey", text.Key().localKey},
                           {"arguments", std::move(arguments)},
                           {"fallback", text.FallbackText()},
                           {"failurePolicy", LocalizedTextFailurePolicyName(text.FailurePolicy())}};
    }

    [[nodiscard]] Result<UiLocalizedText> DecodeLocalizedText(const Json &value, const UiDocumentSerializationLimits &limits) {
        if (!Horo::Foundation::HasAllowedFields(value, {"namespace", "localKey", "arguments", "fallback", "failurePolicy"}))
            return Failed<UiLocalizedText>(UiErrors::LocalizedMessageInvalid);
        auto namespaceId = ReadLocalizedString(value.at("namespace"), MaximumUiMessageKeyBytes, UiErrors::LocalizedKeyInvalid);
        auto localKey = ReadLocalizedString(value.at("localKey"), MaximumUiMessageKeyBytes, UiErrors::LocalizedKeyInvalid);
        auto fallback = ReadLocalizedString(value.at("fallback"), MaximumUiLocalizedFallbackTextBytes, UiErrors::LocalizedMessageInvalid);
        auto policy = ParseLocalizedTextFailurePolicy(value.at("failurePolicy"));
        if (namespaceId.HasError())
            return Result<UiLocalizedText>::Failure(namespaceId.ErrorValue());
        if (localKey.HasError())
            return Result<UiLocalizedText>::Failure(localKey.ErrorValue());
        if (fallback.HasError())
            return Result<UiLocalizedText>::Failure(fallback.ErrorValue());
        if (policy.HasError())
            return Result<UiLocalizedText>::Failure(policy.ErrorValue());
        const auto &encodedArguments = value.at("arguments");
        if (!encodedArguments.is_array() || encodedArguments.size() > MaximumUiLocalizedArguments)
            return Failed<UiLocalizedText>(UiErrors::LocalizedArgumentCapacityExceeded);
        std::vector<UiLocalizedArgument> arguments;
        arguments.reserve(encodedArguments.size());
        for (const auto &encodedArgument : encodedArguments) {
            if (!Horo::Foundation::HasAllowedFields(encodedArgument, {"name", "value"}))
                return Failed<UiLocalizedText>(UiErrors::LocalizedArgumentInvalid);
            auto name =
                ReadLocalizedString(encodedArgument.at("name"), MaximumUiLocalizedArgumentNameBytes, UiErrors::LocalizedArgumentInvalid);
            auto argumentValue = DecodeLocalizedArgumentValue(encodedArgument.at("value"));
            if (name.HasError())
                return Result<UiLocalizedText>::Failure(name.ErrorValue());
            if (argumentValue.HasError())
                return Result<UiLocalizedText>::Failure(argumentValue.ErrorValue());
            arguments.push_back({std::move(name).Value(), std::move(argumentValue).Value()});
        }
        auto key = UiMessageKey::Create(namespaceId.Value(), localKey.Value());
        if (key.HasError())
            return Result<UiLocalizedText>::Failure(key.ErrorValue());
        return UiLocalizedText::Create(std::move(key).Value(), std::move(arguments), std::move(fallback).Value(), policy.Value());
    }

    [[nodiscard]] OrderedJson EncodeLocalizedAsset(const UiLocalizedAssetReference &reference) {
        OrderedJson variants = OrderedJson::array();
        for (const auto &variant : reference.Variants())
            variants.push_back(OrderedJson{{"locale", variant.locale.Value()}, {"asset", variant.asset.ToString()}});
        const auto neutral = reference.NeutralAsset();
        return OrderedJson{{"expectedType", reference.ExpectedType().Value()},
                           {"fallbackPolicy", LocalizedAssetFallbackPolicyName(reference.FallbackPolicy())},
                           {"variants", std::move(variants)},
                           {"neutralAsset", neutral.has_value() ? OrderedJson(neutral->ToString()) : OrderedJson(nullptr)}};
    }

    [[nodiscard]] Result<UiLocalizedAssetReference> DecodeLocalizedAsset(const Json &value, const UiDocumentSerializationLimits &limits) {
        if (!Horo::Foundation::HasAllowedFields(value, {"expectedType", "fallbackPolicy", "variants", "neutralAsset"}))
            return Failed<UiLocalizedAssetReference>(UiErrors::LocalizedAssetReferenceInvalid);
        auto expectedTypeText =
            ReadLocalizedString(value.at("expectedType"), limits.maximumTextBytes, UiErrors::LocalizedAssetReferenceInvalid);
        auto policy = ParseLocalizedAssetFallbackPolicy(value.at("fallbackPolicy"));
        if (expectedTypeText.HasError())
            return Result<UiLocalizedAssetReference>::Failure(expectedTypeText.ErrorValue());
        if (policy.HasError())
            return Result<UiLocalizedAssetReference>::Failure(policy.ErrorValue());
        auto expectedType = Assets::AssetTypeId::Parse(expectedTypeText.Value());
        if (expectedType.HasError())
            return Result<UiLocalizedAssetReference>::Failure(expectedType.ErrorValue());
        const auto &encodedVariants = value.at("variants");
        if (!encodedVariants.is_array() || encodedVariants.size() > MaximumUiLocalizedAssetVariants)
            return Failed<UiLocalizedAssetReference>(UiErrors::CapacityExceeded);
        std::vector<UiLocalizedAssetVariant> variants;
        variants.reserve(encodedVariants.size());
        for (const auto &encodedVariant : encodedVariants) {
            if (!Horo::Foundation::HasAllowedFields(encodedVariant, {"locale", "asset"}))
                return Failed<UiLocalizedAssetReference>(UiErrors::LocalizedAssetReferenceInvalid);
            auto localeText = ReadLocalizedString(encodedVariant.at("locale"), MaximumUiLocaleTagBytes, UiErrors::LocaleInvalid);
            auto assetText =
                ReadLocalizedString(encodedVariant.at("asset"), limits.maximumTextBytes, UiErrors::LocalizedAssetReferenceInvalid);
            if (localeText.HasError())
                return Result<UiLocalizedAssetReference>::Failure(localeText.ErrorValue());
            if (assetText.HasError())
                return Result<UiLocalizedAssetReference>::Failure(assetText.ErrorValue());
            auto locale = UiLocaleTag::Parse(localeText.Value());
            auto asset = Assets::AssetId::Parse(assetText.Value());
            if (locale.HasError())
                return Result<UiLocalizedAssetReference>::Failure(locale.ErrorValue());
            if (asset.HasError() || asset.Value().ToString() != assetText.Value())
                return Failed<UiLocalizedAssetReference>(UiErrors::LocalizedAssetReferenceInvalid);
            variants.push_back({std::move(locale).Value(), asset.Value()});
        }
        std::optional<Assets::AssetId> neutralAsset;
        const auto &encodedNeutral = value.at("neutralAsset");
        if (!encodedNeutral.is_null()) {
            auto neutralText = ReadLocalizedString(encodedNeutral, limits.maximumTextBytes, UiErrors::LocalizedAssetReferenceInvalid);
            if (neutralText.HasError())
                return Result<UiLocalizedAssetReference>::Failure(neutralText.ErrorValue());
            auto neutral = Assets::AssetId::Parse(neutralText.Value());
            if (neutral.HasError() || neutral.Value().ToString() != neutralText.Value())
                return Failed<UiLocalizedAssetReference>(UiErrors::LocalizedAssetReferenceInvalid);
            neutralAsset = neutral.Value();
        }
        return UiLocalizedAssetReference::Create(expectedType.Value(), policy.Value(), std::move(variants), neutralAsset);
    }

    [[nodiscard]] OrderedJson EncodePropertyValue(const UiPropertyValue &value) {
        return std::visit([](const auto &typed) -> OrderedJson {
            using Value = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Value, bool>)
                return OrderedJson{{"type", "bool"}, {"value", typed}};
            if constexpr (std::is_same_v<Value, std::int64_t>)
                return OrderedJson{{"type", "int"}, {"value", typed}};
            if constexpr (std::is_same_v<Value, double>)
                return OrderedJson{{"type", "number"}, {"value", typed == 0.0 ? 0.0 : typed}};
            if constexpr (std::is_same_v<Value, std::string>)
                return OrderedJson{{"type", "text"}, {"value", typed}};
            if constexpr (std::is_same_v<Value, UiReference>)
                return OrderedJson{{"type", "reference"}, {"value", EncodeReference(typed)}};
        }, value);
    }

    [[nodiscard]] Result<UiPropertyValue> DecodeBooleanProperty(const Json &value) {
        if (!value.is_boolean())
            return Failed<UiPropertyValue>(UiErrors::DocumentSerializationInvalid);
        return Result<UiPropertyValue>::Success(value.get<bool>());
    }

    [[nodiscard]] Result<UiPropertyValue> DecodeIntegerProperty(const Json &value) {
        if (!value.is_number_integer())
            return Failed<UiPropertyValue>(UiErrors::DocumentSerializationInvalid);
        return Result<UiPropertyValue>::Success(value.get<std::int64_t>());
    }

    [[nodiscard]] Result<UiPropertyValue> DecodeNumberProperty(const Json &value) {
        auto number = ReadFiniteNumber(value);
        return number.HasError() ? Result<UiPropertyValue>::Failure(number.ErrorValue()) : Result<UiPropertyValue>::Success(number.Value());
    }

    [[nodiscard]] Result<UiPropertyValue> DecodeTextProperty(const Json &value, const UiDocumentSerializationLimits &limits) {
        auto text = ReadText(value, limits);
        return text.HasError() ? Result<UiPropertyValue>::Failure(text.ErrorValue())
                               : Result<UiPropertyValue>::Success(std::move(text).Value());
    }

    [[nodiscard]] Result<UiPropertyValue> DecodeReferenceProperty(const Json &value, const UiDocumentSerializationLimits &limits) {
        auto reference = DecodeReference(value, limits);
        return reference.HasError() ? Result<UiPropertyValue>::Failure(reference.ErrorValue())
                                    : Result<UiPropertyValue>::Success(std::move(reference).Value());
    }

    [[nodiscard]] Result<UiPropertyValue> DecodePropertyValue(const Json &value, const UiDocumentSerializationLimits &limits) {
        if (!Horo::Foundation::HasAllowedFields(value, {"type", "value"}))
            return Failed<UiPropertyValue>(UiErrors::DocumentSerializationInvalid, "Runtime UI property value fields are not canonical.");
        auto type = ReadText(value.at("type"), limits);
        if (type.HasError())
            return Result<UiPropertyValue>::Failure(type.ErrorValue());
        if (type.Value() == "bool")
            return DecodeBooleanProperty(value.at("value"));
        if (type.Value() == "int")
            return DecodeIntegerProperty(value.at("value"));
        if (type.Value() == "number")
            return DecodeNumberProperty(value.at("value"));
        if (type.Value() == "text")
            return DecodeTextProperty(value.at("value"), limits);
        if (type.Value() == "reference")
            return DecodeReferenceProperty(value.at("value"), limits);
        return Failed<UiPropertyValue>(UiErrors::DocumentSerializationInvalid, "Runtime UI property type is unsupported.");
    }

    [[nodiscard]] OrderedJson EncodeCanvas(const UiCanvasDescriptor &canvas) {
        return OrderedJson{{"id", EncodeUiId(canvas.id)},
                           {"rootElement", EncodeUiId(canvas.rootElement)},
                           {"renderMode", RenderModeName(canvas.renderMode)},
                           {"referenceResolution",
                            OrderedJson{{"width", canvas.referenceResolution.width}, {"height", canvas.referenceResolution.height}}},
                           {"scaleMode", ScaleModeName(canvas.scaleMode)}};
    }

    [[nodiscard]] Result<UiCanvasDescriptor> DecodeCanvas(const Json &value, const UiDocumentSerializationLimits &limits) {
        if (!Horo::Foundation::HasAllowedFields(value, {"id", "rootElement", "renderMode", "referenceResolution", "scaleMode"}))
            return Failed<UiCanvasDescriptor>(UiErrors::DocumentSerializationInvalid, "Runtime UI canvas fields are not canonical.");
        auto id = DecodeUiId<UiCanvasId>(value.at("id"));
        auto root = DecodeUiId<UiElementId>(value.at("rootElement"));
        auto renderMode = ParseRenderMode(value.at("renderMode"));
        auto scaleMode = ParseScaleMode(value.at("scaleMode"));
        const auto &resolution = value.at("referenceResolution");
        if (id.HasError())
            return Result<UiCanvasDescriptor>::Failure(id.ErrorValue());
        if (root.HasError())
            return Result<UiCanvasDescriptor>::Failure(root.ErrorValue());
        if (renderMode.HasError())
            return Result<UiCanvasDescriptor>::Failure(renderMode.ErrorValue());
        if (scaleMode.HasError())
            return Result<UiCanvasDescriptor>::Failure(scaleMode.ErrorValue());
        if (!Horo::Foundation::HasAllowedFields(resolution, {"width", "height"}))
            return Failed<UiCanvasDescriptor>(UiErrors::DocumentSerializationInvalid);
        auto width = ReadUnsigned<std::uint32_t>(resolution.at("width"));
        auto height = ReadUnsigned<std::uint32_t>(resolution.at("height"));
        if (width.HasError())
            return Result<UiCanvasDescriptor>::Failure(width.ErrorValue());
        if (height.HasError())
            return Result<UiCanvasDescriptor>::Failure(height.ErrorValue());
        return Result<UiCanvasDescriptor>::Success(
            {id.Value(), root.Value(), renderMode.Value(), {width.Value(), height.Value()}, scaleMode.Value()});
    }

    [[nodiscard]] OrderedJson EncodeElement(const UiDocumentElement &element) {
        std::vector<UiTypedProperty> properties{element.properties.begin(), element.properties.end()};
        std::ranges::sort(properties, {}, &UiTypedProperty::key);
        OrderedJson encodedProperties = OrderedJson::array();
        for (const auto &property : properties)
            encodedProperties.push_back(OrderedJson{{"key", property.key}, {"value", EncodePropertyValue(property.value)}});

        std::vector<UiReference> references{element.references.begin(), element.references.end()};
        std::ranges::sort(references);
        OrderedJson encodedReferences = OrderedJson::array();
        for (const auto &reference : references)
            encodedReferences.push_back(EncodeReference(reference));
        return OrderedJson{{"id", EncodeUiId(element.id)},
                           {"parent", element.parent.IsValid() ? OrderedJson(EncodeUiId(element.parent)) : OrderedJson(nullptr)},
                           {"type", element.type.Value()},
                           {"properties", std::move(encodedProperties)},
                           {"references", std::move(encodedReferences)}};
    }

    [[nodiscard]] Result<std::vector<UiTypedProperty>> DecodeProperties(const Json &encoded, const UiDocumentSerializationLimits &limits) {
        std::vector<UiTypedProperty> properties;
        properties.reserve(encoded.size());
        for (const auto &value : encoded) {
            if (!Horo::Foundation::HasAllowedFields(value, {"key", "value"}))
                return Failed<std::vector<UiTypedProperty>>(UiErrors::DocumentSerializationInvalid);
            auto key = ReadText(value.at("key"), limits);
            auto property = DecodePropertyValue(value.at("value"), limits);
            if (key.HasError())
                return Result<std::vector<UiTypedProperty>>::Failure(key.ErrorValue());
            if (property.HasError())
                return Result<std::vector<UiTypedProperty>>::Failure(property.ErrorValue());
            properties.push_back({std::move(key).Value(), std::move(property).Value()});
        }
        return Result<std::vector<UiTypedProperty>>::Success(std::move(properties));
    }

    [[nodiscard]] Result<std::vector<UiReference>> DecodeReferences(const Json &encoded, const UiDocumentSerializationLimits &limits) {
        std::vector<UiReference> references;
        references.reserve(encoded.size());
        for (const auto &value : encoded) {
            auto reference = DecodeReference(value, limits);
            if (reference.HasError())
                return Result<std::vector<UiReference>>::Failure(reference.ErrorValue());
            references.push_back(std::move(reference).Value());
        }
        return Result<std::vector<UiReference>>::Success(std::move(references));
    }

    [[nodiscard]] Result<UiElementId> DecodeElementParent(const Json &value) {
        if (value.is_null())
            return Result<UiElementId>::Success({});
        auto parsed = DecodeUiId<UiElementId>(value);
        return parsed.HasError() ? Result<UiElementId>::Failure(parsed.ErrorValue()) : parsed;
    }

    [[nodiscard]] Result<Assets::AssetTypeId> DecodeElementType(const Json &value, const UiDocumentSerializationLimits &limits) {
        auto typeText = ReadText(value, limits);
        if (typeText.HasError())
            return Result<Assets::AssetTypeId>::Failure(typeText.ErrorValue());
        return Assets::AssetTypeId::Parse(typeText.Value());
    }

    [[nodiscard]] bool HasValidElementArrays(const Json &properties, const Json &references, const UiDocumentSerializationLimits &limits) {
        return properties.is_array() && properties.size() <= limits.maximumPropertiesPerElement && references.is_array() &&
               references.size() <= limits.maximumReferencesPerElement;
    }

    [[nodiscard]] Result<UiDocumentElement> DecodeElement(const Json &value, const UiDocumentSerializationLimits &limits) {
        if (!Horo::Foundation::HasAllowedFields(value, {"id", "parent", "type", "properties", "references"}))
            return Failed<UiDocumentElement>(UiErrors::DocumentSerializationInvalid, "Runtime UI element fields are not canonical.");
        auto id = DecodeUiId<UiElementId>(value.at("id"));
        if (id.HasError())
            return Result<UiDocumentElement>::Failure(id.ErrorValue());
        auto parent = DecodeElementParent(value.at("parent"));
        if (parent.HasError())
            return Result<UiDocumentElement>::Failure(parent.ErrorValue());
        auto type = DecodeElementType(value.at("type"), limits);
        if (type.HasError())
            return Result<UiDocumentElement>::Failure(type.ErrorValue());
        const auto &propertiesJson = value.at("properties");
        const auto &referencesJson = value.at("references");
        if (!HasValidElementArrays(propertiesJson, referencesJson, limits))
            return Failed<UiDocumentElement>(UiErrors::DocumentPayloadTooLarge);
        auto properties = DecodeProperties(propertiesJson, limits);
        auto references = DecodeReferences(referencesJson, limits);
        if (properties.HasError())
            return Result<UiDocumentElement>::Failure(properties.ErrorValue());
        if (references.HasError())
            return Result<UiDocumentElement>::Failure(references.ErrorValue());
        return Result<UiDocumentElement>::Success(
            {id.Value(), parent.Value(), type.Value(), std::move(properties).Value(), std::move(references).Value()});
    }

    [[nodiscard]] OrderedJson EncodeDependency(const UiAssetDependency &dependency) {
        return OrderedJson{{"asset", dependency.asset.ToString()},
                           {"expectedType", dependency.expectedType.Value()},
                           {"required", dependency.required}};
    }

    [[nodiscard]] Result<UiAssetDependency> DecodeDependency(const Json &value, const UiDocumentSerializationLimits &limits) {
        if (!Horo::Foundation::HasAllowedFields(value, {"asset", "expectedType", "required"}))
            return Failed<UiAssetDependency>(UiErrors::DependencyInvalid);
        auto assetText = ReadText(value.at("asset"), limits);
        auto typeText = ReadText(value.at("expectedType"), limits);
        if (assetText.HasError())
            return Result<UiAssetDependency>::Failure(assetText.ErrorValue());
        if (typeText.HasError())
            return Result<UiAssetDependency>::Failure(typeText.ErrorValue());
        auto asset = Assets::AssetId::Parse(assetText.Value());
        auto type = Assets::AssetTypeId::Parse(typeText.Value());
        if (asset.HasError() || asset.Value().ToString() != assetText.Value() || type.HasError() || !value.at("required").is_boolean())
            return Failed<UiAssetDependency>(UiErrors::DependencyInvalid);
        return Result<UiAssetDependency>::Success({asset.Value(), type.Value(), value.at("required").get<bool>()});
    }

    [[nodiscard]] OrderedJson EncodeRoute(const UiRouteMetadata &route) {
        return OrderedJson{{"id", EncodeUiId(route.id)}, {"band", BandName(route.band)}, {"order", route.order}, {"modal", route.modal}};
    }

    [[nodiscard]] Result<UiRouteMetadata> DecodeRoute(const Json &value, const UiDocumentSerializationLimits &limits) {
        if (!Horo::Foundation::HasAllowedFields(value, {"id", "band", "order", "modal"}))
            return Failed<UiRouteMetadata>(UiErrors::DocumentRouteInvalid);
        auto id = DecodeUiId<UiRouteId>(value.at("id"));
        auto band = ParseBand(value.at("band"));
        auto order = ReadUnsigned<std::uint32_t>(value.at("order"));
        if (id.HasError())
            return Result<UiRouteMetadata>::Failure(id.ErrorValue());
        if (band.HasError())
            return Result<UiRouteMetadata>::Failure(band.ErrorValue());
        if (order.HasError() || !value.at("modal").is_boolean())
            return Failed<UiRouteMetadata>(UiErrors::DocumentRouteInvalid);
        return Result<UiRouteMetadata>::Success({id.Value(), band.Value(), order.Value(), value.at("modal").get<bool>()});
    }

    [[nodiscard]] Result<void> ValidateJsonSource(const std::string_view source, const UiDocumentSerializationLimits &limits) {
        if (!limits.IsValid())
            return Failed(UiErrors::DocumentSerializationInvalid, "Runtime UI serialization limits are invalid.");
        if (source.empty() || source.size() > limits.maximumSourceBytes)
            return Failed(UiErrors::DocumentPayloadTooLarge);
        if (!IsValidUtf8ScalarSequence(source))
            return Failed(UiErrors::DocumentSerializationInvalid, "Runtime UI source is not valid UTF-8.");
        return Result<void>::Success();
    }

    [[nodiscard]] Result<Json> ParseJsonValue(const std::string_view source, const UiDocumentSerializationLimits &limits) {
        try {
            Horo::Foundation::JsonParseGuard guard{limits.maximumJsonDepth};
            Json root = Json::parse(source, std::ref(guard), true, false);
            if (guard.HasDuplicate())
                return Failed<Json>(UiErrors::DocumentSerializationInvalid, "Runtime UI source contains a duplicate JSON field.");
            if (guard.IsTooDeep())
                return Failed<Json>(UiErrors::DocumentPayloadTooLarge);
            if (root.is_discarded() || !root.is_object())
                return Failed<Json>(UiErrors::DocumentSerializationInvalid, "Runtime UI source is not an object.");
            return Result<Json>::Success(std::move(root));
        } catch (const Json::exception &) {
            return Failed<Json>(UiErrors::DocumentSerializationInvalid, "Runtime UI source is not valid JSON.");
        } catch (const std::bad_alloc &) {
            return Failed<Json>(UiErrors::DocumentPayloadTooLarge);
        }
    }

    [[nodiscard]] Result<Json> ParseJson(std::string_view source, const UiDocumentSerializationLimits &limits) {
        if (auto valid = ValidateJsonSource(source, limits); valid.HasError())
            return Result<Json>::Failure(valid.ErrorValue());
        return ParseJsonValue(source, limits);
    }

    template Result<std::uint16_t> ReadUnsigned<std::uint16_t>(const Json &value);
    template Result<std::uint64_t> ReadUnsigned<std::uint64_t>(const Json &value);
    template Result<UiDocumentId> DecodeUiId<UiDocumentId>(const Json &value);

}  // namespace Horo::Runtime::Ui::SerializationInternal
