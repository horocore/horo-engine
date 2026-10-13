#include "Horo/UiTemplates/UiTemplateModel.h"
#include "UiTemplateTestFixtures.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <type_traits>

namespace Horo::UiTemplates {
    namespace {
        using TestSupport::Identity;
    }  // namespace

    TEST_CASE("Template identities are domain distinct and expansion depends only on stable lineage", "[runtime_ui][template][headless]") {
        STATIC_REQUIRE_FALSE(std::is_same_v<UiTemplateLocalElementId, Runtime::Ui::UiElementId>);
        STATIC_REQUIRE_FALSE(std::is_same_v<UiTemplateParameterId, UiTemplateSlotId>);
        STATIC_REQUIRE_FALSE(std::is_same_v<UiTemplateInstanceId, Runtime::Ui::RuntimeUiInstanceId>);
        const auto document = Identity<Runtime::Ui::UiDocumentId>(1);
        const auto local = Identity<UiTemplateLocalElementId>(2);
        const std::array lineage{Identity<UiTemplateInstanceId>(3), Identity<UiTemplateInstanceId>(4)};
        const auto first = DeriveTemplateElementId(document, lineage, local);
        REQUIRE(first.HasValue());
        REQUIRE(first.Value().IsValid());
        // This fixed vector protects the persistent v1 byte contract when its encoder is shared or refactored.
        const Runtime::Ui::SerializedUiId expected{0x85, 0x4e, 0x28, 0x2d, 0x51, 0x2b, 0xed, 0x76,
                                                   0xfb, 0xe2, 0xe3, 0x3a, 0x63, 0x13, 0x9e, 0xfd};
        CHECK(first.Value().Bytes() == expected);
        CHECK(DeriveTemplateElementId(document, lineage, local).Value() == first.Value());
        CHECK(DeriveTemplateElementId(Identity<Runtime::Ui::UiDocumentId>(5), lineage, local).Value() != first.Value());
        CHECK(DeriveTemplateElementId(document, lineage, Identity<UiTemplateLocalElementId>(6)).Value() != first.Value());
        const std::array reversed{lineage[1], lineage[0]};
        CHECK(DeriveTemplateElementId(document, reversed, local).Value() != first.Value());
        CHECK(DeriveTemplateElementId(document, std::span{lineage}.first(1), local).Value() != first.Value());
    }

    TEST_CASE("Template identity derivation rejects missing identity and oversized lineage", "[runtime_ui][template][headless]") {
        const auto document = Identity<Runtime::Ui::UiDocumentId>(1);
        const auto local = Identity<UiTemplateLocalElementId>(2);
        std::array<UiTemplateInstanceId, 33> lineage;
        lineage.fill(Identity<UiTemplateInstanceId>(3));
        CHECK(DeriveTemplateElementId(document, {}, local).HasError());
        CHECK(DeriveTemplateElementId(document, lineage, local).HasError());
        CHECK(DeriveTemplateElementId({}, std::span{lineage}.first(1), local).HasError());
        CHECK(DeriveTemplateElementId(document, std::span{lineage}.first(1), {}).HasError());
        lineage[0] = {};
        CHECK(DeriveTemplateElementId(document, std::span{lineage}.first(1), local).HasError());
    }

    TEST_CASE("Template values retain existing rich typed schemas without string encoding", "[runtime_ui][template][headless]") {
        CHECK(TemplateValueKind(UiTemplateValue{true}) == UiTemplateValueKind::Boolean);
        CHECK(TemplateValueKind(UiTemplateValue{std::int64_t{7}}) == UiTemplateValueKind::Integer);
        CHECK(TemplateValueKind(UiTemplateValue{1.5}) == UiTemplateValueKind::Number);
        CHECK(TemplateValueKind(UiTemplateValue{std::string{"label"}}) == UiTemplateValueKind::Text);
        CHECK(TemplateValueKind(UiTemplateValue{Runtime::Ui::UiReference{}}) == UiTemplateValueKind::Reference);
        CHECK(TemplateValueKind(UiTemplateValue{Runtime::Ui::UiStyleValue{}}) == UiTemplateValueKind::Style);
        STATIC_REQUIRE(std::is_same_v<std::variant_alternative_t<6, UiTemplateValue>, Runtime::Ui::LocalizedMessageRef>);
        CHECK(TemplateValueKind(UiTemplateValue{Runtime::Ui::UiBindingDescriptor{}}) == UiTemplateValueKind::Binding);
    }

    TEST_CASE("Template budgets cannot weaken compiled limits", "[runtime_ui][template][headless]") {
        const UiTemplateLimits defaults;
        REQUIRE(defaults.IsValid());
        auto limits = defaults;
        limits.maximumElements = 0;
        CHECK_FALSE(limits.IsValid());
        limits = defaults;
        limits.maximumDepth = 33;
        CHECK_FALSE(limits.IsValid());
        limits = defaults;
        limits.maximumPayloadBytes = 4 * 1024 * 1024 + 1;
        CHECK_FALSE(limits.IsValid());
        limits = defaults;
        limits.maximumUndoEntries = 17;
        CHECK_FALSE(limits.IsValid());
    }
}  // namespace Horo::UiTemplates
