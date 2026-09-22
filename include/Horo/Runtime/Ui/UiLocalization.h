#pragma once

/**
 * @file UiLocalization.h
 * @brief Stable Runtime UI localized text, locale evidence, and asset variants.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Ui/UiAssetDependency.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Horo::Runtime::Ui {
    /** @brief Maximum bytes in one normalized locale tag. */
    inline constexpr std::size_t MaximumUiLocaleTagBytes = 63;
    /** @brief Maximum entries in a locale fallback chain. */
    inline constexpr std::size_t MaximumUiLocaleFallbackChain = 16;
    /** @brief Maximum bytes in one namespace or local message key. */
    inline constexpr std::size_t MaximumUiMessageKeyBytes = 128;
    /** @brief Maximum bytes in one named localization argument. */
    inline constexpr std::size_t MaximumUiLocalizedArgumentNameBytes = 64;
    /** @brief Maximum number of named arguments in one message reference. */
    inline constexpr std::size_t MaximumUiLocalizedArguments = 16;
    /** @brief Maximum bytes in one owned textual localization value. */
    inline constexpr std::size_t MaximumUiLocalizedValueBytes = 1'024;
    /** @brief Maximum bytes in one source fallback message. */
    inline constexpr std::size_t MaximumUiLocalizedFallbackTextBytes = 4'096;
    /** @brief Maximum declared locale variants in one localized asset reference. */
    inline constexpr std::size_t MaximumUiLocalizedAssetVariants = 64;

    /**
     * @brief Owned normalized BCP 47 locale evidence supplied by Localization.
     *
     * This value validates and stores a locale tag but does not select a locale,
     * inspect platform state, or build a fallback policy. Those decisions remain
     * with the application and Localization authorities.
     */
    struct UiLocaleTag final {
        std::string value;

        /**
         * @brief Parses and normalizes one BCP 47 language tag.
         * @param tag Locale text supplied by an authoring or Localization boundary.
         * @return Owned normalized tag or UiErrors::LocaleInvalid.
         */
        [[nodiscard]] static Result<UiLocaleTag> Parse(std::string_view tag);
        /** @brief Checks that this value is a normalized, bounded locale tag. @return True when valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the normalized tag. @return Borrowed text owned by this value. */
        [[nodiscard]] std::string_view Value() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLocaleTag &) const noexcept = default;
    };

    /** @brief Alias used by the Localization boundary for normalized locale evidence. */
    using NormalizedLocale = UiLocaleTag;
    /** @brief Short alias for callers that use the generic locale-tag terminology. */
    using LocaleTag = UiLocaleTag;

    /**
     * @brief Stable owner-qualified message identity.
     *
     * Namespace and local key remain separate in memory; callers do not parse a
     * canonical string during lookup.
     */
    struct UiMessageKey final {
        std::string namespaceId;
        std::string localKey;

        /**
         * @brief Creates one validated semantic message identity.
         * @param namespaceId Stable namespace owner.
         * @param localKey Semantic append-only local key.
         * @return Owned key or UiErrors::LocalizedKeyInvalid.
         */
        [[nodiscard]] static Result<UiMessageKey> Create(std::string_view namespaceId, std::string_view localKey);
        /** @brief Checks representation and key grammar. @return True when valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the canonical namespace:local form. @return Newly allocated canonical key text. */
        [[nodiscard]] std::string Canonical() const;
        [[nodiscard]] auto operator<=>(const UiMessageKey &) const noexcept = default;
    };

    /** @brief Short alias matching the Localization message contract terminology. */
    using MessageKey = UiMessageKey;

    /** @brief Stable date/time argument represented as UTC milliseconds since the Unix epoch. */
    struct UiLocalizedDateTime final {
        std::int64_t unixMilliseconds{};
        [[nodiscard]] auto operator<=>(const UiLocalizedDateTime &) const noexcept = default;
    };

    /** @brief Non-negative duration argument represented in milliseconds. */
    struct UiLocalizedDuration final {
        std::int64_t milliseconds{};
        [[nodiscard]] auto operator<=>(const UiLocalizedDuration &) const noexcept = default;
    };

    /** @brief Stable typed enum evidence; type identity is owned by the declaring schema. */
    struct UiLocalizedStableEnum final {
        std::uint64_t typeId{};
        std::uint64_t value{};

        [[nodiscard]] bool IsValid() const noexcept {
            return typeId != 0;
        }

        [[nodiscard]] auto operator<=>(const UiLocalizedStableEnum &) const noexcept = default;
    };

    /** @brief Safe shortcut evidence formatted by the Localization boundary. */
    struct UiLocalizedShortcut final {
        std::uint32_t modifiers{};
        std::uint32_t key{};

        [[nodiscard]] bool IsValid() const noexcept {
            return key != 0;
        }

        [[nodiscard]] auto operator<=>(const UiLocalizedShortcut &) const noexcept = default;
    };

    /**
     * @brief Closed, owned argument value vocabulary accepted by Runtime UI text references.
     *
     * `std::string` is an opaque display value. It is never interpreted as markup,
     * a callback, an ImGui command, or a filesystem operation.
     */
    using UiLocalizedArgumentValue = std::variant<std::int64_t, double, bool, UiLocalizedDateTime, UiLocalizedDuration,
                                                  UiLocalizedStableEnum, std::string, UiLocalizedShortcut>;

    /** @brief One named, owned, typed argument captured by a localized message reference. */
    struct UiLocalizedArgument final {
        std::string name;
        UiLocalizedArgumentValue value;
    };

    /** @brief Missing-translation behavior declared by authored Runtime UI data. */
    enum class UiLocalizedTextFailurePolicy : std::uint8_t {
        UseFallback,
        UseSafePlaceholder,
        RequireTranslation,
    };

    /** @brief Alias used by callers that refer to the source fallback directly. */
    using UiLocalizedTextFallbackPolicy = UiLocalizedTextFailurePolicy;

    /**
     * @brief Immutable owned message key, typed arguments, and source fallback.
     *
     * Construction validates and canonicalizes argument order. Reading the key,
     * fallback, or arguments is allocation-free and safe for VariableUpdate;
     * resolution against a Localization snapshot belongs to the Localization
     * boundary and is deliberately not performed here.
     */
    class UiLocalizedText final {
    public:
        /**
         * @brief Creates a complete localized message reference.
         * @param key Stable namespace/local message identity.
         * @param arguments Named typed arguments; ownership is moved and order is canonicalized.
         * @param fallbackText Bounded UTF-8 source fallback, when the policy permits one.
         * @param failurePolicy Explicit missing-translation behavior.
         * @return Owned immutable reference or a typed validation/capacity failure.
         */
        [[nodiscard]] static Result<UiLocalizedText> Create(
            UiMessageKey key, std::vector<UiLocalizedArgument> arguments, std::string fallbackText,
            UiLocalizedTextFailurePolicy failurePolicy = UiLocalizedTextFailurePolicy::UseFallback);

        /**
         * @brief Creates a message reference without named arguments.
         * @param key Stable namespace/local message identity.
         * @param fallbackText Bounded UTF-8 source fallback.
         * @param failurePolicy Explicit missing-translation behavior.
         * @return Owned immutable reference or a typed validation failure.
         */
        [[nodiscard]] static Result<UiLocalizedText> Create(
            UiMessageKey key, std::string fallbackText,
            UiLocalizedTextFailurePolicy failurePolicy = UiLocalizedTextFailurePolicy::UseFallback);

        /** @brief Checks whether the value still contains a complete validated reference. @return True when usable. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the stable message identity. @return Borrowed immutable key. */
        [[nodiscard]] const UiMessageKey &Key() const noexcept;
        /** @brief Returns canonicalized named arguments. @return Borrowed immutable bounded arguments. */
        [[nodiscard]] std::span<const UiLocalizedArgument> Arguments() const noexcept;
        /** @brief Finds one named argument without allocating. @param name Argument name. @return Borrowed value or null. */
        [[nodiscard]] const UiLocalizedArgument *FindArgument(std::string_view name) const noexcept;
        /** @brief Returns owned source fallback text. @return Borrowed UTF-8 text. */
        [[nodiscard]] std::string_view FallbackText() const noexcept;
        /** @brief Returns the declared missing-translation behavior. @return Explicit failure policy. */
        [[nodiscard]] UiLocalizedTextFailurePolicy FailurePolicy() const noexcept;

    private:
        UiLocalizedText(UiMessageKey key, std::vector<UiLocalizedArgument> arguments, std::string fallbackText,
                        UiLocalizedTextFailurePolicy failurePolicy) noexcept;

        UiMessageKey key_;
        std::vector<UiLocalizedArgument> arguments_;
        std::string fallbackText_;
        UiLocalizedTextFailurePolicy failurePolicy_{};
    };

    /** @brief Alias matching the ADR-081 message-reference terminology. */
    using LocalizedMessageRef = UiLocalizedText;
    /** @brief Short alias for the owned Runtime UI localized text value. */
    using LocalizedText = UiLocalizedText;

    /** @brief Missing-localized-asset behavior declared by the presenting UI feature. */
    enum class UiLocalizedAssetFallbackPolicy : std::uint8_t {
        Required,
        UseNeutral,
        Omit,
    };

    /** @brief One locale-specific stable asset variant. */
    struct UiLocalizedAssetVariant final {
        UiLocaleTag locale;
        Assets::AssetId asset;
        [[nodiscard]] auto operator<=>(const UiLocalizedAssetVariant &) const noexcept = default;
    };

    /** @brief Short alias for a locale-specific asset mapping. */
    using LocalizedAssetVariant = UiLocalizedAssetVariant;

    class UiLocaleFallbackChain;

    /**
     * @brief Result of selecting one declared localized asset variant.
     *
     * `matchedLocale` is borrowed from the immutable reference and is empty for a
     * neutral selection. The selection does not retain asset-provider state.
     */
    struct UiLocalizedAssetSelection final {
        Assets::AssetId asset;
        std::string_view matchedLocale;
        bool usedNeutral{};
    };

    /**
     * @brief Immutable finite locale-to-asset mapping with explicit fallback policy.
     *
     * Every variant is authored data and is included in the dependency manifest;
     * resolution only scans the caller-provided bounded Localization fallback
     * chain and never performs I/O or invents a path/substitute.
     */
    class UiLocalizedAssetReference final {
    public:
        /**
         * @brief Creates a validated localized asset variant set.
         * @param expectedType Type required for every variant and optional neutral asset.
         * @param fallbackPolicy Required, neutral, or omit behavior when no variant matches.
         * @param variants Finite locale-to-asset mappings; ownership is moved and locale order is canonicalized.
         * @param neutralAsset Optional neutral asset, required exactly for UseNeutral.
         * @return Owned reference or a typed validation/capacity/conflict failure.
         */
        [[nodiscard]] static Result<UiLocalizedAssetReference> Create(Assets::AssetTypeId expectedType,
                                                                      UiLocalizedAssetFallbackPolicy fallbackPolicy,
                                                                      std::vector<UiLocalizedAssetVariant> variants,
                                                                      std::optional<Assets::AssetId> neutralAsset = std::nullopt);

        /** @brief Checks whether this reference contains complete validated data. @return True when usable. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the expected stable asset type. @return Borrowed immutable type identity. */
        [[nodiscard]] const Assets::AssetTypeId &ExpectedType() const noexcept;
        /** @brief Returns variants in canonical locale order. @return Borrowed immutable mappings. */
        [[nodiscard]] std::span<const UiLocalizedAssetVariant> Variants() const noexcept;
        /** @brief Returns the optional authored neutral asset. @return Empty when no neutral variant is declared. */
        [[nodiscard]] std::optional<Assets::AssetId> NeutralAsset() const noexcept;
        /** @brief Returns the explicit missing-asset behavior. @return Authored fallback policy. */
        [[nodiscard]] UiLocalizedAssetFallbackPolicy FallbackPolicy() const noexcept;
        /** @brief Returns every declared variant/neutral asset in canonical dependency order. @return Borrowed manifest. */
        [[nodiscard]] std::span<const UiAssetDependency> Dependencies() const noexcept;

        /**
         * @brief Resolves against an already normalized, ordered Localization fallback chain.
         * @param localeChain Ordered requested/parent/default/source evidence from Localization.
         * @return Selected declared asset, omission, or typed unavailable/chain failure.
         * @pre The chain is immutable evidence for the same UI generation.
         * @post No allocation, I/O, locale mutation, or provider lookup occurs.
         */
        [[nodiscard]] Result<std::optional<UiLocalizedAssetSelection>> Resolve(std::span<const UiLocaleTag> localeChain) const noexcept;
        /** @brief Resolves against an owned bounded fallback chain without copying its evidence. */
        [[nodiscard]] Result<std::optional<UiLocalizedAssetSelection>> Resolve(const UiLocaleFallbackChain &localeChain) const noexcept;

    private:
        UiLocalizedAssetReference(Assets::AssetTypeId expectedType, UiLocalizedAssetFallbackPolicy fallbackPolicy,
                                  std::vector<UiLocalizedAssetVariant> variants, std::optional<Assets::AssetId> neutralAsset,
                                  std::vector<UiAssetDependency> dependencies) noexcept;

        Assets::AssetTypeId expectedType_;
        UiLocalizedAssetFallbackPolicy fallbackPolicy_{};
        std::vector<UiLocalizedAssetVariant> variants_;
        std::optional<Assets::AssetId> neutralAsset_;
        std::vector<UiAssetDependency> dependencies_;
    };

    /** @brief Alias matching the localized visual/audio reference terminology. */
    using LocalizedAssetReference = UiLocalizedAssetReference;

    /**
     * @brief Owned bounded locale fallback evidence for model-only and cook-time callers.
     *
     * The chain preserves the order supplied by Localization and rejects duplicate
     * tags. It does not infer parent locales or own the active locale policy.
     */
    class UiLocaleFallbackChain final {
    public:
        /**
         * @brief Takes ownership of a normalized, ordered locale chain.
         * @param locales Requested/parent/default/source evidence in precedence order.
         * @return Owned chain or a typed invalid/capacity failure.
         */
        [[nodiscard]] static Result<UiLocaleFallbackChain> Create(std::vector<UiLocaleTag> locales);
        /** @brief Returns the ordered locale evidence. @return Borrowed immutable bounded chain. */
        [[nodiscard]] std::span<const UiLocaleTag> Locales() const noexcept;

    private:
        explicit UiLocaleFallbackChain(std::vector<UiLocaleTag> locales) noexcept;
        std::vector<UiLocaleTag> locales_;
    };

    /** @brief Alias used by callers that treat the chain as a Localization snapshot projection. */
    using UiNormalizedLocaleFallbackChain = UiLocaleFallbackChain;
    /** @brief Short alias for the ordered locale fallback evidence. */
    using LocaleFallbackChain = UiLocaleFallbackChain;
}  // namespace Horo::Runtime::Ui
