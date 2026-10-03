#include "Horo/UiTemplates/UiTemplateDependencyGraph.h"
#include "Horo/UiTemplates/UiTemplateErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <utility>
#include <vector>

namespace Horo::UiTemplates {
    namespace {
        UiTemplateAssetId Asset(const std::uint8_t marker) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = marker;
            return {Assets::AssetId::FromBytes(bytes)};
        }

        Sha256Digest Digest(const std::uint8_t marker) {
            Sha256Digest digest;
            digest.bytes.front() = marker;
            return digest;
        }

        UiTemplateReference Ref(const std::uint8_t marker) {
            return {Asset(marker), Digest(marker), {1, 0}};
        }

        UiTemplateDependencyDescriptor Node(const std::uint8_t marker, std::vector<UiTemplateReference> nested = {}) {
            return {.asset = Asset(marker),
                    .schema = CurrentUiTemplateSchemaVersion,
                    .interfaceVersion = {1, 1},
                    .revision = Digest(marker),
                    .nested = std::move(nested)};
        }

        Packages::HoroPackageId Package(const char *name) {
            return Packages::HoroPackageId::Parse(name).Value();
        }

        Packages::PackageVersion Version(const char *version) {
            return Packages::PackageVersion::Parse(version).Value();
        }

        UiTemplatePackageRequirement RequirePackage(const char *name, const char *version) {
            return {Package(name), {Packages::PackageVersionRange::Kind::Exact, Version(version)}};
        }

