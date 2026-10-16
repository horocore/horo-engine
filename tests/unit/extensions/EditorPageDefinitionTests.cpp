#include "Horo/Extensions/EditorPageDefinition.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <type_traits>

namespace Horo::Extensions::Tests {
    namespace {
        /** @brief Builds a canonical full-int64 field whose UI projection deliberately contains no lossy double hints. */
        EditorPageField IntegerField() {
            EditorPageField field;
            field.binding = {"page.count"};
            field.setting = {SettingKey{"page.count"},
                             SettingValueType::Integer,
                             std::int64_t{0},
                             SettingScope::User,
                             ReloadPolicy::NextOperation,
                             SettingSensitivity::Public,
                             {}};
            return field;
        }

        /** @brief Supplies the exact transient settings metadata used by all profile rejection regressions. */
        EditorSurfaceDescriptor SettingsSurface() {
            return EditorSurfaceDescriptor{.id = "fixture.settings",
                                           .kind = EditorSurfaceKind::SettingsPage,
                                           .labelLocalizationKey = "fixture.title",
                                           .placement = {EditorSurfacePlacementKind::Settings, {}, 0},
                                           .persistence = EditorSurfacePersistence::None,
                                           .provider = {"fixture.package", "fixture.module", 1}};
        }

        /** @brief Couples the canonical full-int64 field with its admitted localized standard primitive. */
        EditorPageDefinition Definition(const EditorSurfaceDescriptor &surface) {
            EditorPageDefinition definition;
            definition.form.id = {surface.id};
            definition.form.title = {EditorUiTextKind::LocalizationKey, "fixture.title"};
            definition.fields.push_back(IntegerField());
            definition.messages = {{"en-US", "fixture.title", "Settings"}, {"en-US", "fixture.count", "Count"}};
            definition.form.nodes.push_back(
                {EditorUiNumberNode{.base = {.id = {"fixture.number"}, .label = {EditorUiTextKind::LocalizationKey, "fixture.count"}},
                                    .binding = {"page.count"},
                                    .numberKind = EditorUiNumberKind::Integer,
                                    .value = std::int64_t{0}}});
            return definition;
        }
    }  // namespace

    TEST_CASE("Extension page decimal editor preserves every signed64bit endpoint and integers above double precision",
              "[Extensions][EditorPage][Integer]") {
        for (const auto value : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-9007199254740993LL}, std::int64_t{0},
                                 std::int64_t{9007199254740993LL}, std::numeric_limits<std::int64_t>::max()}) {
            const auto encoded = std::to_string(value);
            const auto parsed = ParseEditorPageInteger(encoded);
            REQUIRE(parsed.HasValue());
            CHECK(parsed.Value() == value);
            CHECK(std::to_string(parsed.Value()) == encoded);
        }
        for (const auto input : {"9223372036854775808", "-9223372036854775809", "1.0", "1e3", " 1", "1 ", "", "+1", "--1"})
            CHECK(ParseEditorPageInteger(input).HasError());
    }

    TEST_CASE("Extension page exact constraints reject rather than clamp or coerce", "[Extensions][EditorPage][Validation]") {
        auto field = IntegerField();
        field.minimum = -9007199254740993LL;
        field.maximum = 9007199254740993LL;
        CHECK(ValidateEditorPageValue(field, std::int64_t{-9007199254740993LL}).HasValue());
        CHECK(ValidateEditorPageValue(field, std::int64_t{9007199254740993LL}).HasValue());
        CHECK(ValidateEditorPageValue(field, std::int64_t{9007199254740994LL}).HasError());
        static_assert(!std::is_constructible_v<SettingValue, double>);
        CHECK(ValidateEditorPageValue(field, std::string{"1"}).HasError());
        CHECK(ValidateEditorPageValue(field, true).HasError());
    }

    TEST_CASE("Extension settings definition binds exactly one canonical field to its shared form primitive",
              "[Extensions][EditorPage][Definition]") {
        auto surface = SettingsSurface();
        auto definition = Definition(surface);
        REQUIRE(ValidateEditorPageDefinition(surface, definition).HasValue());
        SECTION("duplicate field") {
            definition.fields.push_back(definition.fields.front());
        }
        SECTION("lossy decimal UI") {
            std::get<EditorUiNumberNode>(definition.form.nodes.front().payload).numberKind = EditorUiNumberKind::Decimal;
        }
        SECTION("competing double range") {
            std::get<EditorUiNumberNode>(definition.form.nodes.front().payload).maximum = 9007199254740992.0;
        }
        SECTION("missing fallback") {
            definition.messages.erase(definition.messages.begin());
        }
        SECTION("transient state persistence") {
            surface.persistence = EditorSurfacePersistence::Workspace;
        }
        SECTION("unsupported row layout") {
            definition.form.nodes.push_back({EditorUiContainerNode{.base = {.id = {"row"}}, .layout = EditorUiLayoutKind::Row}});
        }
        SECTION("unsupported grid layout") {
            definition.form.nodes.push_back({EditorUiContainerNode{.base = {.id = {"grid"}}, .layout = EditorUiLayoutKind::Grid}});
        }
        SECTION("unsupported path") {
            definition.form.nodes.push_back({EditorUiPathNode{.base = {.id = {"path"}}, .binding = {"page.path"}}});
        }
        SECTION("unsupported choice") {
            definition.form.nodes.push_back({EditorUiChoiceNode{.base = {.id = {"choice"}}, .binding = {"page.choice"}}});
        }
        SECTION("unsupported interaction profile") {
            std::get<EditorUiNumberNode>(definition.form.nodes.front().payload).base.focusPolicy = EditorUiFocusPolicy::Never;
        }
        SECTION("foreign page domain") {
            definition.fields.front().setting.scope = SettingScope::Session;
        }
        CHECK(ValidateEditorPageDefinition(surface, definition).HasError());
    }

    TEST_CASE("Page boolean and bounded string settings retain canonical types and reject invalid choices",
              "[Extensions][EditorPage][Validation]") {
        auto field = IntegerField();
        field.minimum.reset();
        field.maximum.reset();
        field.setting.type = SettingValueType::Boolean;
        field.setting.defaultValue = false;
        CHECK(ValidateEditorPageValue(field, true).HasValue());
        CHECK(ValidateEditorPageValue(field, std::int64_t{1}).HasError());
        field.setting.type = SettingValueType::String;
        field.setting.defaultValue = std::string{"one"};
        field.maximumStringBytes = 3;
        field.choices = {"one", "two"};
        CHECK(ValidateEditorPageValue(field, std::string{"two"}).HasValue());
        CHECK(ValidateEditorPageValue(field, std::string{"other"}).HasError());
        CHECK(ValidateEditorPageValue(field, std::string{"a\0b", 3}).HasError());
        field.choices.clear();
        CHECK(ValidateEditorPageValue(field, std::string{"\xC0\xAF", 2}).HasError());
        CHECK(ValidateEditorPageValue(field, std::string{"ç"}).HasValue());
        field.choices = {"one", "two"};
        field.choices.push_back("one");
        CHECK(ValidateEditorPageValue(field, std::string{"one"}).HasError());
    }
}  // namespace Horo::Extensions::Tests
