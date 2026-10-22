#pragma once
/** @file UiStyleAsset.h
 * @brief Canonical versioned style assets and explicit host-owned core property schema.
 */
#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Ui/UiAssetDependency.h"
#include "Horo/Runtime/Ui/UiTheme.h"

namespace Horo::Runtime::Ui {
    inline constexpr std::string_view UiStyleAssetTypeName = "core.ui_style";
    inline constexpr std::size_t MaximumUiStyleAssetDeclarations = 4096;
    inline constexpr std::size_t MaximumUiStyleAssetBytes = 4U * 1024U * 1024U;
    /** @brief Stable core property meanings; identity is independent of array ordering or source asset identity. */
    enum class UiCoreStyleProperty : std::uint8_t {
        Width = 0,
        Height = 1,
        Padding = 2,
        Fill = 3,
        Opacity = 4,
        Typography = 5,
        Image = 6,
        Shape = 7,
        ImageFit = 8,
        Motion = 9
    };

    /** @brief Explicit selected-root policy VALUES, never property schema registrations. */
    struct UiCoreStyleDefaults final {
        std::optional<UiStyleTypography> typography;
        std::optional<UiStyleImage> image;
        [[nodiscard]] auto operator<=>(const UiCoreStyleDefaults &) const noexcept = default;
    };

    /** @brief Exact dependency content admitted by the host, bound into the schema/cook/load fingerprint. */
    struct UiStyleResourceEvidence final {
        UiAssetDependency dependency;
        Sha256Digest digest;
        [[nodiscard]] auto operator<=>(const UiStyleResourceEvidence &) const noexcept = default;
    };

    /** @brief Owned inert source namespace; id must match its registry sidecar's exact bytes. */
    struct UiStyleAssetDocument final {
        UiStyleContentRevision revision;
        UiStyleAssetDefinition style;
        UiCoreStyleDefaults defaults;
        std::vector<UiAssetDependency> dependencies;
    };

    /** @brief One explicitly selected provider and default policy for a complete style candidate. */
    struct UiStyleSchema final {
        std::vector<UiStylePropertyDescriptor> properties;
        UiCoreStyleDefaults defaults;
        std::vector<UiStyleResourceEvidence> resources;
        Sha256Digest fingerprint;
    };

    /** @brief Return the domain-assigned stable core property ID. @param property Canonical meaning. @return ID or invalid for unknown. */
    [[nodiscard]] UiStylePropertyId CoreUiStylePropertyId(UiCoreStyleProperty property) noexcept;
    /** @brief Bind actual structural consumer meanings to the same canonical core vocabulary. @return Typed surface bindings. */
    [[nodiscard]] UiThemeSurfaceProperties CoreUiStyleSurfaceProperties() noexcept;
    /** @brief Build one explicit core schema using only real supplied resource defaults.
     * @param defaults Selected root/host values. @param resources Exact typed resource digests, sorted internally.
     * @return Owned fingerprinted schema or typed missing/type/duplicate/value failure. Missing defaults omit only their resource category.
     */
    [[nodiscard]] Result<UiStyleSchema> CreateCoreUiStyleSchema(UiCoreStyleDefaults defaults,
                                                                std::span<const UiStyleResourceEvidence> resources = {});
    /** @brief Encode strict source schema 1. @param document Inert namespace. @return Canonical JSON or typed structural failure. */
    [[nodiscard]] Result<std::string> SerializeUiStyleAsset(const UiStyleAssetDocument &document);
    /** @brief Decode bounded source schema 1, rejecting duplicates and unknown fields. @param source Immutable UTF-8. @return Owned data or
     * failure. */
    [[nodiscard]] Result<UiStyleAssetDocument> DeserializeUiStyleAsset(std::string_view source);
    /** @brief Validate a complete namespace closure against one explicit schema and generation.
     * @param documents Complete owned source/cooked namespace set. @param schema Host-owned single policy.
     * @param generation Exact new registry generation. @return Actual immutable registry or original semantic failure.
     */
    [[nodiscard]] Result<RuntimeStyleRegistry> PrepareUiStyleRegistry(std::span<const UiStyleAssetDocument> documents,
                                                                      const UiStyleSchema &schema, RuntimeStyleGeneration generation);
    /** @brief Encode domain cooked payload 1, binding source identity, revision and schema fingerprint.
     * @param document Namespace. @param schema Exact policy. @return Deterministic bounded payload or failure.
     * @pre Complete closure was admitted by PrepareUiStyleRegistry; encoding alone grants no registry publication.
     */
    [[nodiscard]] Result<std::vector<std::uint8_t>> CookUiStyleAsset(const UiStyleAssetDocument &document, const UiStyleSchema &schema);
    /** @brief Decode cooked payload 1 against the exact sidecar and host policy; no source fallback.
     * @param bytes Bounded logical payload. @param expected Exact Assets identity. @param schema Exact provider/default/resource
     * fingerprint.
     * @return Owned namespace or typed malformed/stale/identity failure; closure admission remains PrepareUiStyleRegistry.
     */
    [[nodiscard]] Result<UiStyleAssetDocument> DecodeCookedUiStyleAsset(std::span<const std::uint8_t> bytes, Assets::AssetId expected,
                                                                        const UiStyleSchema &schema);
}  // namespace Horo::Runtime::Ui
