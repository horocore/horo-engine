#pragma once

#include "Horo/UiTemplates/UiTemplateDocumentOwner.h"
#include "UiTemplateTestFixtures.h"

#include <array>

namespace Horo::UiTemplates::TestSupport {
    using namespace Runtime::Ui;

    inline Assets::AssetTypeId Kind() {
        return Assets::AssetTypeId::Parse("ui.panel").Value();
    }

    inline UiTemplateReference Reference(const std::uint8_t revision) {
        Sha256Digest digest;
        digest.bytes.front() = revision;
        SerializedUiId asset{};
        asset.back() = 1;
        return {{Assets::AssetId::FromBytes(asset)}, digest, {1, 0}};
    }

    inline UiTemplateParameter Parameter() {
        return {Identity<UiTemplateParameterId>(1),
                UiTemplateValueKind::Text,
                true,
                {},
                {},
                {},
                4096,
                {UiTemplateContentTarget{Identity<UiTemplateLocalElementId>(1), Identity<UiStylePropertyId>(1)}}};
    }

    inline UiTemplateDocument Source(const std::uint8_t revision) {
        const auto reference = Reference(revision);
        UiTemplateDocument source;
        source.dependency = {reference.asset, CurrentUiTemplateSchemaVersion, {1, 0}, reference.revision, {}};
        source.root = Identity<UiTemplateLocalElementId>(1);
        source.elements = {{source.root, {}, Kind(), {}}};
        source.parameters = {Parameter()};
        return source;
    }

    inline std::shared_ptr<const UiTemplateCatalog> Catalog(UiTemplateDocument source, const UiTemplateLimits &limits = {}) {
        UiTemplateSchemaSnapshot schemas{{Kind()}, {{Kind(), Identity<UiStylePropertyId>(1), UiTemplateValueKind::Text, "text", {}}}, {}};
        auto catalog = UiTemplateCatalog::Create({std::move(source)}, std::move(schemas), {}, limits);
        REQUIRE(catalog.HasValue());
        return std::make_shared<const UiTemplateCatalog>(std::move(catalog).Value());
    }

    inline UiTemplateAuthoredDocument Document() {
        const std::array elements{UiDocumentElement{Identity<UiElementId>(99), {}, Kind(), {}, {}}};
        UiTemplateInstanceRecord record{Identity<UiTemplateInstanceId>(1),
                                        Identity<UiElementId>(99),
                                        Reference(1),
                                        {{Identity<UiTemplateParameterId>(1), std::string{"authored value"}}},
                                        {}};
        return {OwnerOrdinaryDocument(elements), {std::move(record)}};
    }

    inline std::unique_ptr<UiTemplateDocumentOwner> Owner() {
        auto owner = UiTemplateDocumentOwner::Create(Document(), Catalog(Source(1)));
        REQUIRE(owner.HasValue());
        return std::move(owner).Value();
    }

    /** @brief Supplies an independently declared second instance for accepted-history tests. */
    inline UiTemplateInstanceRecord SecondInstance() {
        auto instance = Document().instances.front();
        instance.id = Identity<UiTemplateInstanceId>(2);
        return instance;
    }

    /** @brief Publishes a real second-instance transaction while retaining its detached reservation for assertions. */
    inline UiTemplatePreparedTransaction InsertAccepted(UiTemplateDocumentOwner &owner, const UiTemplateInstanceRecord &instance) {
        auto inserted = owner.PrepareInsert(instance, Catalog(Source(1)));
        REQUIRE(inserted.HasValue());
        REQUIRE(owner.Commit(inserted.Value()).HasValue());
        return std::move(inserted).Value();
    }

    /** @brief Exercises the real canonical writer and exact pinned-lease reader in one shared roundtrip fixture. */
    struct TemplateRoundTrip final {
        std::string bytes;
        std::unique_ptr<UiTemplateDocumentOwner> reopened;
    };

