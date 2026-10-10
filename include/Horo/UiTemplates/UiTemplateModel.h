#pragma once

/** @file UiTemplateModel.h
 * @brief Owned ADR-083 template schema, typed public customizations and pinned linked-instance records.
 */

#include "Horo/Runtime/Ui/UiBinding.h"
#include "Horo/Runtime/Ui/UiDocument.h"
#include "Horo/Runtime/Ui/UiStyle.h"
#include "Horo/UiTemplates/UiTemplateDependencyGraph.h"

#include <optional>
#include <variant>
#include <vector>

namespace Horo::UiTemplates {
    struct TemplateLocalElementTag;
    struct TemplateParameterTag;
    struct TemplateSlotTag;
    struct TemplateInstanceTag;
    /** @brief Stable element identity inside one template, never a document element or vector index. */
    using UiTemplateLocalElementId = Runtime::Ui::UiStableId<TemplateLocalElementTag>;
    /** @brief Stable public parameter identity, independent of display names and target ordering. */
    using UiTemplateParameterId = Runtime::Ui::UiStableId<TemplateParameterTag>;
    /** @brief Stable public content-slot identity inside one template interface. */
    using UiTemplateSlotId = Runtime::Ui::UiStableId<TemplateSlotTag>;
    /** @brief Stable linked instance identity within its owning authored document. */
    using UiTemplateInstanceId = Runtime::Ui::UiStableId<TemplateInstanceTag>;

    /** @brief Closed supported value categories; identity, parentage and executable state are excluded. */
    enum class UiTemplateValueKind : std::uint8_t {
        Boolean,
        Integer,
        Number,
        Text,
        Reference,
        Style,
        LocalizedText,
        Binding,
    };

    /** @brief Owned public parameter value using existing Horo schemas rather than encoded property paths. */
    using UiTemplateValue = std::variant<bool, std::int64_t, double, std::string, Runtime::Ui::UiReference, Runtime::Ui::UiStyleValue,
                                         Runtime::Ui::LocalizedMessageRef, Runtime::Ui::UiBindingDescriptor>;

    /** @brief Returns the exact closed category without coercion. @param value Owned typed value. @return Category. */
    [[nodiscard]] UiTemplateValueKind TemplateValueKind(const UiTemplateValue &value) noexcept;

    /**
     * @brief Derives a version-one linked element identity without allocating or consulting mutable state.
     * @param document Stable owning document identity.
     * @param lineage Non-empty stable instance lineage, outermost first.
     * @param local Stable element identity within the leaf template.
     * @return Candidate identity or an invalid-identity/depth failure; callers must collision-check before publication.
     */
    [[nodiscard]] Result<Runtime::Ui::UiElementId> DeriveTemplateElementId(Runtime::Ui::UiDocumentId document,
                                                                           std::span<const UiTemplateInstanceId> lineage,
                                                                           UiTemplateLocalElementId local);

    /** @brief Exact registered ordinary property target; the host supplies the finite element schema. */
    struct UiTemplateContentTarget final {
        UiTemplateLocalElementId element;
        Runtime::Ui::UiStylePropertyId property; /**< Stable schema identity; never a string path or C++ offset. */
    };

    /** @brief Exact registered style property target with no implicit coercion. */
    struct UiTemplateStyleTarget final {
        UiTemplateLocalElementId element;
        Runtime::Ui::UiStylePropertyId property;
    };

    /** @brief Typed localization/binding attachment to one exact template-owned element. */
    struct UiTemplateAttachmentTarget final {
        UiTemplateLocalElementId element;
        Runtime::Ui::UiStylePropertyId property;
        UiTemplateValueKind kind{UiTemplateValueKind::LocalizedText};
    };

    /** @brief Forwarding into one declared parameter of an exact nested instance lineage. */
    struct UiTemplateNestedParameterTarget final {
        std::vector<UiTemplateInstanceId> lineage;
        UiTemplateParameterId parameter;
    };

    /** @brief Closed target vocabulary; runtime handles, callbacks and arbitrary deep overrides are not representable. */
    using UiTemplateParameterTarget =
        std::variant<UiTemplateContentTarget, UiTemplateStyleTarget, UiTemplateAttachmentTarget, UiTemplateNestedParameterTarget>;

    /** @brief Exact type, required/default policy and finite constraints for a public parameter. */
    struct UiTemplateParameter final {
        UiTemplateParameterId id;
        UiTemplateValueKind kind{UiTemplateValueKind::Boolean};
        bool required{};
        std::optional<UiTemplateValue> defaultValue;
        std::optional<double> minimum; /**< Inclusive numeric constraint; absent means no additional bound. */
        std::optional<double> maximum;
        std::size_t maximumTextBytes{4096};
        std::vector<UiTemplateParameterTarget> targets;
    };

