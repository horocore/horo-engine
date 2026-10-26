#include "UiThemeTestFixture.h"

namespace Horo::Runtime::Ui {
    namespace {
        using namespace ThemeTest;

        TEST_CASE("Public authoring surface projection preserves typed stale and mismatched property failures",
                  "[runtime_ui][theme][authoring]") {
            auto tree = Tree();
            auto owner = Runtime();
            Frame frame{tree};
            const auto published = owner.Update(tree, frame.request);
            REQUIRE(published.HasValue());
            auto input = frame.surfaces[1];
            auto bindings = Descriptor().properties;
            auto projection = ProjectUiThemeSurface(published.Value().style, input, bindings);
            REQUIRE(projection.HasValue());
            CHECK(projection.Value().layout.style.width == UiLength::Dip(128));
            bindings.width = Property(4);
            ErrorIs(ProjectUiThemeSurface(published.Value().style, input, bindings), UiErrors::StyleTypeMismatch);
            input.style.element = {};
            const auto expected = published.Value().style.Get(input.style.element);
            REQUIRE(expected.HasError());
            const auto stale = ProjectUiThemeSurface(published.Value().style, input, Descriptor().properties);
            REQUIRE(stale.HasError());
            CHECK(stale.ErrorValue().domain.Value() == expected.ErrorValue().domain.Value());
            CHECK(stale.ErrorValue().code.Value() == expected.ErrorValue().code.Value());
            owner.Shutdown();
        }

        TEST_CASE("Explicit runtime surface bindings retain registered default dimensions", "[runtime_ui][theme][defaults]") {
            auto definition = Definition();
            for (auto &asset : definition.assets) {
                asset.assignments.clear();
                asset.classes.clear();
            }
            auto registry = RuntimeStyleRegistry::Create(std::move(definition), Rev<RuntimeStyleGeneration>());
            REQUIRE(registry.HasValue());
            auto created = UiThemeRuntime::Create(Descriptor(), std::move(registry).Value(), Theme());
            REQUIRE(created.HasValue());
            auto owner = std::move(created).Value();
            auto tree = Tree();
            Frame frame{tree};
            frame.surfaces[1].style.classes = {};
            frame.surfaces[1].layout.width = UiLength::Percent(32);
            const auto published = owner.Update(tree, frame.request);
            REQUIRE(published.HasValue());
            CHECK(published.Value().layout.Get(frame.surfaces[1].style.element).Value().arrangement.contentBox.extent.width == 0);
            owner.Shutdown();
        }

    }  // namespace
}  // namespace Horo::Runtime::Ui
