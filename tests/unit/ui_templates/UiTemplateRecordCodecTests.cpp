#include "UiTemplateOwnerTestFixture.h"

#include <nlohmann/json.hpp>

namespace Horo::UiTemplates {
    TEST_CASE("Ordinary slot codec transfers all five owned value alternatives without rich coercion", "[runtime_ui][template][headless]") {
        using namespace Runtime::Ui;
        using namespace TestSupport;
        auto templateSource = Source(1);
        templateSource.slots.push_back({Identity<UiTemplateSlotId>(1), templateSource.root, 0, 4, {Kind()}, {}});
        const auto catalog = Catalog(std::move(templateSource));
        auto source = Document();
        UiReference reference;
        reference.element = Identity<UiElementId>(99);
        source.instances.front().slots.push_back(
            {Identity<UiTemplateSlotId>(1),
             {{Identity<UiElementId>(10), {}, Kind(),
               {{"boolean", true}, {"integer", std::int64_t{-123}}, {"number", 1.75}, {"text", std::string{"owned slot text"}},
                {"reference", reference}}, {}}}, {}});
        const auto owner = UiTemplateDocumentOwner::Create(std::move(source), catalog);
        REQUIRE(owner.HasValue());
        const auto saved = SerializeTemplateDocument(owner.Value()->Capture());
        REQUIRE(saved.HasValue());
        const std::array leases{UiTemplateCatalogLease{Reference(1), catalog}};
        auto reopened = DeserializeTemplateDocument(saved.Value(), leases, catalog, UiTemplateLimits{});
        REQUIRE(reopened.HasValue());
        CHECK(SerializeTemplateDocument(reopened.Value()->Capture()).Value() == saved.Value());
        const auto &properties = reopened.Value()->Capture().Source().instances.front().slots.front().elements.front().properties;
        REQUIRE(properties.size() == 5);
        for (const auto &property : properties) {
            if (property.key == "text")
                CHECK(std::get<std::string>(property.value) == "owned slot text");
            if (property.key == "reference")
                CHECK(std::get<UiReference>(property.value).element == reference.element);
        }
        auto malformed = nlohmann::json::parse(saved.Value());
        malformed["instances"][0]["slots"][0]["elements"][0]["properties"][0]["value"]["kind"] = 5;
        CHECK(DeserializeTemplateDocument(malformed.dump(), leases, catalog, UiTemplateLimits{}).HasError());
    }
}
