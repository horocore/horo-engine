#include "UiTemplateOwnerTestFixture.h"

#include <nlohmann/json.hpp>

namespace Horo::UiTemplates {
    TEST_CASE("Template persistence preserves every typed style alternative", "[runtime_ui][template][headless]") {
        using namespace Runtime::Ui;
        using namespace TestSupport;
        const auto asset = Reference(1).asset.asset;
        const std::array<UiStyleValue, 8> styles{UiStyleColor{0.25F, 0.5F, 0.75F, 1.0F},
                                                 UiStyleDimension{128},
                                                 UiStyleTypography{.family = asset},
                                                 UiStyleImage{.asset = asset, .fit = UiStyleImageFit::Cover, .nineSlice = {1, 2, 3, 4}},
                                                 UiStyleShape{4, 2, 1, -3, 5, 6},
                                                 UiStyleScalar{0.5F},
                                                 UiStyleEnumValue{7},
                                                 UiStyleMotionReference{250000, 2}};
        for (const auto &style : styles) {
            CAPTURE(style.index());
            const auto catalog = ValueCatalog(UiTemplateValue{style});
            auto source = Document();
            source.instances.front().arguments.front().value = style;
            auto roundtrip = RoundTrip(std::move(source), catalog);
            const auto restored = roundtrip.reopened->Capture();
            CHECK(std::get<UiStyleValue>(restored.Source().instances.front().arguments.front().value) == style);
            CHECK(SerializeTemplateDocument(restored).Value() == roundtrip.bytes);

            const auto canonical = nlohmann::json::parse(roundtrip.bytes);
            const std::array leases{UiTemplateCatalogLease{Reference(1), catalog}};
            auto unknownKind = canonical;
            unknownKind["instances"][0]["arguments"][0]["value"]["value"]["kind"] = 8;
            CHECK(DeserializeTemplateDocument(unknownKind.dump(), leases, catalog, UiTemplateLimits{}).HasError());
            auto missingPayload = canonical;
            missingPayload["instances"][0]["arguments"][0]["value"]["value"].erase("value");
            CHECK(DeserializeTemplateDocument(missingPayload.dump(), leases, catalog, UiTemplateLimits{}).HasError());
            auto wrongPayload = canonical;
            wrongPayload["instances"][0]["arguments"][0]["value"]["value"]["value"] = "not a typed style literal";
            CHECK(DeserializeTemplateDocument(wrongPayload.dump(), leases, catalog, UiTemplateLimits{}).HasError());
        }
    }

    TEST_CASE("Template persistence rejects malformed compound style tuples atomically", "[runtime_ui][template][headless]") {
        using namespace Runtime::Ui;
        using namespace TestSupport;
        const auto asset = Reference(1).asset.asset;
        const std::array<UiStyleValue, 5> styles{UiStyleColor{}, UiStyleTypography{.family = asset}, UiStyleImage{.asset = asset},
                                                 UiStyleShape{}, UiStyleMotionReference{1000, 1}};
        for (const auto &style : styles) {
            CAPTURE(style.index());
            const auto catalog = ValueCatalog(UiTemplateValue{style});
            auto source = Document();
            source.instances.front().arguments.front().value = style;
            auto roundtrip = RoundTrip(std::move(source), catalog);
            const auto canonical = nlohmann::json::parse(roundtrip.bytes);
            const std::array leases{UiTemplateCatalogLease{Reference(1), catalog}};
            auto truncated = canonical;
            auto &tuple = truncated["instances"][0]["arguments"][0]["value"]["value"]["value"];
            REQUIRE(tuple.is_array());
            tuple.erase(tuple.size() - 1);
            CHECK(DeserializeTemplateDocument(truncated.dump(), leases, catalog, UiTemplateLimits{}).HasError());
            auto oversized = canonical;
            oversized["instances"][0]["arguments"][0]["value"]["value"]["value"].push_back(0);
            CHECK(DeserializeTemplateDocument(oversized.dump(), leases, catalog, UiTemplateLimits{}).HasError());
            auto invalidLiteral = canonical;
            auto &payload = invalidLiteral["instances"][0]["arguments"][0]["value"]["value"]["value"];
            switch (UiStyleValueCategoryOf(style)) {
                case UiStyleValueCategory::Color:
                    payload[0] = 2.0;
                    break;
                case UiStyleValueCategory::Typography:
                    payload[1] = 1001;
                    break;
                case UiStyleValueCategory::Imagery:
                    payload[2][0] = -1;
                    break;
                case UiStyleValueCategory::Shape:
                    payload[0] = -1;
                    break;
                case UiStyleValueCategory::Motion:
                    payload[1] = 65536;
                    break;
                default:
                    FAIL("Compound style fixture must identify a validated tuple category");
            }
            CHECK(DeserializeTemplateDocument(invalidLiteral.dump(), leases, catalog, UiTemplateLimits{}).HasError());
            CHECK(SerializeTemplateDocument(roundtrip.reopened->Capture()).Value() == roundtrip.bytes);
        }
    }

    TEST_CASE("Ordinary slot codec transfers all five owned value alternatives without rich coercion", "[runtime_ui][template][headless]") {
        using namespace Runtime::Ui;
        using namespace TestSupport;
        auto templateSource = Source(1);
        templateSource.slots.push_back({Identity<UiTemplateSlotId>(1), templateSource.root, 0, 4, {Kind()}, {}});
        const auto catalog = Catalog(std::move(templateSource));
        auto source = Document();
        UiReference reference;
        reference.element = Identity<UiElementId>(99);
        source.instances.front().slots.push_back({Identity<UiTemplateSlotId>(1),
                                                  {{Identity<UiElementId>(10),
                                                    {},
                                                    Kind(),
                                                    {{"boolean", true},
                                                     {"integer", std::int64_t{-123}},
                                                     {"number", 1.75},
                                                     {"text", std::string{"owned slot text"}},
                                                     {"reference", reference}},
                                                    {}}},
                                                  {}});
        auto roundtrip = RoundTrip(std::move(source), catalog);
        CHECK(SerializeTemplateDocument(roundtrip.reopened->Capture()).Value() == roundtrip.bytes);
        const auto &properties = roundtrip.reopened->Capture().Source().instances.front().slots.front().elements.front().properties;
        REQUIRE(properties.size() == 5);
        for (const auto &property : properties) {
            if (property.key == "text")
                CHECK(std::get<std::string>(property.value) == "owned slot text");
            if (property.key == "reference")
                CHECK(std::get<UiReference>(property.value).element == reference.element);
        }
        auto malformed = nlohmann::json::parse(roundtrip.bytes);
        malformed["instances"][0]["slots"][0]["elements"][0]["properties"][0]["value"]["kind"] = 5;
        const std::array leases{UiTemplateCatalogLease{Reference(1), catalog}};
        CHECK(DeserializeTemplateDocument(malformed.dump(), leases, catalog, UiTemplateLimits{}).HasError());
    }
}  // namespace Horo::UiTemplates