    /** @brief Owned argument identified only by the declared public parameter ID. */
    struct UiTemplateArgument final {
        UiTemplateParameterId parameter;
        UiTemplateValue value;
    };

    /** @brief Exact typed authored property; registered schemas map its identity to ordinary document representation. */
    struct UiTemplateProperty final {
        Runtime::Ui::UiStylePropertyId property;
        UiTemplateValue value;
    };

    /** @brief Template-owned element with stable local parentage and existing registered element kind. */
    struct UiTemplateElement final {
        UiTemplateLocalElementId id;
        UiTemplateLocalElementId parent; /**< Invalid only for the unique root. */
        Assets::AssetTypeId type;
        std::vector<UiTemplateProperty> properties;
    };

    struct UiTemplateSlotNestedInstance;

    /** @brief Ordinary document-owned slot content; IDs remain stable when the accepted template changes. */
    struct UiTemplateSlotContent final {
        UiTemplateSlotId slot;
        std::vector<Runtime::Ui::UiDocumentElement> elements;
        std::vector<UiTemplateSlotNestedInstance> nested;
    };

    /** @brief Linked template use inside document-owned slot content, with its own stable instance lineage. */
    struct UiTemplateSlotNestedInstance final {
        UiTemplateInstanceId id;
        Runtime::Ui::UiElementId parent;
        UiTemplateReference accepted;
        std::vector<UiTemplateArgument> arguments;
        std::vector<UiTemplateSlotContent> slots;
    };

    /** @brief Finite named slot whose content cannot replace template-owned ancestors. */
    struct UiTemplateSlot final {
        UiTemplateSlotId id;
        UiTemplateLocalElementId parent;
        std::size_t minimumChildren{};
        std::size_t maximumChildren{64};
        std::vector<Assets::AssetTypeId> allowedRootKinds;
        std::vector<Runtime::Ui::UiDocumentElement> defaultContent;
    };

    /** @brief Nested use whose stable identity extends expansion lineage before the child is resolved. */
    struct UiTemplateNestedInstance final {
        UiTemplateInstanceId id;
        UiTemplateLocalElementId parent;
        UiTemplateReference accepted;
        std::vector<UiTemplateArgument> arguments;
        std::vector<UiTemplateSlotContent> slots;
    };

    /** @brief Pinned source identity and package evidence; authored nested records alone own dependency edges. */
    struct UiTemplateMetadata final {
        UiTemplateAssetId asset;
        UiTemplateSchemaVersion schema{CurrentUiTemplateSchemaVersion};
        UiTemplateInterfaceVersion interfaceVersion;
        Sha256Digest revision;
        std::vector<UiTemplatePackageRequirement> packages;
    };

    /** @brief Complete owned source schema pinned to exact semantic revision and interface evidence. */
    struct UiTemplateDocument final {
        UiTemplateMetadata dependency;
        UiTemplateLocalElementId root;
        std::vector<UiTemplateElement> elements;
        std::vector<UiTemplateParameter> parameters;
        std::vector<UiTemplateSlot> slots;
        std::vector<UiTemplateNestedInstance> nested;
    };

    /** @brief Only durable authority for a linked subtree; no expanded element copy is stored here. */
    struct UiTemplateInstanceRecord final {
        UiTemplateInstanceId id;
        Runtime::Ui::UiElementId parent; /**< Ordinary document-owned attachment point. */
        UiTemplateReference accepted;
        std::vector<UiTemplateArgument> arguments;
        std::vector<UiTemplateSlotContent> slots;
    };

    /** @brief Positive caller budgets, enforced on source inputs and fully expanded output before publication. */
    struct UiTemplateLimits final {
        std::size_t maximumElements{4096};
        std::size_t maximumDepth{32};
        std::size_t maximumInstances{256};
        std::size_t maximumParameters{256};
        std::size_t maximumSlots{256};
        std::size_t maximumProperties{16384};
        std::size_t maximumDependencies{1024};
        std::size_t maximumPayloadBytes{4 * 1024 * 1024};
        std::size_t maximumUndoEntries{16};

        /** @brief Checks positive limits against compiled ceilings. @return Whether the policy is admissible. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Compares every admitted bound exactly; serialized policy never weakens admission by omission. */
        [[nodiscard]] bool operator==(const UiTemplateLimits &) const noexcept = default;
    };
}  // namespace Horo::UiTemplates
