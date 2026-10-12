#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateHostTestFixture.h"

namespace Horo::Application::TemplateHostTests {
    TEST_CASE("Template host cook rejects a moved input lease without dereferencing missing capture authority", "[template][host][cook]") {
        Fixture fixture;
        auto input = fixture.Capture();
        const auto retained = std::move(input);
        const std::array abandoned{std::move(input)};
        CHECK(UiTemplateHost::Cook(fixture.owner->Capture(), abandoned, fixture.type, {}).HasError());
        const std::array valid{retained};
        CHECK(UiTemplateHost::Cook(fixture.owner->Capture(), valid, fixture.type, {}).HasValue());
    }

    TEST_CASE("Template host cook derives manifest authority from actual canonical source and retained captures",
              "[template][host][cook]") {
        Fixture fixture;
        const std::array inputs{fixture.Capture()};
        const auto cooked = UiTemplateHost::Cook(fixture.owner->Capture(), inputs, fixture.type, {});
        REQUIRE(cooked.HasValue());
        const auto again = UiTemplateHost::Cook(fixture.owner->Capture(), inputs, fixture.type, {});
        REQUIRE(again.HasValue());
        CHECK(again.Value().Document().cacheIdentity == cooked.Value().Document().cacheIdentity);
        CHECK(again.Value().Document().dependencyManifest == cooked.Value().Document().dependencyManifest);
        const auto encoded = UiTemplates::EncodeTemplateArtifact(cooked.Value().Document(), fixture.owner->Capture().Limits());
        REQUIRE(encoded.HasValue());
        CHECK(UiTemplates::DecodeTemplateArtifact(encoded.Value(), cooked.Value().Document().dependencyManifest,
                                                  fixture.owner->Capture().Limits())
                  .HasValue());
        fixture.WriteSource("later source cannot replace an accepted owned capture");
        CHECK(UiTemplateHost::Cook(fixture.owner->Capture(), inputs, fixture.type, {}).HasValue());
        const std::array replacement{fixture.Capture()};
        CHECK(UiTemplateHost::Cook(fixture.owner->Capture(), replacement, fixture.type, {}).HasError());
        fixture.owner->Shutdown();
        CHECK(UiTemplates::DecodeTemplateArtifact(encoded.Value(), cooked.Value().Document().dependencyManifest,
                                                  fixture.owner->Capture().Limits())
                  .HasValue());
    }

    TEST_CASE("Template verified manifest charges cumulative input bytes before candidate copies", "[template][host][cook]") {
        UiTemplates::UiTemplateLimits limits;
        limits.maximumPayloadBytes = 4096;
        Fixture fixture{limits};
        const auto input = fixture.Capture();
        const std::array single{input};
        REQUIRE(UiTemplateHost::Cook(fixture.owner->Capture(), single, fixture.type, {}).HasValue());
        const std::vector duplicated(128, input);
        const auto rejected = UiTemplateHost::Cook(fixture.owner->Capture(), duplicated, fixture.type, {});
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == UiTemplates::UiErrors::TemplateGraphBudgetExceeded.code.Value());
        CHECK(UiTemplateHost::Cook(fixture.owner->Capture(), single, fixture.type, {}).HasValue());
    }

    TEST_CASE("Template host rejects unverified source bytes even when a catalog supplies a plausible revision", "[template][host][cook]") {
        Fixture fixture;
        SECTION("different canonical source") {
            fixture.WriteSource(fixture.sourceBytes + " ");
        }
        SECTION("different actual type metadata") {
            const auto other = Assets::AssetTypeId::Parse("core.other_template").Value();
            fixture.Write("assets/template.uitmpl.horo", nlohmann::json{{"schemaVersion", 1},
                                                                        {"assetId", fixture.reference.asset.asset.ToString()},
                                                                        {"assetType", other.Value()}}
                                                             .dump());
            REQUIRE(fixture.registry
                        .Publish({{fixture.reference.asset.asset, other, ProjectPath::Parse("assets/template.uitmpl").Value(),
                                   ProjectPath::Parse("assets/template.uitmpl.horo").Value()}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
        }
        const std::array inputs{fixture.Capture()};
        CHECK(UiTemplateHost::Cook(fixture.owner->Capture(), inputs, fixture.type, {}).HasError());
        CHECK(fixture.owner->Capture().Source().instances.front().accepted.revision == fixture.reference.revision);
    }
}  // namespace Horo::Application::TemplateHostTests
