#include "../../support/AllocationProbe.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Editor/Localization/LocalizedGui.h"

#include <catch2/catch_test_macros.hpp>
#include <type_traits>

namespace {
    /** @brief Concrete surface probe exercising the shared protected resolver. */
    class ScopedSurface final : public Horo::Editor::LocalizedGui {
    public:
        ScopedSurface() : LocalizedGui("workspace.gameplay_behavior.create") {}

        using LocalizedGui::LocalizedText;
    };

    /** @brief Omitting a scope must delete even a concrete surface's default construction. */
    class UnscopedSurface final : public Horo::Editor::LocalizedGui {};

    static_assert(!std::is_default_constructible_v<UnscopedSurface>);

    /** @brief Builds a minimal scoped catalog without depending on unrelated editor copy. */
    Horo::Editor::LocalizationCatalog ScopedCatalog(const char *locale, const char *text) {
        Horo::Editor::LocalizationCatalog catalog{.locale = {locale}};
        catalog.messages.emplace(Horo::Editor::MessageKey{"editor", "workspace.gameplay_behavior.create.available"}, text);
        return catalog;
    }
}  // namespace

TEST_CASE("Scoped GUI text resolves the active locale and canonical missing identity", "[editor][localization][gui_scope]") {
    Horo::Editor::LocalizationService service{{"en-US"}};
    REQUIRE(service.RegisterCatalog(ScopedCatalog("en-US", "Available")));
    REQUIRE(service.RegisterCatalog(ScopedCatalog("tr-TR", "Kullanılabilir")));
    ScopedSurface surface;
    CHECK(surface.LocalizationScope() == "workspace.gameplay_behavior.create");
    constexpr Horo::Editor::GuiLocalizationScope original{"workspace.gameplay_behavior.create"};
    const auto copied = original;
    CHECK(copied.Value() == original.Value());
    CHECK(copied.Value().data() != original.Value().data());
    for (const char *locale : {"en-US", "tr-TR"}) {
        REQUIRE(service.Prepare({locale}));
        REQUIRE(service.ActivatePrepared());
        const auto &value = surface.LocalizedText(service, "available");
        CHECK(value == (std::string_view{locale} == "en-US" ? "Available" : "Kullanılabilir"));
        CHECK(&value == &service.Get("editor", "workspace.gameplay_behavior.create.available"));
    }
    const auto &missing = surface.LocalizedText(service, "absent");
    CHECK(missing == "[missing:editor:workspace.gameplay_behavior.create.absent]");
    CHECK(&missing == &service.Get("editor", "workspace.gameplay_behavior.create.absent"));
}

TEST_CASE("Scoped GUI lookups preserve segment boundaries and allocate no owned lookup keys", "[editor][localization][gui_scope]") {
    Horo::Editor::LocalizationService service{{"en-US"}};
    REQUIRE(service.RegisterCatalog(ScopedCatalog("en-US", "Available")));
    REQUIRE(service.Prepare({"en-US"}));
    REQUIRE(service.ActivatePrepared());
    ScopedSurface surface;
    const auto *expected = &service.Get("editor", "workspace.gameplay_behavior.create.available");
    std::size_t allocations{};
    bool sameValue = true;
    {
        Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
        for (unsigned lookup = 0; lookup < 1000; ++lookup)
            sameValue = sameValue && &surface.LocalizedText(service, "available") == expected;
        allocations = measurement.Snapshot().requests;
    }
    CHECK(sameValue);
    CHECK(allocations == 0);
    const Horo::Editor::MessageKey key{"editor", "scope.value"};
    const Horo::Editor::MessageKeyView segmented{"editor", "scope", "value"};
    CHECK(Horo::Editor::MessageKeyHash{}(key) == Horo::Editor::MessageKeyHash{}(segmented));
    CHECK(Horo::Editor::MessageKeyEqual{}(key, segmented));
    CHECK_FALSE(Horo::Editor::MessageKeyEqual{}(key, {"editor", "scop", "e.value"}));
    CHECK_FALSE(Horo::Editor::MessageKeyEqual{}(key, {"other", "scope", "value"}));
}