        template <typename T> void ExpectError(const Result<T> &result, const ErrorCodeDescriptor &code) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == code.code.Value());
        }
    }  // namespace

    TEST_CASE("Template graph resolves a repeated nested DAG and pinned packages in canonical order",
              "[runtime_ui][template][dependency]") {
        auto root = Node(1, {Ref(3), Ref(2)});
        auto left = Node(2, {Ref(4)});
        auto right = Node(3, {Ref(4)});
        left.packages.push_back(RequirePackage("com.example.zed", "2.0.0"));
        right.packages.push_back(RequirePackage("com.example.alpha", "1.0.0"));
        const std::vector descriptors{right, root, Node(4), left};
        const std::vector<Packages::LockedPackageReference> locked{{Package("com.example.zed"), Version("2.0.0")},
                                                                   {Package("com.example.alpha"), Version("1.0.0")}};
        auto graphResult = UiTemplateDependencyGraph::Create(descriptors, locked);
        REQUIRE(graphResult.HasValue());
        auto graph = std::move(graphResult).Value();
        const auto resolved = graph.Resolve(Ref(1));
        REQUIRE(resolved.HasValue());
        REQUIRE(resolved.Value().templates == std::vector<UiTemplateAssetId>{Asset(4), Asset(2), Asset(3), Asset(1)});
        REQUIRE(resolved.Value().packages.size() == 2);
        REQUIRE(resolved.Value().packages[0].package == Package("com.example.alpha"));
        REQUIRE(resolved.Value().packages[1].package == Package("com.example.zed"));

        root.nested.clear();
        left.packages.clear();
        REQUIRE(graph.Resolve(Ref(1)).HasValue());
        graph.Shutdown();
        graph.Shutdown();
        REQUIRE(resolved.Value().templates.size() == 4);
        ExpectError(graph.Resolve(Ref(1)), UiErrors::TemplateGraphShutdown);
    }

    TEST_CASE("Template graph rejects cycles by asset identity and missing or stale revisions",
              "[runtime_ui][template][dependency][failure]") {
        const std::vector cycle{Node(1, {Ref(2)}), Node(2, {Ref(1)})};
        auto graph = UiTemplateDependencyGraph::Create(cycle, {});
        REQUIRE(graph.HasValue());
        ExpectError(graph.Value().Resolve(Ref(1)), UiErrors::TemplateDependencyCycle);

        auto changedRevision = Ref(1);
        changedRevision.revision = Digest(8);
        const std::vector revisionCycle{Node(1, {Ref(2)}), Node(2, {changedRevision})};
        auto revisionGraph = UiTemplateDependencyGraph::Create(revisionCycle, {});
        REQUIRE(revisionGraph.HasValue());
        ExpectError(revisionGraph.Value().Resolve(Ref(1)), UiErrors::TemplateDependencyCycle);

        const std::vector missing{Node(1, {Ref(9)})};
        auto missingGraph = UiTemplateDependencyGraph::Create(missing, {});
        REQUIRE(missingGraph.HasValue());
        ExpectError(missingGraph.Value().Resolve(Ref(1)), UiErrors::TemplateMissing);

        auto stale = Ref(1);
        stale.revision = Digest(8);
        ExpectError(missingGraph.Value().Resolve(stale), UiErrors::TemplateRevisionUnavailable);
        stale = Ref(1);
        stale.minimumInterface = {2, 0};
        ExpectError(missingGraph.Value().Resolve(stale), UiErrors::TemplateVersionIncompatible);
    }

    TEST_CASE("Template graph rejects missing or incompatible pinned package versions", "[runtime_ui][template][package]") {
        auto root = Node(1);
        root.packages.push_back(RequirePackage("com.example.ui", "2.1.0"));
        const std::vector descriptors{root};
        auto absent = UiTemplateDependencyGraph::Create(descriptors, {});
        REQUIRE(absent.HasValue());
        ExpectError(absent.Value().Resolve(Ref(1)), UiErrors::TemplatePackageUnavailable);

        const std::vector<Packages::LockedPackageReference> oldLock{{Package("com.example.ui"), Version("2.0.0")}};
        auto incompatible = UiTemplateDependencyGraph::Create(descriptors, oldLock);
        REQUIRE(incompatible.HasValue());
        ExpectError(incompatible.Value().Resolve(Ref(1)), UiErrors::TemplatePackageUnavailable);

        const std::vector<Packages::LockedPackageReference> currentLock{{Package("com.example.ui"), Version("2.1.0")}};
        auto reloaded = UiTemplateDependencyGraph::Create(descriptors, currentLock);
        REQUIRE(reloaded.HasValue());
        REQUIRE(reloaded.Value().Resolve(Ref(1)).HasValue());
    }

    TEST_CASE("Template graph rejects malformed snapshots and traversal limits without partial results",
              "[runtime_ui][template][dependency][failure]") {
        const std::vector duplicate{Node(1), Node(1)};
        ExpectError(UiTemplateDependencyGraph::Create(duplicate, {}), UiErrors::TemplateGraphInvalid);
        auto unsupported = Node(1);
        unsupported.schema = {2, 0};
        const std::vector unsupportedCatalog{unsupported};
        ExpectError(UiTemplateDependencyGraph::Create(unsupportedCatalog, {}), UiErrors::TemplateVersionIncompatible);
        const std::vector malformed{Node(1, {UiTemplateReference{}})};
        ExpectError(UiTemplateDependencyGraph::Create(malformed, {}), UiErrors::TemplateGraphInvalid);

        const std::vector chain{Node(1, {Ref(2)}), Node(2)};
        UiTemplateGraphLimits limits;
        limits.maximumDepth = 1;
        auto shallow = UiTemplateDependencyGraph::Create(chain, {}, limits);
        REQUIRE(shallow.HasValue());
        ExpectError(shallow.Value().Resolve(Ref(1)), UiErrors::TemplateGraphBudgetExceeded);
        limits.maximumDepth = 2;
        limits.maximumTemplates = 1;
        ExpectError(UiTemplateDependencyGraph::Create(chain, {}, limits), UiErrors::TemplateGraphBudgetExceeded);
        limits.maximumTemplates = 2;
        limits.maximumEdges = 1;
        auto bounded = UiTemplateDependencyGraph::Create(chain, {}, limits);
        REQUIRE(bounded.HasValue());
        REQUIRE(bounded.Value().Resolve(Ref(1)).HasValue());

        auto packageHeavy = Node(1);
        packageHeavy.packages.push_back(RequirePackage("com.example.one", "1.0.0"));
        packageHeavy.packages.push_back(RequirePackage("com.example.two", "1.0.0"));
        const std::vector packageCatalog{packageHeavy};
        ExpectError(UiTemplateDependencyGraph::Create(packageCatalog, {}, limits), UiErrors::TemplateGraphBudgetExceeded);
    }

    TEST_CASE("Template graph depth limit includes descendants of a previously resolved shared node",
              "[runtime_ui][template][dependency][budget]") {
        const std::vector diamond{Node(1, {Ref(2), Ref(3)}), Node(2, {Ref(4)}), Node(3, {Ref(5)}),
                                  Node(4, {Ref(6)}),         Node(5, {Ref(4)}), Node(6)};
        UiTemplateGraphLimits limits;
        limits.maximumDepth = 4;
        auto graph = UiTemplateDependencyGraph::Create(diamond, {}, limits);
        REQUIRE(graph.HasValue());
        ExpectError(graph.Value().Resolve(Ref(1)), UiErrors::TemplateGraphBudgetExceeded);
        limits.maximumDepth = 5;
        auto deeperGraph = UiTemplateDependencyGraph::Create(diamond, {}, limits);
        REQUIRE(deeperGraph.HasValue());
        REQUIRE(deeperGraph.Value().Resolve(Ref(1)).HasValue());
    }
}  // namespace Horo::UiTemplates
