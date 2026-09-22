#include "Horo/Runtime/Ui/UiLocalization.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiAssetDependencyInternal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsAsciiLower(const char value) noexcept {
            return value >= 'a' && value <= 'z';
        }

        [[nodiscard]] bool IsAsciiUpper(const char value) noexcept {
            return value >= 'A' && value <= 'Z';
        }

        [[nodiscard]] bool IsAsciiAlpha(const char value) noexcept {
            return IsAsciiLower(value) || IsAsciiUpper(value);
        }

        [[nodiscard]] bool IsAsciiDigit(const char value) noexcept {
            return value >= '0' && value <= '9';
        }

        [[nodiscard]] bool IsAsciiAlphaNumeric(const char value) noexcept {
            return IsAsciiAlpha(value) || IsAsciiDigit(value);
        }

        [[nodiscard]] char ToLowerAscii(const char value) noexcept {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        }

        [[nodiscard]] char ToUpperAscii(const char value) noexcept {
            return value >= 'a' && value <= 'z' ? static_cast<char>(value - 'a' + 'A') : value;
        }

        template <typename Predicate> [[nodiscard]] bool IsAll(const std::string_view value, Predicate predicate) noexcept {
            return !value.empty() && std::ranges::all_of(value, predicate);
        }

        [[nodiscard]] bool IsLowercaseAlphaNumeric(const std::string_view value) noexcept {
            return IsAll(value, IsAsciiAlphaNumeric) && std::ranges::all_of(value, [](const char character) noexcept {
                return !IsAsciiAlpha(character) || IsAsciiLower(character);
            });
        }

        [[nodiscard]] bool IsNormalizedLocaleSegment(const std::string_view segment, const std::size_t segmentIndex) noexcept {
            if (segment.empty())
                return false;
            if (segmentIndex == 0)
                return segment.size() >= 2 && segment.size() <= 8 && IsAll(segment, IsAsciiLower);
            if (segment.size() == 4 && IsAll(segment, IsAsciiAlpha))
                return IsAsciiUpper(segment.front()) && IsAll(segment.substr(1), IsAsciiLower);
            if (segment.size() == 2 && IsAll(segment, IsAsciiAlpha))
                return IsAll(segment, IsAsciiUpper);
            if (segment.size() == 3 && IsAll(segment, IsAsciiDigit))
                return true;
            return segment.size() <= 8 && IsLowercaseAlphaNumeric(segment);
        }

        [[nodiscard]] bool IsNormalizedLocale(const std::string_view value) noexcept {
            if (value.empty() || value.size() > MaximumUiLocaleTagBytes || value.front() == '-' || value.back() == '-')
                return false;

            std::size_t segmentStart = 0;
            std::size_t segmentIndex = 0;
            while (segmentStart < value.size()) {
                const std::size_t separator = value.find('-', segmentStart);
                const std::size_t segmentEnd = separator == std::string_view::npos ? value.size() : separator;
                const std::string_view segment = value.substr(segmentStart, segmentEnd - segmentStart);
                if (!IsNormalizedLocaleSegment(segment, segmentIndex))
                    return false;

                ++segmentIndex;
                if (separator == std::string_view::npos)
                    break;
                segmentStart = separator + 1;
            }
            return true;
        }

        [[nodiscard]] bool AppendNormalizedLocaleSegment(const std::string_view segment, const std::size_t segmentIndex,
                                                         std::string &normalized) {
            if (segment.empty())
                return false;
            if (segmentIndex == 0) {
                if (segment.size() < 2 || segment.size() > 8 || !IsAll(segment, IsAsciiAlpha))
                    return false;
                for (const char character : segment)
                    normalized.push_back(ToLowerAscii(character));
                return true;
            }
            if (segment.size() == 4 && IsAll(segment, IsAsciiAlpha)) {
                normalized.push_back(ToUpperAscii(segment.front()));
                for (std::size_t index = 1; index < segment.size(); ++index)
                    normalized.push_back(ToLowerAscii(segment[index]));
                return true;
            }
            if (segment.size() == 2 && IsAll(segment, IsAsciiAlpha)) {
                for (const char character : segment)
                    normalized.push_back(ToUpperAscii(character));
                return true;
            }
            if (segment.size() == 3 && IsAll(segment, IsAsciiDigit)) {
                normalized.append(segment);
                return true;
            }
            if (segment.size() > 8 || !IsAll(segment, IsAsciiAlphaNumeric))
                return false;
            for (const char character : segment)
                normalized.push_back(ToLowerAscii(character));
            return true;
        }

        [[nodiscard]] std::optional<std::string> NormalizeLocale(const std::string_view tag) {
            if (tag.empty() || tag.size() > MaximumUiLocaleTagBytes)
                return std::nullopt;

            std::string normalized;
            normalized.reserve(tag.size());
            std::size_t segmentStart = 0;
            std::size_t segmentIndex = 0;
            while (segmentStart < tag.size()) {
                const std::size_t separator = tag.find('-', segmentStart);
                const std::size_t segmentEnd = separator == std::string_view::npos ? tag.size() : separator;
                const std::string_view segment = tag.substr(segmentStart, segmentEnd - segmentStart);
                if (segmentIndex != 0 && normalized.size() != 0)
                    normalized.push_back('-');
                if (!AppendNormalizedLocaleSegment(segment, segmentIndex, normalized))
                    return std::nullopt;

                ++segmentIndex;
                if (separator == std::string_view::npos)
                    break;
                segmentStart = separator + 1;
            }

            return IsNormalizedLocale(normalized) ? std::optional<std::string>{std::move(normalized)} : std::nullopt;
        }

        [[nodiscard]] bool IsSafeKey(const std::string_view value) noexcept {
            return !value.empty() && value.size() <= MaximumUiMessageKeyBytes &&
                   std::ranges::all_of(value, [](const char character) noexcept {
                return IsAsciiAlphaNumeric(character) || character == '_' || character == '-' || character == '.';
            });
        }

        [[nodiscard]] bool IsSafeArgumentName(const std::string_view value) noexcept {
            return IsSafeKey(value) && value.size() <= MaximumUiLocalizedArgumentNameBytes;
        }

        [[nodiscard]] bool IsValidFailurePolicy(const UiLocalizedTextFailurePolicy policy) noexcept {
            using enum UiLocalizedTextFailurePolicy;
            return policy == UseFallback || policy == UseSafePlaceholder || policy == RequireTranslation;
        }

        [[nodiscard]] bool IsValidArgumentValue(const UiLocalizedArgumentValue &value) noexcept {
            return std::visit([]<typename T>(const T &typed) noexcept {
                using Value = std::decay_t<T>;
                if constexpr (std::is_same_v<Value, double>) {
                    return std::isfinite(typed);
                } else if constexpr (std::is_same_v<Value, UiLocalizedDuration>) {
                    return typed.milliseconds >= 0;
                } else if constexpr (std::is_same_v<Value, UiLocalizedStableEnum>) {
                    return typed.IsValid();
                } else if constexpr (std::is_same_v<Value, UiLocalizedShortcut>) {
                    return typed.IsValid();
                } else if constexpr (std::is_same_v<Value, std::string>) {
                    return typed.size() <= MaximumUiLocalizedValueBytes && IsValidUtf8ScalarSequence(typed);
                } else {
                    return true;
                }
            }, value);
        }

        [[nodiscard]] bool IsValidArgument(const UiLocalizedArgument &argument) noexcept {
            return IsSafeArgumentName(argument.name) && IsValidArgumentValue(argument.value);
        }

        [[nodiscard]] bool IsValidAssetFallbackPolicy(const UiLocalizedAssetFallbackPolicy policy) noexcept {
            using enum UiLocalizedAssetFallbackPolicy;
            return policy == Required || policy == UseNeutral || policy == Omit;
        }

        [[nodiscard]] Result<void> ValidateLocalizedAssetReferenceInput(const Assets::AssetTypeId &expectedType,
                                                                        const UiLocalizedAssetFallbackPolicy fallbackPolicy,
                                                                        const std::vector<UiLocalizedAssetVariant> &variants,
                                                                        const std::optional<Assets::AssetId> &neutralAsset) {
            if (variants.size() > MaximumUiLocalizedAssetVariants)
                return Failure(UiErrors::CapacityExceeded);
            if (expectedType.Value().empty() || !IsValidAssetFallbackPolicy(fallbackPolicy))
                return Failure(UiErrors::LocalizedAssetReferenceInvalid);

            const bool hasNeutral = neutralAsset.has_value();
            using enum UiLocalizedAssetFallbackPolicy;
            if ((fallbackPolicy == Required && hasNeutral) || (fallbackPolicy == Omit && hasNeutral) ||
                (fallbackPolicy == UseNeutral && !hasNeutral) || (variants.empty() && !(fallbackPolicy == UseNeutral && hasNeutral)))
                return Failure(UiErrors::LocalizedAssetReferenceInvalid);
            if (hasNeutral && !neutralAsset->IsValid())
                return Failure(UiErrors::LocalizedAssetReferenceInvalid);
            for (const UiLocalizedAssetVariant &variant : variants)
                if (!variant.locale.IsValid() || !variant.asset.IsValid())
                    return Failure(UiErrors::LocalizedAssetReferenceInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> SortAndValidateLocalizedAssetVariants(std::vector<UiLocalizedAssetVariant> &variants) {
            std::ranges::sort(variants, {}, &UiLocalizedAssetVariant::locale);
            for (std::size_t index = 1; index < variants.size(); ++index)
                if (variants[index - 1].locale == variants[index].locale)
                    return Failure(UiErrors::LocalizedAssetVariantConflict);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> MergeLocalizedAssetDependency(std::vector<UiAssetDependency> &dependencies,
                                                                 UiAssetDependency dependency) {
            using enum Internal::UiAssetDependencyMergeResult;
            const auto merged = Internal::MergeUiAssetDependency(dependencies, std::move(dependency));
            switch (merged) {
                case Inserted:
                case Strengthened:
                    return Result<void>::Success();
                case Invalid:
                    return Failure(UiErrors::DependencyInvalid);
                case Conflict:
                    return Failure(UiErrors::LocalizedAssetVariantConflict);
                case CapacityExceeded:
                    return Failure(UiErrors::CapacityExceeded);
            }
            return Failure(UiErrors::DependencyInvalid);
        }

        [[nodiscard]] Result<std::vector<UiAssetDependency>> BuildLocalizedAssetDependencies(
            const Assets::AssetTypeId &expectedType, const UiLocalizedAssetFallbackPolicy fallbackPolicy,
            const std::vector<UiLocalizedAssetVariant> &variants, const std::optional<Assets::AssetId> &neutralAsset) {
            std::vector<UiAssetDependency> dependencies;
            dependencies.reserve(variants.size() + (neutralAsset.has_value() ? 1U : 0U));
            const bool localizedVariantsRequired = fallbackPolicy == UiLocalizedAssetFallbackPolicy::Required;
            for (const UiLocalizedAssetVariant &variant : variants) {
                if (auto result = MergeLocalizedAssetDependency(dependencies, {variant.asset, expectedType, localizedVariantsRequired});
                    result.HasError())
                    return Result<std::vector<UiAssetDependency>>::Failure(result.ErrorValue());
            }
            if (neutralAsset.has_value()) {
                if (auto result = MergeLocalizedAssetDependency(dependencies, {neutralAsset.value(), expectedType, true});
                    result.HasError())
                    return Result<std::vector<UiAssetDependency>>::Failure(result.ErrorValue());
            }
            return Result<std::vector<UiAssetDependency>>::Success(std::move(dependencies));
        }
    }  // namespace

    /** @copydoc UiLocaleTag::Parse */
    Result<UiLocaleTag> UiLocaleTag::Parse(const std::string_view tag) {
        auto normalized = NormalizeLocale(tag);
        if (!normalized.has_value())
            return Failure<UiLocaleTag>(UiErrors::LocaleInvalid);
        return Result<UiLocaleTag>::Success(UiLocaleTag{std::move(*normalized)});
    }

    /** @copydoc UiLocaleTag::IsValid */
    bool UiLocaleTag::IsValid() const noexcept {
        return IsNormalizedLocale(value);
    }

    /** @copydoc UiLocaleTag::Value */
    std::string_view UiLocaleTag::Value() const noexcept {
        return value;
    }

    /** @copydoc UiMessageKey::Create */
    Result<UiMessageKey> UiMessageKey::Create(const std::string_view namespaceId, const std::string_view localKey) {
        UiMessageKey key{std::string{namespaceId}, std::string{localKey}};
        if (!key.IsValid())
            return Failure<UiMessageKey>(UiErrors::LocalizedKeyInvalid);
        return Result<UiMessageKey>::Success(std::move(key));
    }

    /** @copydoc UiMessageKey::IsValid */
    bool UiMessageKey::IsValid() const noexcept {
        return IsSafeKey(namespaceId) && IsSafeKey(localKey);
    }

    /** @copydoc UiMessageKey::Canonical */
    std::string UiMessageKey::Canonical() const {
        std::string result;
        result.reserve(namespaceId.size() + 1 + localKey.size());
        result.append(namespaceId);
        result.push_back(':');
        result.append(localKey);
        return result;
    }

    /** @copydoc UiLocalizedText::UiLocalizedText */
    UiLocalizedText::UiLocalizedText(UiMessageKey key, std::vector<UiLocalizedArgument> arguments, std::string fallbackText,
                                     const UiLocalizedTextFailurePolicy failurePolicy) noexcept
        : key_(std::move(key)), arguments_(std::move(arguments)), fallbackText_(std::move(fallbackText)), failurePolicy_(failurePolicy) {}

    /** @copydoc UiLocalizedText::Create */
    Result<UiLocalizedText> UiLocalizedText::Create(UiMessageKey key, std::vector<UiLocalizedArgument> arguments, std::string fallbackText,
                                                    const UiLocalizedTextFailurePolicy failurePolicy) {
        if (!key.IsValid() || !IsValidFailurePolicy(failurePolicy) || fallbackText.size() > MaximumUiLocalizedFallbackTextBytes ||
            !IsValidUtf8ScalarSequence(fallbackText) ||
            (failurePolicy == UiLocalizedTextFailurePolicy::UseFallback && fallbackText.empty()))
            return Failure<UiLocalizedText>(UiErrors::LocalizedMessageInvalid);
        if (arguments.size() > MaximumUiLocalizedArguments)
            return Failure<UiLocalizedText>(UiErrors::LocalizedArgumentCapacityExceeded);

        for (const UiLocalizedArgument &argument : arguments)
            if (!IsValidArgument(argument))
                return Failure<UiLocalizedText>(UiErrors::LocalizedArgumentInvalid);

        std::ranges::sort(arguments, {}, &UiLocalizedArgument::name);
        for (std::size_t index = 1; index < arguments.size(); ++index)
            if (arguments[index - 1].name == arguments[index].name)
                return Failure<UiLocalizedText>(UiErrors::LocalizedArgumentConflict);

        return Result<UiLocalizedText>::Success(
            UiLocalizedText{std::move(key), std::move(arguments), std::move(fallbackText), failurePolicy});
    }

    /** @copydoc UiLocalizedText::Create */
    Result<UiLocalizedText> UiLocalizedText::Create(UiMessageKey key, std::string fallbackText,
                                                    const UiLocalizedTextFailurePolicy failurePolicy) {
        return Create(std::move(key), {}, std::move(fallbackText), failurePolicy);
    }

    /** @copydoc UiLocalizedText::IsValid */
    bool UiLocalizedText::IsValid() const noexcept {
        if (!key_.IsValid() || !IsValidFailurePolicy(failurePolicy_) || fallbackText_.size() > MaximumUiLocalizedFallbackTextBytes ||
            !IsValidUtf8ScalarSequence(fallbackText_) || arguments_.size() > MaximumUiLocalizedArguments ||
            (failurePolicy_ == UiLocalizedTextFailurePolicy::UseFallback && fallbackText_.empty()))
            return false;
        for (std::size_t index = 0; index < arguments_.size(); ++index) {
            if (!IsValidArgument(arguments_[index]) || (index != 0 && arguments_[index - 1].name >= arguments_[index].name))
                return false;
        }
        return true;
    }

    /** @copydoc UiLocalizedText::Key */
    const UiMessageKey &UiLocalizedText::Key() const noexcept {
        return key_;
    }

    /** @copydoc UiLocalizedText::Arguments */
    std::span<const UiLocalizedArgument> UiLocalizedText::Arguments() const noexcept {
        return arguments_;
    }

    /** @copydoc UiLocalizedText::FindArgument */
    const UiLocalizedArgument *UiLocalizedText::FindArgument(const std::string_view name) const noexcept {
        const auto found = std::ranges::lower_bound(arguments_, name, {}, &UiLocalizedArgument::name);
        return found != arguments_.end() && found->name == name ? &*found : nullptr;
    }

    /** @copydoc UiLocalizedText::FallbackText */
    std::string_view UiLocalizedText::FallbackText() const noexcept {
        return fallbackText_;
    }

    /** @copydoc UiLocalizedText::FailurePolicy */
    UiLocalizedTextFailurePolicy UiLocalizedText::FailurePolicy() const noexcept {
        return failurePolicy_;
    }

    /** @copydoc UiLocalizedAssetReference::UiLocalizedAssetReference */
    UiLocalizedAssetReference::UiLocalizedAssetReference(Assets::AssetTypeId expectedType,
                                                         const UiLocalizedAssetFallbackPolicy fallbackPolicy,
                                                         std::vector<UiLocalizedAssetVariant> variants,
                                                         std::optional<Assets::AssetId> neutralAsset,
                                                         std::vector<UiAssetDependency> dependencies) noexcept
        : expectedType_(std::move(expectedType)), fallbackPolicy_(fallbackPolicy), variants_(std::move(variants)),
          neutralAsset_(neutralAsset), dependencies_(std::move(dependencies)) {}

    /** @copydoc UiLocalizedAssetReference::Create */
    Result<UiLocalizedAssetReference> UiLocalizedAssetReference::Create(Assets::AssetTypeId expectedType,
                                                                        const UiLocalizedAssetFallbackPolicy fallbackPolicy,
                                                                        std::vector<UiLocalizedAssetVariant> variants,
                                                                        const std::optional<Assets::AssetId> neutralAsset) {
        if (auto validation = ValidateLocalizedAssetReferenceInput(expectedType, fallbackPolicy, variants, neutralAsset);
            validation.HasError())
            return Result<UiLocalizedAssetReference>::Failure(validation.ErrorValue());
        if (auto validation = SortAndValidateLocalizedAssetVariants(variants); validation.HasError())
            return Result<UiLocalizedAssetReference>::Failure(validation.ErrorValue());
        auto dependencies = BuildLocalizedAssetDependencies(expectedType, fallbackPolicy, variants, neutralAsset);
        if (dependencies.HasError())
            return Result<UiLocalizedAssetReference>::Failure(dependencies.ErrorValue());

        return Result<UiLocalizedAssetReference>::Success(UiLocalizedAssetReference{std::move(expectedType), fallbackPolicy,
                                                                                    std::move(variants), neutralAsset,
                                                                                    std::move(dependencies).Value()});
    }

    /** @copydoc UiLocalizedAssetReference::IsValid */
    bool UiLocalizedAssetReference::IsValid() const noexcept {
        if (expectedType_.Value().empty() || !IsValidAssetFallbackPolicy(fallbackPolicy_) ||
            variants_.size() > MaximumUiLocalizedAssetVariants || dependencies_.empty())
            return false;
        for (std::size_t index = 0; index < variants_.size(); ++index) {
            if (!variants_[index].locale.IsValid() || !variants_[index].asset.IsValid() ||
                (index != 0 && variants_[index - 1].locale >= variants_[index].locale))
                return false;
        }
        using enum UiLocalizedAssetFallbackPolicy;
        if ((fallbackPolicy_ == Required || fallbackPolicy_ == Omit) && neutralAsset_.has_value())
            return false;
        if (fallbackPolicy_ == UseNeutral && !neutralAsset_.has_value())
            return false;
        return !neutralAsset_.has_value() || neutralAsset_->IsValid();
    }

    /** @copydoc UiLocalizedAssetReference::ExpectedType */
    const Assets::AssetTypeId &UiLocalizedAssetReference::ExpectedType() const noexcept {
        return expectedType_;
    }

    /** @copydoc UiLocalizedAssetReference::Variants */
    std::span<const UiLocalizedAssetVariant> UiLocalizedAssetReference::Variants() const noexcept {
        return variants_;
    }

    /** @copydoc UiLocalizedAssetReference::NeutralAsset */
    std::optional<Assets::AssetId> UiLocalizedAssetReference::NeutralAsset() const noexcept {
        return neutralAsset_;
    }

    /** @copydoc UiLocalizedAssetReference::FallbackPolicy */
    UiLocalizedAssetFallbackPolicy UiLocalizedAssetReference::FallbackPolicy() const noexcept {
        return fallbackPolicy_;
    }

    /** @copydoc UiLocalizedAssetReference::Dependencies */
    std::span<const UiAssetDependency> UiLocalizedAssetReference::Dependencies() const noexcept {
        return dependencies_;
    }

    /** @copydoc UiLocalizedAssetReference::Resolve */
    Result<std::optional<UiLocalizedAssetSelection>> UiLocalizedAssetReference::Resolve(
        const std::span<const UiLocaleTag> localeChain) const noexcept {
        if (localeChain.empty() || localeChain.size() > MaximumUiLocaleFallbackChain)
            return Failure<std::optional<UiLocalizedAssetSelection>>(UiErrors::LocaleFallbackChainInvalid);
        for (const UiLocaleTag &locale : localeChain)
            if (!locale.IsValid())
                return Failure<std::optional<UiLocalizedAssetSelection>>(UiErrors::LocaleFallbackChainInvalid);
        for (std::size_t index = 0; index < localeChain.size(); ++index)
            for (std::size_t previous = 0; previous < index; ++previous)
                if (localeChain[previous] == localeChain[index])
                    return Failure<std::optional<UiLocalizedAssetSelection>>(UiErrors::LocaleFallbackChainInvalid);

        for (const UiLocaleTag &locale : localeChain) {
            const auto found = std::ranges::lower_bound(variants_, locale, {}, &UiLocalizedAssetVariant::locale);
            if (found != variants_.end() && found->locale == locale)
                return Result<std::optional<UiLocalizedAssetSelection>>::Success(
                    UiLocalizedAssetSelection{found->asset, found->locale.Value(), false});
        }

        using enum UiLocalizedAssetFallbackPolicy;
        if (fallbackPolicy_ == UseNeutral && neutralAsset_.has_value())
            return Result<std::optional<UiLocalizedAssetSelection>>::Success(UiLocalizedAssetSelection{neutralAsset_.value(), {}, true});
        if (fallbackPolicy_ == Omit)
            return Result<std::optional<UiLocalizedAssetSelection>>::Success(std::nullopt);
        return Failure<std::optional<UiLocalizedAssetSelection>>(UiErrors::LocalizedAssetUnavailable);
    }

    /** @copydoc UiLocalizedAssetReference::Resolve */
    Result<std::optional<UiLocalizedAssetSelection>> UiLocalizedAssetReference::Resolve(
        const UiLocaleFallbackChain &localeChain) const noexcept {
        return Resolve(localeChain.Locales());
    }

    /** @copydoc UiLocaleFallbackChain::UiLocaleFallbackChain */
    UiLocaleFallbackChain::UiLocaleFallbackChain(std::vector<UiLocaleTag> locales) noexcept : locales_(std::move(locales)) {}

    /** @copydoc UiLocaleFallbackChain::Create */
    Result<UiLocaleFallbackChain> UiLocaleFallbackChain::Create(std::vector<UiLocaleTag> locales) {
        if (locales.empty() || locales.size() > MaximumUiLocaleFallbackChain)
            return Failure<UiLocaleFallbackChain>(UiErrors::LocaleFallbackChainInvalid);
        for (std::size_t index = 0; index < locales.size(); ++index) {
            if (!locales[index].IsValid())
                return Failure<UiLocaleFallbackChain>(UiErrors::LocaleFallbackChainInvalid);
            for (std::size_t previous = 0; previous < index; ++previous)
                if (locales[previous] == locales[index])
                    return Failure<UiLocaleFallbackChain>(UiErrors::LocaleFallbackChainInvalid);
        }
        return Result<UiLocaleFallbackChain>::Success(UiLocaleFallbackChain{std::move(locales)});
    }

    /** @copydoc UiLocaleFallbackChain::Locales */
    std::span<const UiLocaleTag> UiLocaleFallbackChain::Locales() const noexcept {
        return locales_;
    }
}  // namespace Horo::Runtime::Ui
