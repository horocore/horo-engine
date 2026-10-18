#include "PrefabHierarchyTestSupport.h"

namespace Horo::Editor {
    using namespace PrefabHierarchyTestSupport;

    TEST_CASE("Hierarchy source capture reads only typed source closure and fences changed bytes", "[editor][prefab][hierarchy][native]") {
        Project project;
        const std::array roots{project.asset};
        auto captured = project.Capture(project.registry.Snapshot(), roots, ScenePrefabLimits(), {});
        REQUIRE(captured.HasValue());
        CHECK(captured.Value().sources.inputs.Registry().Revision() == project.registry.Snapshot().Revision());
        REQUIRE(captured.Value().sources.inputs.Sources().size() == 1);
        CHECK(captured.Value().sources.inputs.VerifyUnchanged().HasValue());
        project.WriteSource("Changed source");
        CHECK(captured.Value().sources.inputs.VerifyUnchanged().HasError());
        const auto candidate = captured.Value().sources.resolver.Resolve(project.asset, project.instance, ScenePrefabLimits());
        REQUIRE(candidate.HasValue());
        CHECK(candidate.Value().Objects().front().object.name == "Source root");
    }

    TEST_CASE("Hierarchy source capture preserves canonical size support and rejects unreadable identities",
              "[editor][prefab][hierarchy][native]") {
        Project project;
        const std::array roots{project.asset};
        CancellationSource cancellation;
        bool accepted{};
        SECTION("larger than query capability budget") {
            std::ofstream padding(project.source, std::ios::binary | std::ios::app);
            padding << std::string(2U * 1024U * 1024U, ' ');
            padding.close();
            accepted = true;
        }
        SECTION("deleted source") {
            REQUIRE(std::filesystem::remove(project.source));
        }
        SECTION("identity mismatch") {
            std::ofstream(project.source.string() + ".horo")
                << R"({"schemaVersion":1,"assetId":"00000000-0000-0000-0000-000000000002","assetType":"core.prefab"})";
        }
        SECTION("malformed source") {
            std::ofstream(project.source) << "{";
        }
        SECTION("cancelled") {
            cancellation.RequestCancellation();
        }
        const auto result = project.Capture(project.registry.Snapshot(), roots, ScenePrefabLimits(), cancellation.Token());
        CHECK(result.HasValue() == accepted);
        CHECK(project.registry.Snapshot().Records().size() == 1);
    }

    TEST_CASE("Hierarchy source capture follows actual nested edges and retains source-qualified identities",
              "[editor][prefab][hierarchy][native]") {
        Project project;
        const auto childAsset = PrefabAsset(2).Asset();
        const auto childRecord = ScenePrefabRecord(childAsset, "nested");
        const auto child =
            ScenePrefabDocument(childAsset, {{.localId = {0}, .name = "Nested root"},
                                             {.localId = {1}, .parentLocalId = Prefab::LocalObjectId{0}, .name = "Nested member"}});
        const auto childBytes = child.SerializeCanonical().Value();
        const Prefab::PrefabSourceRevision childRevision{ScenePrefabVersion(),
                                                         ComputeSha256(std::as_bytes(std::span(childBytes.data(), childBytes.size())))};
        std::ofstream(project.directory.path / childRecord.sourcePath.String()) << childBytes;
        std::ofstream(project.directory.path / childRecord.metadataPath.String())
            << "{\"schemaVersion\":1,\"assetId\":\"" << childAsset.ToString() << "\",\"assetType\":\"core.prefab\"}";
        auto parent =
            Prefab::PrefabDocument::Create({.projectVersion = ScenePrefabVersion(),
                                            .assetId = project.asset,
                                            .objects = {{.localId = {0}, .name = "Outer root"}},
                                            .composition =
                                                Prefab::PrefabComposition{.nestedPlacements = {{.placementLocalId = {1},
                                                                                                .sourcePrefab = PrefabAsset(2),
                                                                                                .authoredAgainst = childRevision}}},
                                            .referencedAssets = {childAsset}},
                                           ScenePrefabLimits());
        REQUIRE(parent.HasValue());
        std::ofstream(project.source, std::ios::trunc) << parent.Value().SerializeCanonical().Value();
        REQUIRE(project.registry.Publish({project.record, childRecord}).status == Assets::AssetRegistryBuildStatus::Complete);
        const std::array roots{project.asset};
        auto captured = project.Capture(project.registry.Snapshot(), roots, ScenePrefabLimits(), {});
        REQUIRE(captured.HasValue());
        CHECK(captured.Value().sources.inputs.Sources().size() == 2);
        auto candidate = captured.Value().sources.resolver.Resolve(project.asset, project.instance, ScenePrefabLimits());
        REQUIRE(candidate.HasValue());
        REQUIRE(candidate.Value().Objects().size() == 3);
        CHECK(candidate.Value().Objects().back().sourcePrefab == childAsset);
        CHECK(candidate.Value().Objects().back().key.instance == project.instance);
        CHECK(candidate.Value().Objects().back().parent.has_value());
        CHECK(candidate.Value().Objects().back().key.object != candidate.Value().Objects().front().key.object);
    }
}  // namespace Horo::Editor
