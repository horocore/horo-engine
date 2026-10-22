#include "UiStyleAssetValidation.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>

namespace Horo::Runtime::Ui::StyleAssetInternal {
    namespace {
        /** @brief A foreign namespace must be explicitly declared; a namespace may reference its own classes/tokens without a self edge. */
        bool Namespace(const UiStyleAssetDocument &document, const RuntimeStyleAssetId id) {
            return id == document.style.id || std::ranges::any_of(document.dependencies, [&](const auto &dependency) {
                return dependency.asset.Bytes() == id.Bytes() && dependency.expectedType.Value() == UiStyleAssetTypeName &&
                       dependency.required;
            });
        }

        /** @brief Source resource values must have both a typed dependency declaration and matching real content evidence. */
        bool Resource(const UiStyleAssetDocument &document, const UiStyleSchema &schema, const Assets::AssetId id) {
            const auto dependency = std::ranges::find(document.dependencies, id, &UiAssetDependency::asset);
            return dependency != document.dependencies.end() && dependency->required &&
                   dependency->expectedType.Value() != UiStyleAssetTypeName &&
                   std::ranges::any_of(schema.resources, [&](const auto &value) {
                return value.dependency.asset == id && value.dependency.expectedType == dependency->expectedType;
            });
        }

        /** @brief Check direct literal resources without interpreting editor or renderer types. */
        bool Literal(const UiStyleAssetDocument &document, const UiStyleSchema &schema, const UiStyleValue &value) {
            if (const auto *font = std::get_if<UiStyleTypography>(&value))
                return Resource(document, schema, font->family);
            if (const auto *image = std::get_if<UiStyleImage>(&value))
                return Resource(document, schema, image->asset);
            return true;
        }

        /** @brief Qualified aliases use their declaring namespace's own resource validation. */
        bool Source(const UiStyleAssetDocument &document, const UiStyleSchema &schema, const UiStyleValueSource &value) {
            return value.referencesToken ? Namespace(document, value.token.asset) : Literal(document, schema, value.literal);
        }

        /** @brief Validate each owned assignment without resolving another document's references under this source's authority. */
        bool Assignments(const UiStyleAssetDocument &document, const UiStyleSchema &schema, std::span<const UiStyleAssignment> values) {
            return std::ranges::all_of(values, [&](const auto &value) {
                return Source(document, schema, value.value);
            });
        }

        /** @brief State resources must be valid even when no current element selects their state. */
        bool States(const UiStyleAssetDocument &document, const UiStyleSchema &schema, std::span<const UiStyleStateOverride> values) {
            return std::ranges::all_of(values, [&](const auto &value) {
                return Assignments(document, schema, value.assignments);
            });
        }
    }  // namespace

    /** @copydoc ValidateReferences */
    Result<void> ValidateReferences(const UiStyleAssetDocument &document, const UiStyleSchema &schema) {
        if (document.defaults.typography && !Resource(document, schema, document.defaults.typography->family))
            return Result<void>::Failure(MakeError(UiErrors::DependencyInvalid));
        if (document.defaults.image && !Resource(document, schema, document.defaults.image->asset))
            return Result<void>::Failure(MakeError(UiErrors::DependencyInvalid));
        if ((document.style.base && !Namespace(document, *document.style.base)) ||
            !Assignments(document, schema, document.style.assignments) || !States(document, schema, document.style.states))
            return Result<void>::Failure(MakeError(UiErrors::DependencyInvalid));
        for (const auto &token : document.style.tokens)
            if (!Source(document, schema, token.value))
                return Result<void>::Failure(MakeError(UiErrors::DependencyInvalid));
        for (const auto &value : document.style.classes)
            if ((value.base && !Namespace(document, value.base->asset)) || !Assignments(document, schema, value.assignments) ||
                !States(document, schema, value.states))
                return Result<void>::Failure(MakeError(UiErrors::DependencyInvalid));
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui::StyleAssetInternal
