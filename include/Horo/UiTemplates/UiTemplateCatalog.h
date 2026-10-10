#pragma once

/** @file UiTemplateCatalog.h
 * @brief Owned pinned template and registered property-schema snapshots for deterministic load-time expansion.
 */

#include "Horo/UiTemplates/UiTemplateModel.h"

namespace Horo::UiTemplates {
    /** @brief One registered exact element/property pair; names are codec keys, never target paths. */
    struct UiTemplatePropertySchema final {
        Assets::AssetTypeId elementType;
        Runtime::Ui::UiStylePropertyId property;
        UiTemplateValueKind kind{UiTemplateValueKind::Boolean};
        std::string documentKey; /**< Required only for ordinary document property alternatives. */
        std::optional<Runtime::Ui::UiStylePropertyDescriptor> style;
    };

    /** @brief Immutable host-validated schema inputs; no service pointer or executable contribution is retained. */
    struct UiTemplateSchemaSnapshot final {
        std::vector<Assets::AssetTypeId> elementTypes;
        std::vector<UiTemplatePropertySchema> properties;
        std::vector<Runtime::Ui::UiBindingProviderSchema> bindingProviders;
    };

    /** @brief Sole owner of complete immutable template/schema/lock inputs; not a runtime discovery service. */
    class UiTemplateCatalog final {
    public:
        /**
         * @brief Validates and owns complete pinned source documents and their registered schemas.
         * @param documents Owned source candidates, one exact revision per asset.
         * @param schemas Owned finite registered element/property/binding schema snapshot.
         * @param packages Already selected verified package lock.
         * @param limits Positive expansion and source-input bounds.
         * @return Immutable catalog or a typed metadata/schema/package/budget failure without partial publication.
         */
        [[nodiscard]] static Result<UiTemplateCatalog> Create(std::vector<UiTemplateDocument> documents, UiTemplateSchemaSnapshot schemas,
                                                              std::span<const Packages::LockedPackageReference> packages,
                                                              const UiTemplateLimits &limits = {});
        UiTemplateCatalog(UiTemplateCatalog &&) noexcept = default;
        UiTemplateCatalog &operator=(UiTemplateCatalog &&) noexcept = default;
        UiTemplateCatalog(const UiTemplateCatalog &) = delete;
        UiTemplateCatalog &operator=(const UiTemplateCatalog &) = delete;

        /** @brief Resolves exact accepted revision/interface and its complete dependency closure.
         * @param reference Pinned root reference. @return Borrowed complete document or typed admission failure.
         * @pre No concurrent catalog shutdown or move; borrowers retain the owner for the whole operation.
         */
        [[nodiscard]] Result<const UiTemplateDocument *> Resolve(const UiTemplateReference &reference) const;
        /** @brief Returns immutable registered schemas. @return Borrowed snapshot while the catalog is active. */
        [[nodiscard]] const UiTemplateSchemaSnapshot &Schemas() const noexcept;
        /** @brief Returns the copied input/output budgets. @return Immutable policy. */
        [[nodiscard]] const UiTemplateLimits &Limits() const noexcept;
        /** @brief Checks a literal against an exact registered element/property schema.
         * @param element Registered kind. @param property Registered property ID. @param value Typed candidate.
         * @return Success or a typed schema/value failure; no coercion or provider invocation occurs.
         */
        [[nodiscard]] Result<void> ValidateProperty(const Assets::AssetTypeId &element, Runtime::Ui::UiStylePropertyId property,
                                                    const UiTemplateValue &value) const;
        /** @brief Closes resolution admission; existing independently owned expansion results remain valid. */
        void Shutdown() noexcept;

    private:
        UiTemplateCatalog(std::vector<UiTemplateDocument> documents, UiTemplateSchemaSnapshot schemas, UiTemplateDependencyGraph graph,
                          UiTemplateLimits limits) noexcept;
        std::vector<UiTemplateDocument> documents_;
        UiTemplateSchemaSnapshot schemas_;
        UiTemplateDependencyGraph graph_;
        UiTemplateLimits limits_;
        bool active_{true};
    };
}  // namespace Horo::UiTemplates
