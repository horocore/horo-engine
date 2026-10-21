#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiStyleAsset.h"
#include "UiStyleAssetValidation.h"
#include "UiStyleValueCodec.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace Horo::Runtime::Ui {
    namespace {
        using Out = nlohmann::ordered_json;

        /** @brief Preserve the style domain's failures without choosing a host fallback. */
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Add only domain-owned schema metadata; resource defaults remain typed host policy values. */
        void Property(UiStyleSchema &schema, const UiCoreStyleProperty property, const UiStyleValue value,
                      const UiStylePropertyEffects effects, const bool inherits = false) {
            schema.properties.push_back({.id = CoreUiStylePropertyId(property),
                                         .category = UiStyleValueCategoryOf(value),
                                         .defaultValue = value,
                                         .effects = effects,
                                         .inheritsToChildren = inherits});
        }

        /** @brief Canonical fingerprint includes every byte-affecting provider rule, actual default and dependency digest. */
        Sha256Digest Fingerprint(const UiStyleSchema &schema) {
            Out properties = Out::array(), resources = Out::array();
            for (const auto &p : schema.properties)
                properties.push_back(Out::array({Assets::AssetId::FromBytes(p.id.Bytes()).ToString(), static_cast<std::uint8_t>(p.category),
                                                 StyleCodec::EncodeLiteral(p.defaultValue), p.effects.measure, p.effects.paint,
                                                 p.effects.hitTest, p.effects.accessibility, p.inheritsToChildren, p.allowsSignedDimension,
                                                 p.minimumScalar, p.maximumScalar, p.maximumEnumValue}));
            for (const auto &resource : schema.resources)
                resources.push_back(Out::array({resource.dependency.asset.ToString(), resource.dependency.expectedType.Value(),
                                                resource.dependency.required, FormatSha256(resource.digest)}));
            const auto bytes = Out{{"provider", "horo.runtime-ui.core-style"},
                                   {"version", 1},
                                   {"properties", std::move(properties)},
                                   {"resources", std::move(resources)}}
                                   .dump();
            return ComputeSha256(std::as_bytes(std::span{bytes}));
        }

        /** @brief Actual resource IDs must have one typed nonempty content commitment. */
        bool HasResource(const UiStyleSchema &schema, const Assets::AssetId id) {
            return std::ranges::find(schema.resources, id, [](const auto &resource) {
                return resource.dependency.asset;
            }) != schema.resources.end();
        }

        /** @brief Validate real default resource admission independently from ordinary properties. */
        Result<void> ResourceDefaults(const UiStyleSchema &schema) {
            if (schema.defaults.typography &&
                (!schema.defaults.typography->IsValid() || !HasResource(schema, schema.defaults.typography->family)))
                return Failure<void>(UiErrors::AssetMissing);
            if (schema.defaults.image && (!schema.defaults.image->IsValid() || !HasResource(schema, schema.defaults.image->asset)))
                return Failure<void>(UiErrors::AssetMissing);
            return Result<void>::Success();
        }

        /** @brief Reject defaults from a competing namespace policy rather than merging by source order. */
        bool CompatibleDefaults(const UiCoreStyleDefaults &values, const UiCoreStyleDefaults &selected) {
            return (!values.typography || values.typography == selected.typography) && (!values.image || values.image == selected.image);
        }

        /** @brief Resolve declared namespace dependencies separately from resource-byte evidence. */
        bool HasDependency(const UiAssetDependency &dependency, const std::span<const UiStyleAssetDocument> documents,
                           const UiStyleSchema &schema) {
            if (dependency.expectedType.Value() == UiStyleAssetTypeName)
                return std::ranges::any_of(documents, [&](const auto &document) {
                    return document.style.id.Bytes() == dependency.asset.Bytes();
                });
            return std::ranges::any_of(schema.resources, [&](const auto &resource) {
                return resource.dependency.asset == dependency.asset && resource.dependency.expectedType == dependency.expectedType;
            });
        }
    }  // namespace

    /** @copydoc CoreUiStylePropertyId */
    UiStylePropertyId CoreUiStylePropertyId(const UiCoreStyleProperty property) noexcept {
        if (property > UiCoreStyleProperty::Motion)
            return {};
        // Domain-assigned UUID namespace, published as the core property ABI; never an AssetId or a hash of a name.
        SerializedUiId bytes{0x8d, 0xc9, 0x4e, 0x71, 0x7a, 0x69, 0x4c, 0xe1, 0xa2, 0x12, 0x95, 0x3e, 0x63, 0x54, 0xb8, 0x00};
        bytes.back() = static_cast<std::uint8_t>(property) + 1;
        return UiStylePropertyId::Create(bytes).Value();
    }

    /** @copydoc CoreUiStyleSurfaceProperties */
    UiThemeSurfaceProperties CoreUiStyleSurfaceProperties() noexcept {
        return {CoreUiStylePropertyId(UiCoreStyleProperty::Width), CoreUiStylePropertyId(UiCoreStyleProperty::Height),
                CoreUiStylePropertyId(UiCoreStyleProperty::Padding), CoreUiStylePropertyId(UiCoreStyleProperty::Fill),
                CoreUiStylePropertyId(UiCoreStyleProperty::Opacity)};
    }

    /** @copydoc CreateCoreUiStyleSchema */
    Result<UiStyleSchema> CreateCoreUiStyleSchema(UiCoreStyleDefaults defaults, const std::span<const UiStyleResourceEvidence> resources) {
        if (resources.size() > MaximumUiStyleTokens)
            return Failure<UiStyleSchema>(UiErrors::CapacityExceeded);
        UiStyleSchema schema{{}, std::move(defaults), {resources.begin(), resources.end()}, {}};
        std::ranges::sort(schema.resources, {}, [](const auto &resource) {
            return resource.dependency.asset;
        });
        for (std::size_t i = 0; i < schema.resources.size(); ++i) {
            const auto &value = schema.resources[i];
            if (!value.dependency.asset.IsValid() || value.dependency.expectedType.Value().empty() || value.digest == Sha256Digest{} ||
                (i > 0 && schema.resources[i - 1].dependency.asset == value.dependency.asset))
                return Failure<UiStyleSchema>(UiErrors::DependencyInvalid);
        }
        if (const auto valid = ResourceDefaults(schema); valid.HasError())
            return Result<UiStyleSchema>::Failure(valid.ErrorValue());
        Property(schema, UiCoreStyleProperty::Width, UiStyleDimension{0}, {.measure = true, .hitTest = true, .accessibility = true});
        Property(schema, UiCoreStyleProperty::Height, UiStyleDimension{0}, {.measure = true, .hitTest = true, .accessibility = true});
        Property(schema, UiCoreStyleProperty::Padding, UiStyleDimension{0}, {.measure = true, .hitTest = true, .accessibility = true});
        Property(schema, UiCoreStyleProperty::Fill, UiStyleColor{0, 0, 0, 0, UiStyleColorRole::Surface}, {.paint = true});
        Property(schema, UiCoreStyleProperty::Opacity, UiStyleScalar{1}, {.paint = true});
        Property(schema, UiCoreStyleProperty::Shape, UiStyleShape{}, {.paint = true});
        Property(schema, UiCoreStyleProperty::ImageFit, UiStyleEnumValue{static_cast<std::uint32_t>(UiStyleImageFit::Stretch)},
                 {.paint = true});
        schema.properties.back().maximumEnumValue = static_cast<std::uint32_t>(UiStyleImageFit::Tile);
        Property(schema, UiCoreStyleProperty::Motion, UiStyleMotionReference{}, {.paint = true});
        if (schema.defaults.typography)
            Property(schema, UiCoreStyleProperty::Typography, *schema.defaults.typography,
                     {.measure = true, .paint = true, .accessibility = true}, true);
        if (schema.defaults.image)
            Property(schema, UiCoreStyleProperty::Image, *schema.defaults.image, {.paint = true});
        std::ranges::sort(schema.properties, {}, &UiStylePropertyDescriptor::id);
        schema.fingerprint = Fingerprint(schema);
        return Result<UiStyleSchema>::Success(std::move(schema));
    }

    /** @copydoc PrepareUiStyleRegistry */
    Result<RuntimeStyleRegistry> PrepareUiStyleRegistry(const std::span<const UiStyleAssetDocument> documents, const UiStyleSchema &schema,
                                                        const RuntimeStyleGeneration generation) {
        if (documents.empty() || documents.size() > MaximumUiStyleAssets)
            return Failure<RuntimeStyleRegistry>(UiErrors::CapacityExceeded);
        const auto expected = CreateCoreUiStyleSchema(schema.defaults, schema.resources);
        if (expected.HasError())
            return Result<RuntimeStyleRegistry>::Failure(expected.ErrorValue());
        if (expected.Value().fingerprint != schema.fingerprint || Fingerprint(schema) != schema.fingerprint)
            return Failure<RuntimeStyleRegistry>(UiErrors::StyleSourceStale);
        UiStyleRegistryDefinition definition{schema.properties, {}};
        for (const auto &document : documents) {
            if (!document.revision.IsValid() || !CompatibleDefaults(document.defaults, schema.defaults))
                return Failure<RuntimeStyleRegistry>(UiErrors::StyleIdentityConflict);
            for (const auto &dependency : document.dependencies)
                if (dependency.required && !HasDependency(dependency, documents, schema))
                    return Failure<RuntimeStyleRegistry>(UiErrors::AssetMissing);
            if (document.style.base && std::ranges::none_of(document.dependencies, [&](const auto &dependency) {
                return dependency.asset.Bytes() == document.style.base->Bytes() &&
                       dependency.expectedType.Value() == UiStyleAssetTypeName && dependency.required;
            }))
                return Failure<RuntimeStyleRegistry>(UiErrors::DependencyInvalid);
            if (const auto references = StyleAssetInternal::ValidateReferences(document, schema); references.HasError())
                return Result<RuntimeStyleRegistry>::Failure(references.ErrorValue());
            definition.assets.push_back(document.style);
        }
        return RuntimeStyleRegistry::Create(std::move(definition), generation);
    }
}  // namespace Horo::Runtime::Ui
