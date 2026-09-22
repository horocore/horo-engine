#include "Horo/Foundation/Utf8.h"
#include "JsonUtils.h"
#include "UiDocumentSerializationInternal.h"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui::SerializationInternal {
    using Json = nlohmann::json;
    using OrderedJson = nlohmann::ordered_json;

    template <typename T> [[nodiscard]] Result<T> Failed(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
        return Result<T>::Failure(MakeError(descriptor, std::move(message)));
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

    template <typename Value> [[nodiscard]] Result<UiLocalizedArgumentValue> DecodeLocalizedIntegerArgument(const Json &encoded) {
        auto decoded = ReadLocalizedInteger(encoded);
        if (decoded.HasError())
            return Result<UiLocalizedArgumentValue>::Failure(decoded.ErrorValue());
        return Result<UiLocalizedArgumentValue>::Success(UiLocalizedArgumentValue{Value{decoded.Value()}});
    }

    [[nodiscard]] Result<UiLocalizedArgumentValue> DecodeLocalizedNumberArgument(const Json &encoded) {
        auto decoded = ReadFiniteNumber(encoded);
        if (decoded.HasError())
            return Result<UiLocalizedArgumentValue>::Failure(decoded.ErrorValue());
        return Result<UiLocalizedArgumentValue>::Success(UiLocalizedArgumentValue{decoded.Value()});
    }

    [[nodiscard]] Result<UiLocalizedArgumentValue> DecodeLocalizedBooleanArgument(const Json &encoded) {
        if (!encoded.is_boolean())
            return Failed<UiLocalizedArgumentValue>(UiErrors::LocalizedArgumentInvalid);
        return Result<UiLocalizedArgumentValue>::Success(UiLocalizedArgumentValue{encoded.get<bool>()});
    }

    [[nodiscard]] Result<UiLocalizedArgumentValue> DecodeLocalizedEnumArgument(const Json &encoded) {
        if (!Horo::Foundation::HasAllowedFields(encoded, {"typeId", "value"}))
            return Failed<UiLocalizedArgumentValue>(UiErrors::LocalizedArgumentInvalid);
        auto typeId = ReadUnsigned<std::uint64_t>(encoded.at("typeId"));
        auto enumValue = ReadUnsigned<std::uint64_t>(encoded.at("value"));
        if (typeId.HasError())
            return Result<UiLocalizedArgumentValue>::Failure(typeId.ErrorValue());
        if (enumValue.HasError())
            return Result<UiLocalizedArgumentValue>::Failure(enumValue.ErrorValue());
        return Result<UiLocalizedArgumentValue>::Success(
            UiLocalizedArgumentValue{UiLocalizedStableEnum{typeId.Value(), enumValue.Value()}});
    }

    [[nodiscard]] Result<UiLocalizedArgumentValue> DecodeLocalizedTextArgument(const Json &encoded) {
        auto decoded = ReadLocalizedString(encoded, MaximumUiLocalizedValueBytes, UiErrors::LocalizedArgumentInvalid);
        if (decoded.HasError())
            return Result<UiLocalizedArgumentValue>::Failure(decoded.ErrorValue());
        return Result<UiLocalizedArgumentValue>::Success(UiLocalizedArgumentValue{std::move(decoded).Value()});
    }

    [[nodiscard]] Result<UiLocalizedArgumentValue> DecodeLocalizedShortcutArgument(const Json &encoded) {
        if (!Horo::Foundation::HasAllowedFields(encoded, {"modifiers", "key"}))
            return Failed<UiLocalizedArgumentValue>(UiErrors::LocalizedArgumentInvalid);
        auto modifiers = ReadUnsigned<std::uint32_t>(encoded.at("modifiers"));
        auto key = ReadUnsigned<std::uint32_t>(encoded.at("key"));
        if (modifiers.HasError())
            return Result<UiLocalizedArgumentValue>::Failure(modifiers.ErrorValue());
        if (key.HasError())
            return Result<UiLocalizedArgumentValue>::Failure(key.ErrorValue());
        return Result<UiLocalizedArgumentValue>::Success(UiLocalizedArgumentValue{UiLocalizedShortcut{modifiers.Value(), key.Value()}});
    }

    [[nodiscard]] Result<UiLocalizedArgumentValue> DecodeLocalizedArgumentValue(const Json &value) {
        if (!Horo::Foundation::HasAllowedFields(value, {"type", "value"}) || !value.at("type").is_string())
            return Failed<UiLocalizedArgumentValue>(UiErrors::LocalizedArgumentInvalid);
        const auto type = value.at("type").get<std::string>();
        const auto &encoded = value.at("value");
        if (type == "int")
            return DecodeLocalizedIntegerArgument<std::int64_t>(encoded);
        if (type == "number")
            return DecodeLocalizedNumberArgument(encoded);
        if (type == "bool")
            return DecodeLocalizedBooleanArgument(encoded);
        if (type == "dateTime")
            return DecodeLocalizedIntegerArgument<UiLocalizedDateTime>(encoded);
        if (type == "duration")
            return DecodeLocalizedIntegerArgument<UiLocalizedDuration>(encoded);
        if (type == "enum")
            return DecodeLocalizedEnumArgument(encoded);
        if (type == "text")
            return DecodeLocalizedTextArgument(encoded);
        if (type == "shortcut")
            return DecodeLocalizedShortcutArgument(encoded);
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

    [[nodiscard]] Result<UiLocalizedText> DecodeLocalizedText(const Json &value, const UiDocumentSerializationLimits &) {
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
}  // namespace Horo::Runtime::Ui::SerializationInternal