    inline TemplateRoundTrip RoundTrip(UiTemplateAuthoredDocument source, const std::shared_ptr<const UiTemplateCatalog> &catalog) {
        auto owner = UiTemplateDocumentOwner::Create(std::move(source), catalog);
        REQUIRE(owner.HasValue());
        auto saved = SerializeTemplateDocument(owner.Value()->Capture());
        REQUIRE(saved.HasValue());
        const std::array leases{UiTemplateCatalogLease{Reference(1), catalog}};
        auto reopened = DeserializeTemplateDocument(saved.Value(), leases, catalog, UiTemplateLimits{});
        REQUIRE(reopened.HasValue());
        return {std::move(saved).Value(), std::move(reopened).Value()};
    }

    inline UiBindingProviderSchema Provider() {
        const auto type = UiBindingProviderTypeId::Parse("game.hud.player").Value();
        const std::array properties{
            UiBindingPropertyDescriptor{.id = UiBindingPropertyId::Parse("health").Value(), .type = UiBindingValueType::FixedScalar}};
        UiBindingSchemaVersion version{1, 0, 0};
        version.fingerprint = ComputeUiBindingSchemaFingerprint(type, version, properties);
        auto schema = UiBindingProviderSchema::Create({type, ModuleId{"game.hud"}, version, properties,
                                                       UiBindingProviderScopeBit(UiBindingProviderScopeKind::GameInstance),
                                                       UiBindingProviderFlags::None});
        REQUIRE(schema.HasValue());
        return std::move(schema).Value();
    }

    inline UiBindingDescriptor Binding(const UiBindingProviderSchema &provider) {
        return {.id = Identity<UiBindingId>(9),
                .source = {provider.Type(),
                           provider.Properties().front().id,
                           {1, 0},
                           ComputeUiBindingPropertyFingerprint(provider.Properties().front())},
                .target = {Identity<UiElementId>(99), UiBindingTargetProperty::Progress},
                .fallback = UiBindingValue{0.5},
                .requirement = UiBindingRequirement::Optional};
    }

    inline LocalizedMessageRef Message() {
        auto text = UiLocalizedText::Create(UiMessageKey::Create("game", "welcome").Value(),
                                            {{"player", std::string{"Ada"}}, {"score", std::int64_t{42}}}, "Welcome");
        REQUIRE(text.HasValue());
        return std::move(text).Value();
    }

    inline std::shared_ptr<const UiTemplateCatalog> ValueCatalog(const UiTemplateValue &value) {
        auto source = Source(1);
        const auto kind = TemplateValueKind(value);
        auto &parameter = source.parameters.front();
        parameter.kind = kind;
        if (kind == UiTemplateValueKind::Style)
            parameter.targets = {UiTemplateStyleTarget{source.root, Identity<UiStylePropertyId>(1)}};
        else if (kind >= UiTemplateValueKind::LocalizedText)
            parameter.targets = {UiTemplateAttachmentTarget{source.root, Identity<UiStylePropertyId>(1), kind}};
        UiTemplatePropertySchema property{Kind(),
                                          Identity<UiStylePropertyId>(1),
                                          kind,
                                          kind < UiTemplateValueKind::Style ? "value" : "",
                                          {}};
        if (kind == UiTemplateValueKind::Style)
            property.style = UiStylePropertyDescriptor{.id = property.property, .defaultValue = UiStyleColor{}};
        UiTemplateSchemaSnapshot schemas{{Kind()}, {property}, {Provider()}};
        auto catalog = UiTemplateCatalog::Create({std::move(source)}, std::move(schemas), {});
        REQUIRE(catalog.HasValue());
        return std::make_shared<const UiTemplateCatalog>(std::move(catalog).Value());
    }

    /** @brief Complete authored rich fixture retaining its exact localization target and real provider schema. */
    inline UiTemplateAuthoredDocument RichDocument() {
        auto source = Document();
        source.instances.front().arguments.front().value = Message();
        source.localizedTexts.push_back({Identity<UiElementId>(99), Message(), Identity<UiStylePropertyId>(1)});
        source.bindings.push_back(Binding(Provider()));
        return source;
    }
}  // namespace Horo::UiTemplates::TestSupport
