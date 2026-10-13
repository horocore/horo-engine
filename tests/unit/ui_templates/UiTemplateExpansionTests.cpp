#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "Horo/UiTemplates/UiTemplateExpansion.h"
#include "UiTemplateTestFixtures.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::UiTemplates {
    namespace {
        using namespace Runtime::Ui;

        using TestSupport::Identity;

        UiTemplateReference Reference(const std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            Sha256Digest revision;
            revision.bytes.front() = marker;
            return {{Assets::AssetId::FromBytes(bytes)}, revision, {1, 0}};
        }

        Assets::AssetTypeId Kind() {
            return Assets::AssetTypeId::Parse("ui.panel").Value();
        }

        UiTemplateDocument Source(const std::uint8_t marker = 1) {
            const auto accepted = Reference(marker);
            UiTemplateDocument result;
            result.dependency = {accepted.asset, CurrentUiTemplateSchemaVersion, {1, 0}, accepted.revision, {}};
            result.root = Identity<UiTemplateLocalElementId>(1);
            result.elements = {{result.root, {}, Kind(), {}}, {Identity<UiTemplateLocalElementId>(2), result.root, Kind(), {}}};
            return result;
        }

        UiTemplateSchemaSnapshot Schemas() {
            return {{Kind()}, {{Kind(), Identity<UiStylePropertyId>(1), UiTemplateValueKind::Text, "text", {}}}, {}};
        }

        UiTemplateInstanceRecord Instance() {
            return {Identity<UiTemplateInstanceId>(1), Identity<UiElementId>(99), Reference(1), {}, {}};
        }

        UiTemplateSlot Slot() {
            return {Identity<UiTemplateSlotId>(1), Identity<UiTemplateLocalElementId>(2), 0, 4, {Kind()}, {}};
        }

        UiTemplateParameter TextParameter(const std::uint8_t marker, const UiTemplateLocalElementId element) {
            UiTemplateParameter parameter;
            parameter.id = Identity<UiTemplateParameterId>(marker);
            parameter.kind = UiTemplateValueKind::Text;
            parameter.targets.emplace_back(UiTemplateContentTarget{element, Identity<UiStylePropertyId>(1)});
            return parameter;
        }
    }  // namespace

    TEST_CASE("Template expansion preserves identities and canonical elements under permuted authored inputs",
              "[runtime_ui][template][headless]") {
        auto source = Source();
        source.parameters = {TextParameter(1, source.root), TextParameter(2, Identity<UiTemplateLocalElementId>(2))};
        source.slots.push_back(Slot());
        auto record = Instance();
        record.arguments = {{Identity<UiTemplateParameterId>(1), std::string{"root"}},
                            {Identity<UiTemplateParameterId>(2), std::string{"child"}}};
        record.slots = {{Identity<UiTemplateSlotId>(1),
                         {{Identity<UiElementId>(10), {}, Kind(), {{"text", std::string{"slot root"}}}, {}},
                          {Identity<UiElementId>(11), Identity<UiElementId>(10), Kind(), {}, {}}},
                         {}}};
        auto firstCatalog = UiTemplateCatalog::Create({source}, Schemas(), {});
        REQUIRE(firstCatalog.HasValue());
        const std::array occupied{record.parent};
        const auto document = Identity<UiDocumentId>(1);
        const auto first = ExpandTemplate(firstCatalog.Value(), document, record, occupied);
        REQUIRE(first.HasValue());

        std::ranges::reverse(source.elements);
        std::ranges::reverse(source.parameters);
        std::ranges::reverse(record.arguments);
        std::ranges::reverse(record.slots.front().elements);
        auto secondCatalog = UiTemplateCatalog::Create({source}, Schemas(), {});
        REQUIRE(secondCatalog.HasValue());
        const auto second = ExpandTemplate(secondCatalog.Value(), document, record, occupied);
        REQUIRE(second.HasValue());
        CHECK(second.Value().root == first.Value().root);
        CHECK(second.Value().elements == first.Value().elements);
        CHECK(second.Value().elements[2].id == Identity<UiElementId>(10));
        CHECK(second.Value().elements[3].id == Identity<UiElementId>(11));
    }

    TEST_CASE("Repeated default slot content receives distinct instance scoped identities and local references",
              "[runtime_ui][template][headless]") {
        auto source = Source();
        auto slot = Slot();
        UiReference reference;
        reference.element = Identity<UiElementId>(10);
        slot.defaultContent = {{Identity<UiElementId>(10), {}, Kind(), {}, {}},
                               {Identity<UiElementId>(11), Identity<UiElementId>(10), Kind(), {{"target", reference}}, {reference}}};
        source.slots.push_back(std::move(slot));
        auto catalog = UiTemplateCatalog::Create({source}, Schemas(), {});
        REQUIRE(catalog.HasValue());
        auto record = Instance();
        std::vector occupied{record.parent};
        const auto document = Identity<UiDocumentId>(1);
        const auto first = ExpandTemplate(catalog.Value(), document, record, occupied);
        REQUIRE(first.HasValue());
        for (const auto &element : first.Value().elements)
            occupied.push_back(element.id);
        record.id = Identity<UiTemplateInstanceId>(2);
        const auto second = ExpandTemplate(catalog.Value(), document, record, occupied);
        REQUIRE(second.HasValue());
        CHECK(first.Value().elements[2].id != second.Value().elements[2].id);
        CHECK(second.Value().elements[3].parent == second.Value().elements[2].id);
        CHECK(std::get<UiReference>(second.Value().elements[3].properties.front().value).element == second.Value().elements[2].id);
        CHECK(second.Value().elements[3].references.front().element == second.Value().elements[2].id);
    }

    TEST_CASE("Optional template targets require exact registered categories even without a supplied value",
              "[runtime_ui][template][headless][failure]") {
        auto source = Source();
        auto parameter = TextParameter(1, source.root);
        parameter.targets = {UiTemplateStyleTarget{source.root, Identity<UiStylePropertyId>(1)}};
        source.parameters = {parameter};
        CHECK(UiTemplateCatalog::Create({source}, Schemas(), {}).HasError());
        parameter.targets = {UiTemplateContentTarget{source.root, Identity<UiStylePropertyId>(2)}};
        source.parameters = {parameter};
        CHECK(UiTemplateCatalog::Create({source}, Schemas(), {}).HasError());
        parameter.targets = {UiTemplateAttachmentTarget{source.root, Identity<UiStylePropertyId>(1), UiTemplateValueKind::Text}};
        source.parameters = {parameter};
        CHECK(UiTemplateCatalog::Create({source}, Schemas(), {}).HasError());
    }

    TEST_CASE("Missing and duplicated template property targets preserve exact admission failure and accepted catalog authority",
              "[runtime_ui][template][headless][failure]") {
        auto source = Source();
        source.parameters = {TextParameter(1, source.root)};
        auto accepted = UiTemplateCatalog::Create({source}, Schemas(), {});
        REQUIRE(accepted.HasValue());
        const auto record = Instance();
        const std::array occupied{record.parent};
        const auto before = ExpandTemplate(accepted.Value(), Identity<UiDocumentId>(1), record, occupied);
        REQUIRE(before.HasValue());
        for (const bool missing : {true, false}) {
            auto malformed = source;
            if (missing)
                malformed.parameters.front().targets = {
                    UiTemplateContentTarget{Identity<UiTemplateLocalElementId>(99), Identity<UiStylePropertyId>(1)}};
            else
                malformed.parameters.push_back(TextParameter(2, source.root));
            const auto rejected = UiTemplateCatalog::Create({malformed}, Schemas(), {});
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == UiErrors::TemplateGraphInvalid.code.Value());
            CHECK(rejected.ErrorValue().message == "Template parameter property target is absent or duplicated.");
            const auto after = ExpandTemplate(accepted.Value(), Identity<UiDocumentId>(1), record, occupied);
            REQUIRE(after.HasValue());
            CHECK(after.Value().elements == before.Value().elements);
            CHECK(after.Value().root == before.Value().root);
        }
    }

    TEST_CASE("Template numeric constraints reject nonfinite bounds and defaults at catalog admission",
              "[runtime_ui][template][headless][failure]") {
        auto source = Source();
        auto parameter = TextParameter(1, source.root);
        parameter.kind = UiTemplateValueKind::Number;
        auto schemas = Schemas();
        schemas.properties.front().kind = UiTemplateValueKind::Number;
        for (const auto invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
            parameter.minimum = invalid;
            source.parameters = {parameter};
            CHECK(UiTemplateCatalog::Create({source}, schemas, {}).HasError());
            parameter.minimum.reset();
            parameter.defaultValue = invalid;
            source.parameters = {parameter};
            CHECK(UiTemplateCatalog::Create({source}, schemas, {}).HasError());
            parameter.defaultValue.reset();
        }
    }

    TEST_CASE("Template zero numeric bounds admit zero defaults and reject nonzero supplied values",
              "[runtime_ui][template][headless][failure]") {
        auto source = Source();
        auto parameter = TextParameter(1, source.root);
        parameter.kind = UiTemplateValueKind::Number;
        parameter.minimum = 0.0;
        parameter.maximum = 0.0;
        parameter.defaultValue = 0.0;
        source.parameters = {parameter};
        auto schemas = Schemas();
        schemas.properties.front().kind = UiTemplateValueKind::Number;
        auto catalog = UiTemplateCatalog::Create({source}, schemas, {});
        REQUIRE(catalog.HasValue());
        auto record = Instance();
        const std::array occupied{record.parent};
        const auto expanded = ExpandTemplate(catalog.Value(), Identity<UiDocumentId>(1), record, occupied);
        REQUIRE(expanded.HasValue());
        CHECK(std::get<double>(expanded.Value().elements.front().properties.front().value) == 0.0);
        for (const auto nonzero : {-1.0, 1.0}) {
            record.arguments = {{parameter.id, nonzero}};
            const auto rejected = ExpandTemplate(catalog.Value(), Identity<UiDocumentId>(1), record, occupied);
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == UiErrors::TemplateGraphInvalid.code.Value());
        }
    }

    TEST_CASE("Template payload and composed depth admission include recursive slot inputs and defaults",
              "[runtime_ui][template][headless][failure]") {
        auto source = Source();
        source.slots.push_back(Slot());
        UiTemplateLimits limits;
        limits.maximumPayloadBytes = 4096;
        auto catalog = UiTemplateCatalog::Create({source}, Schemas(), {}, limits);
        REQUIRE(catalog.HasValue());
        auto record = Instance();
        record.slots = {
            {Identity<UiTemplateSlotId>(1), {{Identity<UiElementId>(10), {}, Kind(), {{"text", std::string(800, 'x')}}, {}}}, {}}};
        const std::array occupied{record.parent};
        CHECK(ExpandTemplate(catalog.Value(), Identity<UiDocumentId>(1), record, occupied).HasError());
        source.slots.front().defaultContent = record.slots.front().elements;
        CHECK(UiTemplateCatalog::Create({source}, Schemas(), {}, limits).HasError());

        source = Source();
        source.nested = {{Identity<UiTemplateInstanceId>(2), Identity<UiTemplateLocalElementId>(2), Reference(2), {}, {}}};
        limits = {};
        limits.maximumDepth = 3;
        auto nestedCatalog = UiTemplateCatalog::Create({source, Source(2)}, Schemas(), {}, limits);
        REQUIRE(nestedCatalog.HasValue());
        record = Instance();
        CHECK(ExpandTemplate(nestedCatalog.Value(), Identity<UiDocumentId>(1), record, occupied).HasError());
    }
}  // namespace Horo::UiTemplates
